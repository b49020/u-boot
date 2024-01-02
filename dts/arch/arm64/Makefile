# SPDX-License-Identifier: GPL-2.0+

dtb-$(CONFIG_ARCH_MESON) += \
	amlogic/meson-a1-ad401.dtb \
	amlogic/meson-axg-s400.dtb \
	amlogic/meson-axg-jethome-jethub-j100.dtb \
	amlogic/meson-gxbb-kii-pro.dtb \
	amlogic/meson-gxbb-nanopi-k2.dtb \
	amlogic/meson-gxbb-odroidc2.dtb \
	amlogic/meson-gxbb-nanopi-k2.dtb \
	amlogic/meson-gxbb-p200.dtb \
	amlogic/meson-gxbb-p201.dtb \
	amlogic/meson-gxbb-wetek-hub.dtb \
	amlogic/meson-gxbb-wetek-play2.dtb \
	amlogic/meson-gxl-s805x-libretech-ac.dtb \
	amlogic/meson-gxl-s905d-libretech-pc.dtb \
	amlogic/meson-gxl-s905w-jethome-jethub-j80.dtb \
	amlogic/meson-gxl-s905x-khadas-vim.dtb \
	amlogic/meson-gxl-s905x-libretech-cc.dtb \
	amlogic/meson-gxl-s905x-libretech-cc-v2.dtb \
	amlogic/meson-gxl-s905x-p212.dtb \
	amlogic/meson-gxm-gt1-ultimate.dtb \
	amlogic/meson-gxm-khadas-vim2.dtb \
	amlogic/meson-gxm-s912-libretech-pc.dtb \
	amlogic/meson-gxm-wetek-core2.dtb \
	amlogic/meson-g12a-radxa-zero.dtb \
	amlogic/meson-g12a-sei510.dtb \
	amlogic/meson-g12a-u200.dtb \
	amlogic/meson-g12b-a311d-bananapi-m2s.dtb \
	amlogic/meson-g12b-a311d-khadas-vim3.dtb \
	amlogic/meson-g12b-bananapi-cm4-cm4io.dtb \
	amlogic/meson-g12b-gsking-x.dtb \
	amlogic/meson-g12b-gtking.dtb \
	amlogic/meson-g12b-gtking-pro.dtb \
	amlogic/meson-g12b-odroid-go-ultra.dtb \
	amlogic/meson-g12b-odroid-n2.dtb \
	amlogic/meson-g12b-odroid-n2l.dtb \
	amlogic/meson-g12b-odroid-n2-plus.dtb \
	amlogic/meson-g12b-radxa-zero2.dtb \
	amlogic/meson-sm1-bananapi-m2-pro.dtb \
	amlogic/meson-sm1-bananapi-m5.dtb \
	amlogic/meson-sm1-khadas-vim3l.dtb \
	amlogic/meson-sm1-odroid-c4.dtb \
	amlogic/meson-sm1-odroid-hc4.dtb \
	amlogic/meson-sm1-sei610.dtb

include $(srctree)/scripts/Makefile.dts

targets += $(dtb-y)

# Add any required device tree compiler flags here
DTC_FLAGS += -a 0x8

PHONY += dtbs
dtbs: $(addprefix $(obj)/, $(dtb-y))
	@:

clean-files := */*.dtb */*.dtbo */*_HS
