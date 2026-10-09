// SPDX-License-Identifier: GPL-2.0-only
/*
 * Temporary R1 diagnostic: hold the display clocks while the installed
 * vendor framebuffer performs its ordinary blank/unblank callbacks. This
 * module deliberately performs no MMIO access and does not replace or call
 * framebuffer operations.
 */
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>

static struct clk *gate_lcd;
static struct clk *div_lcd;
static bool gate_enabled;
static bool div_enabled;

static int __init compas_display_clock_observer_init(void)
{
	int ret;

	gate_lcd = clk_get(NULL, "gate_lcd");
	if (IS_ERR(gate_lcd)) {
		ret = PTR_ERR(gate_lcd);
		gate_lcd = NULL;
		pr_err("compas_display_clock_observer: clk_get(gate_lcd) failed: %d\n",
		       ret);
		return ret;
	}

	div_lcd = clk_get(NULL, "div_lcd");
	if (IS_ERR(div_lcd)) {
		ret = PTR_ERR(div_lcd);
		div_lcd = NULL;
		goto err_put_gate;
	}

	ret = clk_prepare_enable(gate_lcd);
	if (ret)
		goto err_put_both;
	gate_enabled = true;

	ret = clk_prepare_enable(div_lcd);
	if (ret)
		goto err_disable_gate;
	div_enabled = true;

	pr_info("compas_display_clock_observer: holding gate_lcd and div_lcd\n");
	return 0;

err_disable_gate:
	clk_disable_unprepare(gate_lcd);
	gate_enabled = false;
err_put_both:
	clk_put(div_lcd);
	div_lcd = NULL;
err_put_gate:
	clk_put(gate_lcd);
	gate_lcd = NULL;
	return ret;
}

static void __exit compas_display_clock_observer_exit(void)
{
	if (div_enabled) {
		clk_disable_unprepare(div_lcd);
		div_enabled = false;
	}
	if (gate_enabled) {
		clk_disable_unprepare(gate_lcd);
		gate_enabled = false;
	}
	if (div_lcd) {
		clk_put(div_lcd);
		div_lcd = NULL;
	}
	if (gate_lcd) {
		clk_put(gate_lcd);
		gate_lcd = NULL;
	}
	pr_info("compas_display_clock_observer: released display clocks\n");
}

module_init(compas_display_clock_observer_init);
module_exit(compas_display_clock_observer_exit);

MODULE_DESCRIPTION("Temporary R1 framebuffer clock observer diagnostic");
MODULE_LICENSE("GPL");
