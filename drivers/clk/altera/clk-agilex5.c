// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Intel Corporation <www.intel.com>
 * Copyright (C) 2025 Altera Corporation <www.altera.com>
 */

#include <config.h>
#include <log.h>
#include <dm.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <vsprintf.h>
#include <asm/global_data.h>
#include <asm/io.h>
#include <asm/system.h>
#include <dm/lists.h>
#include <dm/util.h>
#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/types.h>
#include <asm/arch/clock_manager.h>
#include <dt-bindings/clock/agilex5-clock.h>
#include <wait_bit.h>
#include <clk-uclass.h>

DECLARE_GLOBAL_DATA_PTR;

#define CLKMGR_CTRL_SWCTRLBTCLKEN_MASK		BIT(8)
#define CLKMGR_CTRL_SWCTRLBTCLKSEL_MASK		BIT(9)

struct socfpga_clk_plat {
	void __iomem *regs;
};

/*
 * function to write the bypass register which requires a poll of the
 * busy bit
 */
static void clk_write_bypass_mainpll(struct socfpga_clk_plat *plat, u32 val)
{
	uintptr_t base_addr = (uintptr_t)plat->regs;

	CM_REG_WRITEL(plat, val, CLKMGR_MAINPLL_BYPASS);
	wait_for_bit_le32((const void *)(base_addr + CLKMGR_STAT), CLKMGR_STAT_BUSY,
			  false, 20000, false);
}

static void clk_write_bypass_perpll(struct socfpga_clk_plat *plat, u32 val)
{
	uintptr_t base_addr = (uintptr_t)plat->regs;

	CM_REG_WRITEL(plat, val, CLKMGR_PERPLL_BYPASS);
	wait_for_bit_le32((const void *)(base_addr + CLKMGR_STAT), CLKMGR_STAT_BUSY,
			  false, 20000, false);
}

/* function to write the ctrl register which requires a poll of the busy bit */
static void clk_write_ctrl(struct socfpga_clk_plat *plat, u32 val)
{
	uintptr_t base_addr = (uintptr_t)plat->regs;

	CM_REG_WRITEL(plat, val, CLKMGR_CTRL);
	wait_for_bit_le32((const void *)(base_addr + CLKMGR_STAT), CLKMGR_STAT_BUSY,
			  false, 20000, false);
}

static const struct {
	u32 reg;
	u32 val;
	u32 mask;
} membus_pll[] = {
	{
		MEMBUS_SYNTHCALFOSC_INIT_CENTERFREQ_REG,
		/*
		 * BIT[0:0]
		 * Sets synthcalfosc_init_centerfreq=1 to limit overshoot
		 * frequency during lock
		 */
		BIT(0),
		BIT(0)
	},
	{
		MEMBUS_SYNTHPPM_WATCHDOGTMR_VF01_REG,
		/*
		 * BIT[0:0]
		 * Sets synthppm_watchdogtmr_vf0=1 to give the pll more time
		 * to settle before lock is asserted.
		 */
		BIT(0),
		BIT(0)
	},
	{
		MEMBUS_CALCLKSLICE0_DUTY_LOCOVR_REG,
		/*
		 * BIT[6:0]
		 * Centering duty cycle for clkslice0 output
		 */
		0x4a,
		GENMASK(6, 0)
	},
	{
		MEMBUS_CALCLKSLICE1_DUTY_LOCOVR_REG,
		/*
		 * BIT[6:0]
		 * Centering duty cycle for clkslice1 output
		 */
		0x4a,
		GENMASK(6, 0)
	},
};

static int membus_wait_for_req(struct socfpga_clk_plat *plat, u32 pll,
			       int timeout)
{
	int cnt = 0;
	u32 req_status;

	if (pll == MEMBUS_MAINPLL)
		req_status = CM_REG_READL(plat, CLKMGR_MAINPLL_MEM);
	else
		req_status = CM_REG_READL(plat, CLKMGR_PERPLL_MEM);

	while ((cnt < timeout) && (req_status & CLKMGR_MEM_REQ_SET_MSK)) {
		if (pll == MEMBUS_MAINPLL)
			req_status = CM_REG_READL(plat, CLKMGR_MAINPLL_MEM);
		else
			req_status = CM_REG_READL(plat, CLKMGR_PERPLL_MEM);
		cnt++;
	}

	if (cnt >= timeout)
		return -ETIMEDOUT;

	return 0;
}

static int membus_write_pll(struct socfpga_clk_plat *plat, u32 pll,
			    u32 addr_offset, u32 wdat, int timeout)
{
	u32 addr;
	u32 val;

	addr = ((addr_offset | CLKMGR_MEM_ADDR_START) & CLKMGR_MEM_ADDR_MASK);

	val = (CLKMGR_MEM_REQ_SET_MSK | CLKMGR_MEM_WR_SET_MSK |
	       (wdat << CLKMGR_MEM_WDAT_LSB_OFFSET) | addr);

	if (pll == MEMBUS_MAINPLL)
		CM_REG_WRITEL(plat, val, CLKMGR_MAINPLL_MEM);
	else
		CM_REG_WRITEL(plat, val, CLKMGR_PERPLL_MEM);

	debug("MEMBUS: Write 0x%08x to addr = 0x%08x\n", wdat, addr);

	return membus_wait_for_req(plat, pll, timeout);
}

static int membus_read_pll(struct socfpga_clk_plat *plat, u32 pll,
			   u32 addr_offset, u32 *rdata, int timeout)
{
	u32 addr;
	u32 val;

	addr = ((addr_offset | CLKMGR_MEM_ADDR_START) & CLKMGR_MEM_ADDR_MASK);

	val = ((CLKMGR_MEM_REQ_SET_MSK & ~CLKMGR_MEM_WR_SET_MSK) | addr);

	if (pll == MEMBUS_MAINPLL)
		CM_REG_WRITEL(plat, val, CLKMGR_MAINPLL_MEM);
	else
		CM_REG_WRITEL(plat, val, CLKMGR_PERPLL_MEM);

	*rdata = 0;

	if (membus_wait_for_req(plat, pll, timeout))
		return -ETIMEDOUT;

	if (pll == MEMBUS_MAINPLL)
		*rdata = CM_REG_READL(plat, CLKMGR_MAINPLL_MEMSTAT);
	else
		*rdata = CM_REG_READL(plat, CLKMGR_PERPLL_MEMSTAT);

	debug("MEMBUS: Read 0x%08x from addr = 0x%08x\n", *rdata, addr);

	return 0;
}

static void membus_pll_configs(struct socfpga_clk_plat *plat, u32 pll)
{
	int i;
	u32 rdata;

	for (i = 0; i < ARRAY_SIZE(membus_pll); i++) {
		membus_read_pll(plat, pll, membus_pll[i].reg,
				&rdata, MEMBUS_TIMEOUT);
		membus_write_pll(plat, pll, membus_pll[i].reg,
						 ((rdata & ~membus_pll[i].mask) |
						   membus_pll[i].val),
						 MEMBUS_TIMEOUT);
	}
}

static u32 calc_vocalib_pll(u32 pllm, u32 pllglob)
{
	u32 mdiv, refclkdiv, arefclkdiv, drefclkdiv, mscnt, hscnt, vcocalib;

	mdiv = pllm & CLKMGR_PLLM_MDIV_MASK;
	arefclkdiv = (pllglob & CLKMGR_PLLGLOB_AREFCLKDIV_MASK) >>
		      CLKMGR_PLLGLOB_AREFCLKDIV_OFFSET;
	drefclkdiv = (pllglob & CLKMGR_PLLGLOB_DREFCLKDIV_MASK) >>
		      CLKMGR_PLLGLOB_DREFCLKDIV_OFFSET;
	refclkdiv = (pllglob & CLKMGR_PLLGLOB_REFCLKDIV_MASK) >>
		     CLKMGR_PLLGLOB_REFCLKDIV_OFFSET;
	mscnt = CLKMGR_VCOCALIB_MSCNT_CONST / (mdiv * BIT(drefclkdiv));
	if (!mscnt)
		mscnt = 1;
	hscnt = (mdiv * mscnt * BIT(drefclkdiv) / refclkdiv) -
		CLKMGR_VCOCALIB_HSCNT_CONST;
	vcocalib = (hscnt & CLKMGR_VCOCALIB_HSCNT_MASK) |
		   ((mscnt << CLKMGR_VCOCALIB_MSCNT_OFFSET) &
		     CLKMGR_VCOCALIB_MSCNT_MASK);

	/* Dump all the pll calibration settings for debug purposes */
	debug("mdiv          : %d\n", mdiv);
	debug("arefclkdiv    : %d\n", arefclkdiv);
	debug("drefclkdiv    : %d\n", drefclkdiv);
	debug("refclkdiv     : %d\n", refclkdiv);
	debug("mscnt         : %d\n", mscnt);
	debug("hscnt         : %d\n", hscnt);
	debug("vcocalib      : 0x%08x\n", vcocalib);

	return vcocalib;
}

/*
 * Setup clocks while making no assumptions about previous state of the clocks.
 */
static void clk_basic_init(struct udevice *dev,
			   const struct cm_config * const cfg)
{
	struct socfpga_clk_plat *plat = dev_get_plat(dev);
	u32 vcocalib;
	uintptr_t base_addr = (uintptr_t)plat->regs;

	if (!cfg)
		return;

	if (IS_ENABLED(CONFIG_TARGET_SOCFPGA_AGILEX5_EMU)) {
		/* Take both PLL out of reset and power up */
		CM_REG_SETBITS(plat, CLKMGR_MAINPLL_PLLGLOB,
			       CLKMGR_PLLGLOB_PD_MASK | CLKMGR_PLLGLOB_RST_MASK);
		CM_REG_SETBITS(plat, CLKMGR_PERPLL_PLLGLOB,
			       CLKMGR_PLLGLOB_PD_MASK | CLKMGR_PLLGLOB_RST_MASK);

		wait_for_bit_le32((const void *)(base_addr + CLKMGR_STAT),
				  CLKMGR_STAT_ALLPLL_LOCKED_MASK, true, 20000, false);

		/* Put both PLLs in bypass */
		clk_write_bypass_mainpll(plat, CLKMGR_BYPASS_MAINPLL_ALL);
		clk_write_bypass_perpll(plat, CLKMGR_BYPASS_PERPLL_ALL);

		/* Take all PLLs out of bypass */
		clk_write_bypass_mainpll(plat, 0);
		clk_write_bypass_perpll(plat, 0);

		/* Out of boot mode */
		clk_write_ctrl(plat,
			       CM_REG_READL(plat, CLKMGR_CTRL) & ~CLKMGR_CTRL_BOOTMODE);
	} else {
#ifdef CONFIG_XPL_BUILD
		/*
		 * Configure HPS Internal Oscillator as default boot_clk source,
		 * always force clock manager into boot mode before any configuration
		 */
		clk_write_ctrl(plat, CM_REG_READL(plat, CLKMGR_CTRL) |
			       CLKMGR_CTRL_BOOTMODE |
			       CLKMGR_CTRL_SWCTRLBTCLKEN_MASK |
			       CLKMGR_CTRL_SWCTRLBTCLKSEL_MASK);
#else
		/* Skip clock configuration in SSBL if it's not in boot mode */
		if (!(CM_REG_READL(plat, CLKMGR_CTRL) & CLKMGR_CTRL_BOOTMODE))
			return;
#endif

		/* Put both PLLs in bypass */
		clk_write_bypass_mainpll(plat, CLKMGR_BYPASS_MAINPLL_ALL);
		clk_write_bypass_perpll(plat, CLKMGR_BYPASS_PERPLL_ALL);

		/* Put both PLLs in Reset and Power Down */
		CM_REG_CLRBITS(plat, CLKMGR_MAINPLL_PLLGLOB,
			       CLKMGR_PLLGLOB_PD_MASK | CLKMGR_PLLGLOB_RST_MASK);
		CM_REG_CLRBITS(plat, CLKMGR_PERPLL_PLLGLOB,
			       CLKMGR_PLLGLOB_PD_MASK | CLKMGR_PLLGLOB_RST_MASK);

		/* setup main PLL dividers where calculate the vcocalib value */
		vcocalib = calc_vocalib_pll(cfg->main_pll_pllm, cfg->main_pll_pllglob);
		CM_REG_WRITEL(plat, cfg->main_pll_pllglob & ~CLKMGR_PLLGLOB_RST_MASK,
			      CLKMGR_MAINPLL_PLLGLOB);
		CM_REG_WRITEL(plat, cfg->main_pll_fdbck, CLKMGR_MAINPLL_FDBCK);
		CM_REG_WRITEL(plat, vcocalib, CLKMGR_MAINPLL_VCOCALIB);
		CM_REG_WRITEL(plat, cfg->main_pll_pllc0, CLKMGR_MAINPLL_PLLC0);
		CM_REG_WRITEL(plat, cfg->main_pll_pllc1, CLKMGR_MAINPLL_PLLC1);
		CM_REG_WRITEL(plat, cfg->main_pll_pllc2, CLKMGR_MAINPLL_PLLC2);
		CM_REG_WRITEL(plat, cfg->main_pll_pllc3, CLKMGR_MAINPLL_PLLC3);
		CM_REG_WRITEL(plat, cfg->main_pll_pllm, CLKMGR_MAINPLL_PLLM);
		CM_REG_WRITEL(plat, cfg->main_pll_nocclk, CLKMGR_MAINPLL_NOCCLK);
		CM_REG_WRITEL(plat, cfg->main_pll_nocdiv, CLKMGR_MAINPLL_NOCDIV);

		/* setup peripheral PLL dividers where calculate the vcocalib value */
		vcocalib = calc_vocalib_pll(cfg->per_pll_pllm, cfg->per_pll_pllglob);
		CM_REG_WRITEL(plat, cfg->per_pll_pllglob & ~CLKMGR_PLLGLOB_RST_MASK,
			      CLKMGR_PERPLL_PLLGLOB);
		CM_REG_WRITEL(plat, cfg->per_pll_fdbck, CLKMGR_PERPLL_FDBCK);
		CM_REG_WRITEL(plat, vcocalib, CLKMGR_PERPLL_VCOCALIB);
		CM_REG_WRITEL(plat, cfg->per_pll_pllc0, CLKMGR_PERPLL_PLLC0);
		CM_REG_WRITEL(plat, cfg->per_pll_pllc1, CLKMGR_PERPLL_PLLC1);
		CM_REG_WRITEL(plat, cfg->per_pll_pllc2, CLKMGR_PERPLL_PLLC2);
		CM_REG_WRITEL(plat, cfg->per_pll_pllc3, CLKMGR_PERPLL_PLLC3);
		CM_REG_WRITEL(plat, cfg->per_pll_pllm, CLKMGR_PERPLL_PLLM);
		CM_REG_WRITEL(plat, cfg->per_pll_emacctl, CLKMGR_PERPLL_EMACCTL);
		CM_REG_WRITEL(plat, cfg->per_pll_gpiodiv, CLKMGR_PERPLL_GPIODIV);

		/* Configure ping pong counters in control group */
		CM_REG_WRITEL(plat, cfg->ctl_emacactr, CLKMGR_CTL_EMACACTR);
		CM_REG_WRITEL(plat, cfg->ctl_emacbctr, CLKMGR_CTL_EMACBCTR);
		CM_REG_WRITEL(plat, cfg->ctl_emacptpctr, CLKMGR_CTL_EMACPTPCTR);
		CM_REG_WRITEL(plat, cfg->ctl_gpiodbctr, CLKMGR_CTL_GPIODBCTR);
		CM_REG_WRITEL(plat, cfg->ctl_s2fuser0ctr, CLKMGR_CTL_S2FUSER0CTR);
		CM_REG_WRITEL(plat, cfg->ctl_s2fuser1ctr, CLKMGR_CTL_S2FUSER1CTR);
		CM_REG_WRITEL(plat, cfg->ctl_psirefctr, CLKMGR_CTL_PSIREFCTR);
		CM_REG_WRITEL(plat, cfg->ctl_usb31ctr, CLKMGR_CTL_USB31CTR);
		CM_REG_WRITEL(plat, cfg->ctl_dsuctr, CLKMGR_CTL_DSUCTR);
		CM_REG_WRITEL(plat, cfg->ctl_core01ctr, CLKMGR_CTL_CORE01CTR);
		CM_REG_WRITEL(plat, cfg->ctl_core23ctr, CLKMGR_CTL_CORE23CTR);
		CM_REG_WRITEL(plat, cfg->ctl_core2ctr, CLKMGR_CTL_CORE2CTR);
		CM_REG_WRITEL(plat, cfg->ctl_core3ctr, CLKMGR_CTL_CORE3CTR);

		/* Take both PLL out of reset and power up */
		CM_REG_SETBITS(plat, CLKMGR_MAINPLL_PLLGLOB,
			       CLKMGR_PLLGLOB_PD_MASK | CLKMGR_PLLGLOB_RST_MASK);
		CM_REG_SETBITS(plat, CLKMGR_PERPLL_PLLGLOB,
			       CLKMGR_PLLGLOB_PD_MASK | CLKMGR_PLLGLOB_RST_MASK);

		/* Membus programming for mainpll */
		membus_pll_configs(plat, MEMBUS_MAINPLL);
		/* Membus programming for peripll */
		membus_pll_configs(plat, MEMBUS_PERPLL);

		/* Enable Main pll clkslices */
		CM_REG_WRITEL(plat, CM_REG_READL(plat, CLKMGR_MAINPLL_PLLC0) |
				CLKMGR_PLLCX_EN_SET_MSK,
				CLKMGR_MAINPLL_PLLC0);
		CM_REG_WRITEL(plat, CM_REG_READL(plat, CLKMGR_MAINPLL_PLLC1) |
				CLKMGR_PLLCX_EN_SET_MSK,
				CLKMGR_MAINPLL_PLLC1);
		CM_REG_WRITEL(plat, CM_REG_READL(plat, CLKMGR_MAINPLL_PLLC2) |
				CLKMGR_PLLCX_EN_SET_MSK,
				CLKMGR_MAINPLL_PLLC2);
		CM_REG_WRITEL(plat, CM_REG_READL(plat, CLKMGR_MAINPLL_PLLC3) |
				CLKMGR_PLLCX_EN_SET_MSK,
				CLKMGR_MAINPLL_PLLC3);

		/* Enable Periph pll clkslices */
		CM_REG_WRITEL(plat, CM_REG_READL(plat, CLKMGR_PERPLL_PLLC0) |
				CLKMGR_PLLCX_EN_SET_MSK,
				CLKMGR_PERPLL_PLLC0);
		CM_REG_WRITEL(plat, CM_REG_READL(plat, CLKMGR_PERPLL_PLLC1) |
				CLKMGR_PLLCX_EN_SET_MSK,
				CLKMGR_PERPLL_PLLC1);
		CM_REG_WRITEL(plat, CM_REG_READL(plat, CLKMGR_PERPLL_PLLC2) |
				CLKMGR_PLLCX_EN_SET_MSK,
				CLKMGR_PERPLL_PLLC2);
		CM_REG_WRITEL(plat, CM_REG_READL(plat, CLKMGR_PERPLL_PLLC3) |
				CLKMGR_PLLCX_EN_SET_MSK,
				CLKMGR_PERPLL_PLLC3);

		wait_for_bit_le32((const void *)(base_addr + CLKMGR_STAT),
				  CLKMGR_STAT_ALLPLL_LOCKED_MASK, true, 20000, false);

		CM_REG_WRITEL(plat, CLKMGR_LOSTLOCK_SET_MASK, CLKMGR_MAINPLL_LOSTLOCK);
		CM_REG_WRITEL(plat, CLKMGR_LOSTLOCK_SET_MASK, CLKMGR_PERPLL_LOSTLOCK);

		CM_REG_WRITEL(plat, CM_REG_READL(plat, CLKMGR_MAINPLL_PLLGLOB) |
				CLKMGR_PLLGLOB_CLR_LOSTLOCK_BYPASS_MASK,
				CLKMGR_MAINPLL_PLLGLOB);
		CM_REG_WRITEL(plat, CM_REG_READL(plat, CLKMGR_PERPLL_PLLGLOB) |
				CLKMGR_PLLGLOB_CLR_LOSTLOCK_BYPASS_MASK,
				CLKMGR_PERPLL_PLLGLOB);

		/* Take all PLLs out of bypass */
		clk_write_bypass_mainpll(plat, 0);
		clk_write_bypass_perpll(plat, 0);

		/* Clear the loss of lock bits (write 1 to clear) */
		CM_REG_CLRBITS(plat, CLKMGR_INTRCLR,
			       CLKMGR_INTER_PERPLLLOST_MASK |
			       CLKMGR_INTER_MAINPLLLOST_MASK);

		/* Take all ping pong counters out of reset */
		CM_REG_CLRBITS(plat, CLKMGR_CTL_EXTCNTRST,
			       CLKMGR_CTL_EXTCNTRST_ALLCNTRST);

#ifdef COUNTER_FREQUENCY_REAL
	u32 cntfrq = COUNTER_FREQUENCY_REAL;
	u32 counter_freq = 0;

	/* Update with accurate clock frequency */
	if (current_el() == 3) {
		asm volatile("msr cntfrq_el0, %0" : : "r" (cntfrq) : "memory");
		asm volatile("mrs %0, cntfrq_el0" : "=r" (counter_freq));
		debug("Counter freq = 0x%x\n", counter_freq);
	}
#endif

		/* Out of boot mode */
		clk_write_ctrl(plat,
			       CM_REG_READL(plat, CLKMGR_CTRL) & ~CLKMGR_CTRL_BOOTMODE);
	}
}

static u64 clk_get_vco_clk_hz(struct socfpga_clk_plat *plat,
			      u32 pllglob_reg, u32 pllm_reg)
{
	 u64 fref, arefdiv, mdiv, reg, vco;

	reg = CM_REG_READL(plat, pllglob_reg);

	fref = (reg & CLKMGR_PLLGLOB_VCO_PSRC_MASK) >>
		CLKMGR_PLLGLOB_VCO_PSRC_OFFSET;

	switch (fref) {
	case CLKMGR_VCO_PSRC_EOSC1:
		fref = cm_get_osc_clk_hz();
		break;
	case CLKMGR_VCO_PSRC_INTOSC:
		fref = cm_get_intosc_clk_hz();
		break;
	case CLKMGR_VCO_PSRC_F2S:
		fref = cm_get_fpga_clk_hz();
		break;
	}

	arefdiv = (reg & CLKMGR_PLLGLOB_AREFCLKDIV_MASK) >>
		   CLKMGR_PLLGLOB_AREFCLKDIV_OFFSET;

	mdiv = CM_REG_READL(plat, pllm_reg) & CLKMGR_PLLM_MDIV_MASK;

	vco = fref / arefdiv;
	vco = vco * mdiv;

	return vco;
}

static u64 clk_get_main_vco_clk_hz(struct socfpga_clk_plat *plat)
{
	return clk_get_vco_clk_hz(plat, CLKMGR_MAINPLL_PLLGLOB,
				 CLKMGR_MAINPLL_PLLM);
}

static u64 clk_get_per_vco_clk_hz(struct socfpga_clk_plat *plat)
{
	return clk_get_vco_clk_hz(plat, CLKMGR_PERPLL_PLLGLOB,
				 CLKMGR_PERPLL_PLLM);
}

static u32 clk_get_5_1_clk_src(struct socfpga_clk_plat *plat, u64 reg)
{
	u32 clksrc = CM_REG_READL(plat, reg);

	return (clksrc & CLKMGR_CLKSRC_MASK) >> CLKMGR_CLKSRC_OFFSET;
}

static u64 clk_get_clksrc_hz(struct socfpga_clk_plat *plat, u32 clksrc_reg,
			     u32 main_reg, u32 per_reg)
{
	u64 clock;
	u32 clklsrc = clk_get_5_1_clk_src(plat, clksrc_reg);

	switch (clklsrc) {
	case CLKMGR_CLKSRC_MAIN:
		clock = clk_get_main_vco_clk_hz(plat);
		clock /= (CM_REG_READL(plat, main_reg) &
			  CLKMGR_CLKCNT_MSK);
		break;

	case CLKMGR_CLKSRC_PER:
		clock = clk_get_per_vco_clk_hz(plat);
		clock /= (CM_REG_READL(plat, per_reg) &
			  CLKMGR_CLKCNT_MSK);
		break;

	case CLKMGR_CLKSRC_OSC1:
		clock = cm_get_osc_clk_hz();
		break;

	case CLKMGR_CLKSRC_INTOSC:
		clock = cm_get_intosc_clk_hz();
		break;

	case CLKMGR_CLKSRC_FPGA:
		clock = cm_get_fpga_clk_hz();
		break;
	default:
		return 0;
	}

	return clock;
}

static u64 clk_get_mpu_clk_hz(struct socfpga_clk_plat *plat)
{
	u64 clock;
	u32 ctr_reg;
	u32 cpu = ((read_mpidr() >> MPIDR_AFF1_OFFSET) & MPIDR_AFF1_OFFSET);

	if (cpu > CORE1) {
		ctr_reg = CLKMGR_CTL_CORE23CTR;

		clock = clk_get_clksrc_hz(plat, ctr_reg,
					  CLKMGR_MAINPLL_PLLC0,
					  CLKMGR_PERPLL_PLLC0);
	} else {
		ctr_reg = CLKMGR_CTL_CORE01CTR;

		clock = clk_get_clksrc_hz(plat, ctr_reg,
					  CLKMGR_MAINPLL_PLLC1,
					  CLKMGR_PERPLL_PLLC0);
	}

	if (cpu == CORE3)
		ctr_reg = CLKMGR_CTL_CORE3CTR;
	else if (cpu == CORE2)
		ctr_reg = CLKMGR_CTL_CORE2CTR;
	else
		ctr_reg = CLKMGR_CTL_CORE01CTR;

	clock /= 1 + (CM_REG_READL(plat, ctr_reg) &
		 CLKMGR_CLKCNT_MSK);

	return clock;
}

static u32 clk_get_l3_main_clk_hz(struct socfpga_clk_plat *plat)
{
	return clk_get_clksrc_hz(plat, CLKMGR_MAINPLL_NOCCLK,
				      CLKMGR_MAINPLL_PLLC3,
				      CLKMGR_PERPLL_PLLC1);
}

static u32 clk_get_l4_main_clk_hz(struct socfpga_clk_plat *plat)
{
	u64 clock = clk_get_l3_main_clk_hz(plat);

	return clock;
}

static u32 clk_get_l4_sp_clk_hz(struct socfpga_clk_plat *plat)
{
	u64 clock = clk_get_l3_main_clk_hz(plat);

	clock /= BIT((CM_REG_READL(plat, CLKMGR_MAINPLL_NOCDIV) >>
		      CLKMGR_NOCDIV_L4SPCLK_OFFSET) &
		      CLKMGR_NOCDIV_DIVIDER_MASK);

	return clock;
}

static u32 clk_get_l4_mp_clk_hz(struct socfpga_clk_plat *plat)
{
	u64 clock = clk_get_l3_main_clk_hz(plat);

	clock /= BIT((CM_REG_READL(plat, CLKMGR_MAINPLL_NOCDIV) >>
		      CLKMGR_NOCDIV_L4MPCLK_OFFSET) &
		      CLKMGR_NOCDIV_DIVIDER_MASK);

	return clock;
}

static u32 clk_get_sdmmc_clk_hz(struct socfpga_clk_plat *plat)
{
	u64 clock = clk_get_l4_mp_clk_hz(plat);

	clock /= BIT((CM_REG_READL(plat, CLKMGR_MAINPLL_NOCDIV) >>
		      CLKMGR_NOCDIV_SOFTPHY_OFFSET) &
		      CLKMGR_NOCDIV_DIVIDER_MASK);

	return clock;
}

static u32 clk_get_l4_sys_free_clk_hz(struct socfpga_clk_plat *plat)
{
	if (CM_REG_READL(plat, CLKMGR_STAT) & CLKMGR_STAT_BOOTMODE)
		return clk_get_l3_main_clk_hz(plat) / 2;

	return clk_get_l3_main_clk_hz(plat) / 4;
}

static u32 clk_get_emac_clk_hz(struct socfpga_clk_plat *plat, u32 emac_id)
{
	u32 ctl;
	u32 ctr_reg;
	u32 clock;
	u32 div;
	u32 reg;

	if (emac_id == AGILEX5_EMAC_PTP_CLK) {
		reg = CM_REG_READL(plat, CLKMGR_CTL_EMACPTPCTR);
		ctr_reg = CLKMGR_CTL_EMACPTPCTR;
	} else {
		reg = CM_REG_READL(plat, CLKMGR_CTL_EMACACTR);
		ctl = CM_REG_READL(plat, CLKMGR_PERPLL_EMACCTL);
		if (emac_id == AGILEX5_EMAC0_CLK)
			ctl = (ctl & CLKMGR_PERPLLGRP_EMACCTL_EMAC0SELB_MASK) >>
				CLKMGR_PERPLLGRP_EMACCTL_EMAC0SELB_OFFSET;
		else if (emac_id == AGILEX5_EMAC1_CLK)
			ctl = (ctl & CLKMGR_PERPLLGRP_EMACCTL_EMAC1SELB_MASK) >>
				CLKMGR_PERPLLGRP_EMACCTL_EMAC1SELB_OFFSET;
		else if (emac_id == AGILEX5_EMAC2_CLK)
			ctl = (ctl & CLKMGR_PERPLLGRP_EMACCTL_EMAC2SELB_MASK) >>
				CLKMGR_PERPLLGRP_EMACCTL_EMAC2SELB_OFFSET;
		else
			return 0;

		if (ctl) {
			/* EMAC B source */
			ctr_reg = CLKMGR_CTL_EMACBCTR;
		} else {
			/* EMAC A source */
			ctr_reg = CLKMGR_CTL_EMACACTR;
		}
	}
	/* Get EMAC clock source */
	clock = (reg & CLKMGR_CTL_EMACCTR_SRC_MASK)
		 >> CLKMGR_CTL_EMACCTR_SRC_OFFSET;

	reg = CM_REG_READL(plat, ctr_reg);
	div = (reg & CLKMGR_CTL_EMACCTR_CNT_MASK)
		>> CLKMGR_CTL_EMACCTR_CNT_OFFSET;

	switch (clock) {
	case CLKMGR_CLKSRC_MAIN:
		clock = clk_get_main_vco_clk_hz(plat);

		if (emac_id == AGILEX5_EMAC_PTP_CLK) {
			clock /= (CM_REG_READL(plat, CLKMGR_MAINPLL_PLLC3) &
				  CLKMGR_CLKCNT_MSK);
		} else {
			clock /= (CM_REG_READL(plat, CLKMGR_MAINPLL_PLLC1) &
				  CLKMGR_CLKCNT_MSK);
		}
		break;

	case CLKMGR_CLKSRC_PER:
		clock = clk_get_per_vco_clk_hz(plat);

		clock /= (CM_REG_READL(plat, CLKMGR_PERPLL_PLLC3) &
			  CLKMGR_CLKCNT_MSK);
		break;

	case CLKMGR_CLKSRC_OSC1:
		clock = cm_get_osc_clk_hz();
		break;

	case CLKMGR_CLKSRC_INTOSC:
		clock = cm_get_intosc_clk_hz();
		break;

	case CLKMGR_CLKSRC_FPGA:
		clock = cm_get_fpga_clk_hz();
		break;
	}

	clock /= 1 + div;

	return clock;
}

static ulong socfpga_clk_get_rate(struct clk *clk)
{
	struct socfpga_clk_plat *plat = dev_get_plat(clk->dev);

	switch (clk->id) {
	case AGILEX5_MPU_CLK:
		return clk_get_mpu_clk_hz(plat);
	case AGILEX5_L4_MAIN_CLK:
		return clk_get_l4_main_clk_hz(plat);
	case AGILEX5_L4_SYS_FREE_CLK:
		return clk_get_l4_sys_free_clk_hz(plat);
	case AGILEX5_L4_MP_CLK:
		return clk_get_l4_mp_clk_hz(plat);
	case AGILEX5_L4_SP_CLK:
		return clk_get_l4_sp_clk_hz(plat);
	case AGILEX5_SDMMC_CLK:
	case AGILEX5_NAND_CLK:
		return clk_get_sdmmc_clk_hz(plat);
	case AGILEX5_EMAC0_CLK:
	case AGILEX5_EMAC1_CLK:
	case AGILEX5_EMAC2_CLK:
	case AGILEX5_EMAC_PTP_CLK:
		return clk_get_emac_clk_hz(plat, clk->id);
	case AGILEX5_USB_CLK:
	case AGILEX5_NAND_X_CLK:
		return clk_get_l4_mp_clk_hz(plat);
	default:
		return -ENXIO;
	}
}

static int socfpga_clk_enable(struct clk *clk)
{
	return 0;
}

static int socfpga_clk_probe(struct udevice *dev)
{
	const struct cm_config *cm_default_cfg = cm_get_default_config();

	clk_basic_init(dev, cm_default_cfg);

	return 0;
}

static int socfpga_clk_of_to_plat(struct udevice *dev)
{
	struct socfpga_clk_plat *plat = dev_get_plat(dev);
	fdt_addr_t addr;

	addr = dev_read_addr(dev);
	if (addr == FDT_ADDR_T_NONE)
		return -EINVAL;
	plat->regs = (void __iomem *)addr;

	return 0;
}

static struct clk_ops socfpga_clk_ops = {
	.enable		= socfpga_clk_enable,
	.get_rate	= socfpga_clk_get_rate,
};

static const struct udevice_id socfpga_clk_match[] = {
	{ .compatible = "intel,agilex5-clkmgr" },
	{}
};

U_BOOT_DRIVER(socfpga_agilex5_clk) = {
	.name		= "clk-agilex5",
	.id		= UCLASS_CLK,
	.of_match	= socfpga_clk_match,
	.ops		= &socfpga_clk_ops,
	.probe		= socfpga_clk_probe,
	.of_to_plat = socfpga_clk_of_to_plat,
	.plat_auto	= sizeof(struct socfpga_clk_plat),
};
