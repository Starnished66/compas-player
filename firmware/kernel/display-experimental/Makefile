# SPDX-License-Identifier: GPL-2.0-only
obj-m += soc_fb.o
soc_fb-y := compas_fb_driver.o compas_fb_core.o compas_fb_clock.o compas_fb_hw.o compas_fb_fops.o
ccflags-y += -I$(src)
KBUILD_EXTRA_SYMBOLS += $(src)/hiby.symvers
