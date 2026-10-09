// SPDX-License-Identifier: GPL-2.0-only
/*
 * Experimental R1 X1600 framebuffer replacement for the vendor soc_fb.ko.
 * The panel module registers its immutable 168-byte panel descriptor through
 * jzfb_register_lcd(). Scanout memory and mappings are deliberately permanent:
 * the retained panel module has no unregister-safe lifetime contract.
 */
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/console.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fb.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/preempt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/uaccess.h>
#include <asm/fb.h>
#include <asm/irqflags.h>
#include <asm/pgtable.h>

#include "compas_fb_core.h"
#include "compas_fb_clock.h"
#include "compas_fb_fops.h"
#include "compas_fb_hw.h"
#include "compas_lcdc_abi.h"

#ifdef CONFIG_SMP
#error "R1 LCD CGU inversion update requires verified UP clock-writer exclusion"
#endif

#define COMPAS_FB_IRQ		39
#define COMPAS_FB_MMIO_PHYS	0x13050000UL
#define COMPAS_FB_MMIO_SIZE	0x10000UL
#define COMPAS_FB_CLOCK_HZ	28303248UL
#define COMPAS_FB_PIN_MASK	0x0ffcfcfcU
#define COMPAS_FB_GPIO_FUNC_0	0x0100U
#define COMPAS_FB_GPIO_OUTPUT_0	0x0104U
#define COMPAS_FB_PAN_TIMEOUT_MS	500U
#define COMPAS_FB_LIVE_PAN_TIMEOUT_MS	100U
#define COMPAS_FB_STOP_POLLS	COMPAS_FB_HW_STOP_POLLS
#define COMPAS_FB_ALLOC_BYTES	(COMPAS_FB_ALLOCATION_BYTES + \
				 COMPAS_FB_HW_DESC_SLOT_BYTES * COMPAS_FB_PAGES)

/* R1 port-A display pins are GPIO2..27 selected by the verified pin mask. */
static const unsigned int r1_display_pins[] = {
	2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15,
	18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
};

extern unsigned long rmem_alloc_aligned(int size, int align);
extern void rmem_free(unsigned long addr, int size);
extern int gpio_port_set_func(int port, unsigned int pins, int function);

struct compas_fb_device {
	struct mutex op_mutex;
	spinlock_t irq_lock;
	struct completion pan_done;
	struct completion first_frame;
	struct compas_fb_pan_state pan;
	struct compas_fb_hw_ops hw_ops;
	void __iomem *mmio;
	struct clk *gate_clk;
	struct clk *pixel_clk;
	struct fb_info *info;
	struct lcdc_data panel;
	const struct lcdc_data *panel_source;
	struct module *panel_owner;
	unsigned long memory_kseg0;
	unsigned long memory_phys;
	unsigned int memory_bytes;
	unsigned long pixel_clock_hz;
	unsigned int pixelclock_ps;
	struct compas_fb_hw_desc_words *descriptors[COMPAS_FB_PAGES];
	unsigned int committed_page;
	int pan_error;
	u32 pan_events;
	int first_frame_error;
	bool irq_requested;
	bool irq_disabled;
	bool clocks_enabled;
	bool gate_enabled;
	bool pixel_enabled;
	bool memory_region_requested;
	bool pins_requested;
	bool fb_registered;
	bool registering;
	bool blanked;
	bool initialized;
	bool panel_powered;
	bool pinned;
	bool quarantined;
	bool starting;
	bool panel_owner_pinned;
	bool irq_fault;
	bool pm_blank_requested;
	u32 pseudo_palette[16];
};

static struct compas_fb_device r1fb;
static int frame_num = COMPAS_FB_PAGES;
static int pan_display_sync = 1;
static int lcd_is_inited;
static unsigned long underrun_count;
module_param(frame_num, int, 0444);
module_param(pan_display_sync, int, 0444);
module_param(lcd_is_inited, int, 0444);
module_param(underrun_count, ulong, 0444);
MODULE_PARM_DESC(frame_num, "R1 framebuffer page count (fixed at two)");
MODULE_PARM_DESC(pan_display_sync, "R1 framebuffer pan is synchronous");
MODULE_PARM_DESC(lcd_is_inited, "R1 panel registration state");
MODULE_PARM_DESC(underrun_count, "Count of non-fatal DPU underrun events");

static int start_first_frame_locked(unsigned int page);
static void quarantine_resources(const char *reason);

static u32 hw_read(void *context, u32 offset)
{
	struct compas_fb_device *fb = context;

	return readl(fb->mmio + offset);
}

static void hw_write(void *context, u32 offset, u32 value)
{
	struct compas_fb_device *fb = context;

	writel(value, fb->mmio + offset);
}

static void hw_delay_us(void *context, u32 usec)
{
	(void)context;
	/* Stop polling runs under op_mutex in process context, never in the IRQ. */
	if (usec >= 1000U)
		usleep_range(usec, usec + 100U);
	else
		udelay(usec);
}

static void flush_dma_range(void *address, size_t bytes)
{
	dma_cache_sync(NULL, address, bytes, DMA_TO_DEVICE);
	wmb();
}

static void drain_cpu_writes(void)
{
	/* Match the vendor XBurst uncached read used to drain CCA0 stores. */
	wmb();
	(void)readl((void __iomem *)0xa0000000UL);
}

static int request_display_pins(void)
{
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(r1_display_pins); ++i) {
		ret = gpio_request(r1_display_pins[i], "r1-dpu");
		if (ret)
			goto fail;
	}
	r1fb.pins_requested = true;
	return 0;

fail:
	while (i--)
		gpio_free(r1_display_pins[i]);
	return ret;
}

static void release_display_pins(void)
{
	unsigned int i;

	if (!r1fb.pins_requested)
		return;
	for (i = 0; i < ARRAY_SIZE(r1_display_pins); ++i)
		gpio_free(r1_display_pins[i]);
	r1fb.pins_requested = false;
}

static int enable_clocks(void)
{
	bool gate_was_enabled = r1fb.gate_enabled;
	int ret;

	if (r1fb.clocks_enabled)
		return 0;
	if (!r1fb.gate_enabled) {
		ret = clk_prepare_enable(r1fb.gate_clk);
		if (ret)
			return ret;
		r1fb.gate_enabled = true;
	}
	if (!r1fb.pixel_enabled) {
		ret = clk_prepare_enable(r1fb.pixel_clk);
		if (ret) {
			if (!gate_was_enabled) {
				clk_disable_unprepare(r1fb.gate_clk);
				r1fb.gate_enabled = false;
			}
			return ret;
		}
		r1fb.pixel_enabled = true;
	}
	r1fb.clocks_enabled = true;
	return 0;
}

static int set_pixel_clock(void)
{
	return clk_set_rate(r1fb.pixel_clk, COMPAS_FB_CLOCK_HZ);
}

static int validate_pixel_clock_rate(bool initial)
{
	unsigned long rate = clk_get_rate(r1fb.pixel_clk);
	unsigned int pixclock_ps;
	int ret;

	if (rate > 0xffffffffUL ||
	    !compas_fb_clock_rate_valid((compas_fb_clock_u32)rate,
					COMPAS_FB_CLOCK_NOMINAL_HZ))
		return -ERANGE;
	if (!initial && rate != r1fb.pixel_clock_hz)
		return -ERANGE;
	ret = compas_fb_clock_pixclock_ps((compas_fb_clock_u32)rate,
					  &pixclock_ps);
	if (ret)
		return ret;
	if (initial) {
		r1fb.pixel_clock_hz = rate;
		r1fb.pixelclock_ps = pixclock_ps;
	} else if (pixclock_ps != r1fb.pixelclock_ps) {
		return -ERANGE;
	}
	return 0;
}

static int clear_pixel_clock_invert(void)
{
	void __iomem *cgu = (void __iomem *)0xb0000064UL;
	unsigned long irq_flags;
	compas_fb_clock_u32 before, after, readback;
	int ret;

	/* CCF's DIV writer uses this UP CPU with local-IRQ exclusion. */
	preempt_disable();
	local_irq_save(irq_flags);
	before = readl(cgu);
	ret = compas_fb_clock_clear_invert(before, &after);
	if (!ret) {
		writel(after, cgu);
		readback = readl(cgu);
		if (readback != after)
			ret = -EIO;
	}
	local_irq_restore(irq_flags);
	preempt_enable();
	return ret;
}

static void disable_clocks(void)
{
	if (r1fb.pixel_enabled) {
		clk_disable_unprepare(r1fb.pixel_clk);
		r1fb.pixel_enabled = false;
	}
	if (r1fb.gate_enabled) {
		clk_disable_unprepare(r1fb.gate_clk);
		r1fb.gate_enabled = false;
	}
	r1fb.clocks_enabled = false;
}

static int select_display_pins(void)
{
	return gpio_port_set_func(0, COMPAS_FB_PIN_MASK,
				 COMPAS_FB_GPIO_FUNC_0);
}

static int select_power_off_pins(void)
{
	return gpio_port_set_func(0, (1U << 24) | (1U << 27),
				 COMPAS_FB_GPIO_OUTPUT_0);
}

static u32 interrupt_events(void)
{
	return COMPAS_FB_HW_FRAME_END | COMPAS_FB_HW_UNDERRUN |
	       COMPAS_FB_HW_STOP_ACK;
}

static int arm_hardware_events(void)
{
	u32 expected = interrupt_events();

	writel(expected, r1fb.mmio + COMPAS_FB_HW_REG_INTERRUPT_MASK);
	return readl(r1fb.mmio + COMPAS_FB_HW_REG_INTERRUPT_MASK) == expected ?
		0 : -EIO;
}

static irqreturn_t compas_fb_irq(int irq, void *data)
{
	struct compas_fb_device *fb = data;
	unsigned long flags;
	u32 events, remaining, stuck, mask_readback, pending, token, raw_events;
	unsigned int attempt;
	bool disable_line = false;

	(void)irq;
	raw_events = readl(fb->mmio + COMPAS_FB_HW_REG_INTERRUPT_FLAG);
	events = raw_events & interrupt_events();
	if (!events)
		return IRQ_NONE;
	/* Preserve simple-reader completion before acknowledging display end. */
	raw_events |= readl(fb->mmio + COMPAS_FB_HW_REG_STATUS);
	pending = events;
	remaining = events;
	for (attempt = 0; attempt < 3 && remaining; ++attempt) {
		writel(pending, fb->mmio + COMPAS_FB_HW_REG_CLEAR_STATUS);
		remaining = readl(fb->mmio + COMPAS_FB_HW_REG_INTERRUPT_FLAG) &
			    interrupt_events();
		raw_events |= remaining |
			readl(fb->mmio + COMPAS_FB_HW_REG_STATUS);
		pending = remaining;
	}
	/* EOD may legitimately reassert on the next normal frame. */
	stuck = remaining;
	spin_lock_irqsave(&fb->irq_lock, flags);
	if (fb->pan.pending) {
		fb->pan_events |= raw_events;
		if (raw_events & (COMPAS_FB_HW_UNDERRUN | COMPAS_FB_HW_STOP_ACK))
			fb->pan_error = -EIO;
		if ((events & COMPAS_FB_HW_FRAME_END) || fb->pan_error)
			complete(&fb->pan_done);
	}
	if (stuck) {
		/* A sticky level source must be masked before returning from IRQ. */
		writel(0, fb->mmio + COMPAS_FB_HW_REG_INTERRUPT_MASK);
		mask_readback = readl(fb->mmio + COMPAS_FB_HW_REG_INTERRUPT_MASK);
		fb->irq_fault = true;
		if (mask_readback && fb->irq_requested && !fb->irq_disabled) {
			fb->irq_disabled = true;
			disable_line = true;
		}
		if (fb->starting) {
			fb->starting = false;
			fb->first_frame_error = -EIO;
			complete(&fb->first_frame);
		}
		if (fb->pan.pending) {
			token = fb->pan.generation;
			(void)compas_fb_pan_timeout(&fb->pan, token);
			fb->pan_error = -EIO;
			complete(&fb->pan_done);
		} else if (fb->pan.running) {
			compas_fb_pan_stop_requested(&fb->pan);
		}
	}
	if (events & COMPAS_FB_HW_UNDERRUN) {
		++underrun_count;
		pr_warn_ratelimited("soc_fb: non-fatal DPU underrun (%lu total)\n",
				    underrun_count);
	}
	if (!stuck && (events & COMPAS_FB_HW_FRAME_END) && fb->starting) {
		fb->starting = false;
		fb->first_frame_error = 0;
		complete(&fb->first_frame);
	}
	spin_unlock_irqrestore(&fb->irq_lock, flags);
	if (disable_line)
		disable_irq_nosync(COMPAS_FB_IRQ);
	return IRQ_HANDLED;
}

static void disable_scanout_irq(void)
{
	unsigned long flags;
	bool disable_line = false, requested;

	spin_lock_irqsave(&r1fb.irq_lock, flags);
	requested = r1fb.irq_requested;
	if (requested && !r1fb.irq_disabled) {
		r1fb.irq_disabled = true;
		disable_line = true;
	}
	spin_unlock_irqrestore(&r1fb.irq_lock, flags);
	if (!requested)
		return;
	if (disable_line)
		disable_irq(COMPAS_FB_IRQ);
	/* Also joins a handler disabled with disable_irq_nosync() from the ISR. */
	synchronize_irq(COMPAS_FB_IRQ);
}

static void enable_scanout_irq(void)
{
	unsigned long flags;
	bool enable_line = false;

	spin_lock_irqsave(&r1fb.irq_lock, flags);
	if (r1fb.irq_requested && r1fb.irq_disabled && !r1fb.irq_fault) {
		r1fb.irq_disabled = false;
		enable_line = true;
	}
	spin_unlock_irqrestore(&r1fb.irq_lock, flags);
	if (enable_line)
		enable_irq(COMPAS_FB_IRQ);
}

static bool scanout_irq_faulted(void)
{
	unsigned long flags;
	bool faulted;

	spin_lock_irqsave(&r1fb.irq_lock, flags);
	faulted = r1fb.irq_fault;
	spin_unlock_irqrestore(&r1fb.irq_lock, flags);
	return faulted;
}

static int establish_idle_locked(void)
{
	int ret;

	disable_scanout_irq();
	ret = arm_hardware_events();
	if (ret)
		return ret;
	ret = compas_fb_hw_stop_rdma(&r1fb.hw_ops, COMPAS_FB_STOP_POLLS);
	if (ret)
		return ret;
	ret = compas_fb_hw_clear_events(&r1fb.hw_ops, interrupt_events());
	return ret;
}

static int stop_scanout_locked(void)
{
	unsigned long flags;
	int ret;

	disable_scanout_irq();
	spin_lock_irqsave(&r1fb.irq_lock, flags);
	r1fb.starting = false;
	compas_fb_pan_stop_requested(&r1fb.pan);
	spin_unlock_irqrestore(&r1fb.irq_lock, flags);
	ret = arm_hardware_events();
	if (ret)
		return ret;
	ret = compas_fb_hw_stop_rdma(&r1fb.hw_ops, COMPAS_FB_STOP_POLLS);
	if (ret)
		return ret;
	ret = compas_fb_hw_clear_events(&r1fb.hw_ops, interrupt_events());
	if (ret)
		return ret;
	spin_lock_irqsave(&r1fb.irq_lock, flags);
	ret = compas_fb_pan_stop_confirmed(&r1fb.pan);
	if (!ret)
		ret = compas_fb_pan_reset(&r1fb.pan, r1fb.committed_page);
	spin_unlock_irqrestore(&r1fb.irq_lock, flags);
	if (ret)
		return ret;
	return 0;
}

static int compas_fb_open(struct fb_info *info, int user)
{
	(void)info;
	(void)user;
	return 0;
}

static int compas_fb_release(struct fb_info *info, int user)
{
	(void)info;
	(void)user;
	return 0;
}

static int compas_fb_check_var_fb(struct fb_var_screeninfo *var,
				  struct fb_info *info)
{
	(void)info;
	return compas_fb_check_var(var, r1fb.pixelclock_ps);
}

static int compas_fb_setcolreg_fb(unsigned int regno, unsigned int red,
				  unsigned int green, unsigned int blue,
				  unsigned int transp, struct fb_info *info)
{
	return compas_fb_setcolreg(regno, red, green, blue, transp, info);
}

static int compas_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	unsigned long length, phys, offset;
	int ret;

	ret = compas_fb_mmap_bounds(vma, COMPAS_FB_ALLOCATION_BYTES);
	if (ret)
		return ret;
	length = vma->vm_end - vma->vm_start;
	offset = vma->vm_pgoff << PAGE_SHIFT;
	if ((r1fb.memory_phys & ~PAGE_MASK) ||
	    offset > COMPAS_FB_ALLOCATION_BYTES ||
	    length > COMPAS_FB_ALLOCATION_BYTES - offset)
		return -EINVAL;
	phys = r1fb.memory_phys + offset;
	if (phys < r1fb.memory_phys || phys + length < phys)
		return -EINVAL;
	vma->vm_flags |= VM_IO | VM_DONTEXPAND | VM_DONTDUMP;
	/* Preserve the retained driver's MIPS CCA0 userspace mapping policy. */
	vma->vm_page_prot = __pgprot(pgprot_val(vma->vm_page_prot) &
					     ~_CACHE_MASK);
	return remap_pfn_range(vma, vma->vm_start, phys >> PAGE_SHIFT,
			       length, vma->vm_page_prot);
}

/* EOD wakes the process at the same boundary used by the stock driver.
 * A short bounded retry covers descriptor/site visibility at that edge; a
 * pending fence then sleeps for another IRQ, under one absolute deadline. */
static int wait_live_pan(unsigned int target,
			 struct compas_fb_hw_live_fence *fence)
{
	unsigned long deadline = jiffies +
		msecs_to_jiffies(COMPAS_FB_LIVE_PAN_TIMEOUT_MS);
	unsigned long flags, now;
	unsigned int retry;
	int ret;

	for (;;) {
		for (retry = 0; retry <= COMPAS_FB_HW_ADOPT_FAST_RETRIES; ++retry) {
			if (time_after_eq(jiffies, deadline))
				return -ETIMEDOUT;
			spin_lock_irqsave(&r1fb.irq_lock, flags);
			ret = r1fb.pan_error;
			if (!ret && (r1fb.irq_fault || !r1fb.pan.pending))
				ret = -EIO;
			if (!ret)
				ret = compas_fb_hw_sample_live_fence(&r1fb.hw_ops,
					(unsigned long)r1fb.descriptors[target] & 0x1fffffffU,
					r1fb.memory_phys + target * COMPAS_FB_HW_R1_PAGE_BYTES,
					COMPAS_FB_HW_R1_PAGE_BYTES, r1fb.pan_events, fence);
			spin_unlock_irqrestore(&r1fb.irq_lock, flags);
			if (!ret && time_after_eq(jiffies, deadline))
				return -ETIMEDOUT;
			if (ret != -EAGAIN)
				return ret;
			if (retry < COMPAS_FB_HW_ADOPT_FAST_RETRIES)
				hw_delay_us(NULL, COMPAS_FB_HW_ADOPT_FAST_DELAY_US);
		}
		now = jiffies;
		if (time_after_eq(now, deadline))
			return -ETIMEDOUT;
		if (!wait_for_completion_timeout(&r1fb.pan_done, deadline - now))
			return -ETIMEDOUT;
	}
}

static int compas_fb_pan_display(struct fb_var_screeninfo *var,
				 struct fb_info *info)
{
	unsigned long flags;
	unsigned int target;
	unsigned int token = 0;
	struct compas_fb_hw_live_fence fence;
	int ret;

	ret = compas_fb_check_var(var, r1fb.pixelclock_ps);
	if (ret)
		return ret;
	if (var->yoffset % COMPAS_FB_HEIGHT)
		return -EINVAL;
	target = var->yoffset / COMPAS_FB_HEIGHT;
	mutex_lock(&r1fb.op_mutex);
	if (!r1fb.initialized || r1fb.blanked) {
		ret = -EBUSY;
		goto out;
	}
	if (r1fb.quarantined || scanout_irq_faulted()) {
		ret = -EIO;
		goto out;
	}
	if (target == r1fb.committed_page) {
		spin_lock_irqsave(&r1fb.irq_lock, flags);
		ret = r1fb.pan.uncertain ? -EIO : 0;
		spin_unlock_irqrestore(&r1fb.irq_lock, flags);
		goto out;
	}
	/* Normal pan changes the immutable descriptor while TFT keeps running.
	 * GENERAL_STOP here interrupts physical output on every animation frame.
	 * Drain only the CPU IRQ handler; the hardware remains active. */
	disable_scanout_irq();
	if (scanout_irq_faulted()) {
		ret = -EIO;
		goto mark_uncertain;
	}
	ret = arm_hardware_events();
	if (ret)
		goto mark_uncertain;
	reinit_completion(&r1fb.pan_done);
	spin_lock_irqsave(&r1fb.irq_lock, flags);
	r1fb.pan_events = 0;
	r1fb.pan_error = 0;
	ret = compas_fb_pan_begin(&r1fb.pan, target, &token);
	spin_unlock_irqrestore(&r1fb.irq_lock, flags);
	if (ret) {
		enable_scanout_irq();
		goto out;
	}
	drain_cpu_writes();
	wmb();
	/* Keep the clear-to-submit window free of scheduler/IRQ delay on R1.
	 * Neither helper waits; physical frame progress remains independent. */
	local_irq_save(flags);
	ret = compas_fb_hw_prepare_live_fence(&r1fb.hw_ops, &fence);
	if (!ret)
		ret = compas_fb_hw_start_rdma(&r1fb.hw_ops,
			(unsigned long)r1fb.descriptors[target] & 0x1fffffffU);
	local_irq_restore(flags);
	if (ret)
		goto mark_uncertain;
	/* Fresh retiring-frame ends and target adoption are accumulated together.
	 * The TFT remains active throughout; IRQ completion avoids poll jitter. */
	enable_scanout_irq();
	ret = wait_live_pan(target, &fence);
	if (ret)
		goto mark_uncertain;
	spin_lock_irqsave(&r1fb.irq_lock, flags);
	ret = r1fb.pan_error;
	if (!ret && r1fb.irq_fault)
		ret = -EIO;
	if (!ret)
		ret = compas_fb_pan_complete(&r1fb.pan, token);
	if (!ret)
		r1fb.committed_page = r1fb.pan.committed_page;
	else
		compas_fb_pan_stop_requested(&r1fb.pan);
	if (ret) {
		r1fb.starting = false;
		r1fb.pan_error = ret;
	}
	spin_unlock_irqrestore(&r1fb.irq_lock, flags);
	if (ret)
		goto mark_uncertain;
	info->var.yoffset = target * COMPAS_FB_HEIGHT;
	enable_scanout_irq();
	goto out;

mark_uncertain:
	{
		int original_ret = ret;
		int stop_ret;
		u32 raw_status, raw_flags, raw_tft;

		/* A failed fence never advances yoffset or restarts a guessed page.
		 * Quiesce before userspace can write again; retain all resources if
		 * hardware stop is uncertain, and reject further presentation. */
		raw_status = readl(r1fb.mmio + COMPAS_FB_HW_REG_STATUS);
		raw_flags = readl(r1fb.mmio + COMPAS_FB_HW_REG_INTERRUPT_FLAG);
		raw_tft = readl(r1fb.mmio + COMPAS_FB_HW_REG_TFT_STATUS);
		if (((raw_status | raw_flags) & COMPAS_FB_HW_UNDERRUN) ||
		    (raw_tft & COMPAS_FB_HW_TFT_UNDERRUN))
			++underrun_count;
		pr_err("soc_fb: continuous pan failed %d; ST=%08x FLAG=%08x TFT=%08x\n",
		       original_ret, raw_status, raw_flags, raw_tft);
		stop_ret = stop_scanout_locked();
		quarantine_resources(stop_ret ?
			"continuous pan failed; hardware stop remains uncertain" :
			"continuous pan failed; scanout stopped safely");
		ret = stop_ret ? stop_ret : original_ret;
	}
out:
	mutex_unlock(&r1fb.op_mutex);
	return ret;
}

static int start_first_frame_locked(unsigned int page)
{
	unsigned long flags;
	unsigned long completed;
	int ret;

	drain_cpu_writes();
	reinit_completion(&r1fb.first_frame);
	spin_lock_irqsave(&r1fb.irq_lock, flags);
	r1fb.first_frame_error = 0;
	r1fb.starting = true;
	spin_unlock_irqrestore(&r1fb.irq_lock, flags);
	wmb();
	ret = compas_fb_hw_start_rdma(&r1fb.hw_ops,
		(unsigned long)r1fb.descriptors[page] & 0x1fffffffU);
	if (ret) {
		spin_lock_irqsave(&r1fb.irq_lock, flags);
		r1fb.starting = false;
		r1fb.first_frame_error = ret;
		compas_fb_pan_stop_requested(&r1fb.pan);
		spin_unlock_irqrestore(&r1fb.irq_lock, flags);
		return ret;
	}
	enable_scanout_irq();
	completed = wait_for_completion_timeout(&r1fb.first_frame,
						msecs_to_jiffies(COMPAS_FB_PAN_TIMEOUT_MS));
	spin_lock_irqsave(&r1fb.irq_lock, flags);
	if (!completed) {
		r1fb.starting = false;
		r1fb.first_frame_error = -ETIMEDOUT;
		compas_fb_pan_stop_requested(&r1fb.pan);
	}
	ret = completed ? r1fb.first_frame_error : -ETIMEDOUT;
	if (!ret && r1fb.pan.uncertain)
		ret = -EIO;
	if (ret)
		compas_fb_pan_stop_requested(&r1fb.pan);
	spin_unlock_irqrestore(&r1fb.irq_lock, flags);
	return ret;
}

static int compas_fb_blank(int blank, struct fb_info *info)
{
	unsigned long flags;
	bool power_on_attempted = false;
	int ret = 0, unblank_error;

	mutex_lock(&r1fb.op_mutex);
	if (!r1fb.initialized) {
		ret = -ENODEV;
		goto out;
	}
	if (blank == FB_BLANK_UNBLANK) {
		if (r1fb.quarantined || scanout_irq_faulted()) {
			ret = -EIO;
			goto out;
		}
		if (!r1fb.blanked)
			goto out;
		ret = enable_clocks();
		if (ret)
			goto out;
		/* From here powerdown must retry even if unblank later faults. */
		r1fb.blanked = false;
		ret = establish_idle_locked();
		if (ret)
			goto unblank_quarantine;
		if (scanout_irq_faulted()) {
			ret = -EIO;
			goto unblank_quarantine;
		}
		ret = set_pixel_clock();
		if (ret)
			goto unblank_fail;
		ret = validate_pixel_clock_rate(false);
		if (ret)
			goto unblank_fail;
		ret = clear_pixel_clock_invert();
		if (ret)
			goto unblank_fail;
		ret = select_display_pins();
		if (ret)
			goto unblank_fail;
		ret = compas_fb_hw_program_r1_tft(&r1fb.hw_ops);
		if (ret)
			goto unblank_fail;
		power_on_attempted = true;
		/* Treat a failed callback as potentially partially powered. */
		r1fb.panel_powered = true;
		ret = r1fb.panel.power_on(NULL);
		if (ret)
			goto unblank_fail;
		spin_lock_irqsave(&r1fb.irq_lock, flags);
		ret = compas_fb_pan_start(&r1fb.pan);
		spin_unlock_irqrestore(&r1fb.irq_lock, flags);
		if (ret)
			goto unblank_power_off;
		ret = start_first_frame_locked(r1fb.committed_page);
		if (ret)
			goto unblank_stop;
		r1fb.blanked = false;
		info->var.yoffset = r1fb.committed_page * COMPAS_FB_HEIGHT;
		goto out;
unblank_stop:
		unblank_error = ret;
		ret = stop_scanout_locked();
		if (ret)
			goto unblank_quarantine;
		ret = unblank_error;
unblank_power_off:
unblank_fail:
		unblank_error = ret;
		ret = select_power_off_pins();
		if (ret) {
			quarantine_resources("cannot park pins after failed unblank");
			goto out;
		}
		if (power_on_attempted) {
			ret = r1fb.panel.power_off(NULL);
			if (ret) {
				quarantine_resources("panel power-off failed after failed unblank");
				goto out;
			}
			r1fb.panel_powered = false;
		}
		disable_clocks();
		r1fb.blanked = true;
		ret = unblank_error;
		goto out;
	unblank_quarantine:
		r1fb.quarantined = true;
		if (!r1fb.pinned) {
			__module_get(THIS_MODULE);
			r1fb.pinned = true;
		}
		goto out;
	}
	if (r1fb.blanked)
		goto out;
	ret = stop_scanout_locked();
	if (ret)
		goto out;
	ret = select_power_off_pins();
	if (ret) {
		int restore_ret = select_display_pins();

		spin_lock_irqsave(&r1fb.irq_lock, flags);
		if (!restore_ret)
			restore_ret = compas_fb_pan_start(&r1fb.pan);
		spin_unlock_irqrestore(&r1fb.irq_lock, flags);
		if (!restore_ret)
			restore_ret = start_first_frame_locked(r1fb.committed_page);
		if (restore_ret) {
			quarantine_resources("failed to restore scanout after pin error");
			ret = restore_ret;
		}
		goto out;
	}
	if (r1fb.panel_powered) {
		ret = r1fb.panel.power_off(NULL);
		if (!ret)
			r1fb.panel_powered = false;
	} else {
		ret = 0;
	}
	if (ret) {
		/* Leave clocks and pins on when the panel did not power off. */
		int restore_ret = select_display_pins();

		spin_lock_irqsave(&r1fb.irq_lock, flags);
		if (!restore_ret)
			restore_ret = compas_fb_pan_start(&r1fb.pan);
		spin_unlock_irqrestore(&r1fb.irq_lock, flags);
		if (!restore_ret)
			restore_ret = start_first_frame_locked(r1fb.committed_page);
		if (restore_ret) {
			quarantine_resources("failed to restore scanout after panel error");
			ret = restore_ret;
		}
		goto out;
	}
	r1fb.blanked = true;
	disable_clocks();
out:
	mutex_unlock(&r1fb.op_mutex);
	return ret;
}

static struct fb_ops compas_fb_ops = {
	.owner = THIS_MODULE,
	.fb_open = compas_fb_open,
	.fb_release = compas_fb_release,
	.fb_check_var = compas_fb_check_var_fb,
	.fb_setcolreg = compas_fb_setcolreg_fb,
	.fb_pan_display = compas_fb_pan_display,
	.fb_blank = compas_fb_blank,
	.fb_mmap = compas_fb_mmap,
};

static int compas_fb_pm_notify(struct notifier_block *notifier,
			       unsigned long event, void *unused)
{
	struct fb_info *info;
	bool should_blank = false, should_restore = false;
	int ret = 0;

	(void)notifier;
	(void)unused;
	switch (event) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		mutex_lock(&r1fb.op_mutex);
		info = r1fb.initialized ? r1fb.info : NULL;
		mutex_unlock(&r1fb.op_mutex);
		if (!info)
			return NOTIFY_OK;
		console_lock();
		if (!lock_fb_info(info)) {
			console_unlock();
			return NOTIFY_BAD;
		}
		mutex_lock(&r1fb.op_mutex);
		should_blank = r1fb.initialized && !r1fb.blanked;
		mutex_unlock(&r1fb.op_mutex);
		if (should_blank)
			ret = fb_blank(info, FB_BLANK_POWERDOWN);
		mutex_lock(&r1fb.op_mutex);
		if (should_blank && !ret)
			r1fb.pm_blank_requested = true;
		mutex_unlock(&r1fb.op_mutex);
		unlock_fb_info(info);
		console_unlock();
		if (ret)
			return NOTIFY_BAD;
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		mutex_lock(&r1fb.op_mutex);
		info = (r1fb.initialized && r1fb.pm_blank_requested) ?
			r1fb.info : NULL;
		mutex_unlock(&r1fb.op_mutex);
		if (!info)
			return NOTIFY_OK;
		console_lock();
		if (!lock_fb_info(info)) {
			console_unlock();
			return NOTIFY_BAD;
		}
		mutex_lock(&r1fb.op_mutex);
		should_restore = r1fb.pm_blank_requested && r1fb.initialized;
		mutex_unlock(&r1fb.op_mutex);
		if (should_restore)
			ret = fb_blank(info, FB_BLANK_UNBLANK);
		mutex_lock(&r1fb.op_mutex);
		r1fb.pm_blank_requested = false;
		mutex_unlock(&r1fb.op_mutex);
		unlock_fb_info(info);
		console_unlock();
		if (ret)
			pr_err("soc_fb: framebuffer restore after resume failed: %d\n",
			       ret);
		return ret ? NOTIFY_BAD : NOTIFY_OK;
	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block compas_fb_pm_notifier = {
	.notifier_call = compas_fb_pm_notify,
};

static int setup_framebuffer(struct lcdc_data *panel)
{
	struct fb_info *info;
	struct compas_fb_geometry geometry;
	unsigned int i;
	int ret;

	ret = compas_fb_geometry_init(&geometry, COMPAS_FB_WIDTH,
				      COMPAS_FB_HEIGHT,
				      COMPAS_FB_WIDTH * 2U, COMPAS_FB_PAGES);
	if (ret)
		return ret;
	r1fb.memory_bytes = COMPAS_FB_ALLOC_BYTES;
	r1fb.memory_kseg0 = rmem_alloc_aligned(r1fb.memory_bytes, PAGE_SIZE);
	if (!r1fb.memory_kseg0)
		return -ENOMEM;
	if (r1fb.memory_kseg0 < 0x80000000UL ||
	    r1fb.memory_kseg0 >= 0xa0000000UL) {
		ret = -ERANGE;
		goto fail_memory;
	}
	r1fb.memory_phys = r1fb.memory_kseg0 & 0x1fffffffUL;
	if (r1fb.memory_phys >= COMPAS_FB_HW_PHYS_LIMIT ||
	    COMPAS_FB_ALLOC_BYTES > COMPAS_FB_HW_PHYS_LIMIT - r1fb.memory_phys ||
	    (r1fb.memory_phys & (PAGE_SIZE - 1))) {
		ret = -ERANGE;
		goto fail_memory;
	}
	memset((void *)r1fb.memory_kseg0, 0, r1fb.memory_bytes);
	for (i = 0; i < COMPAS_FB_PAGES; ++i) {
		unsigned long offset = COMPAS_FB_ALLOCATION_BYTES +
				       i * COMPAS_FB_HW_DESC_SLOT_BYTES;
		unsigned long desc_kseg0 = r1fb.memory_kseg0 + offset;
		unsigned long desc_phys = r1fb.memory_phys + offset;
		compas_fb_u32 page_phys;

		if ((desc_kseg0 & (COMPAS_FB_HW_DESC_ALIGN - 1)) ||
		    desc_phys >= COMPAS_FB_HW_PHYS_LIMIT) {
			ret = -ERANGE;
			goto fail_memory;
		}
		r1fb.descriptors[i] = (void *)desc_kseg0;
		ret = compas_fb_dma_page_address(&geometry, r1fb.memory_phys,
						 COMPAS_FB_ALLOCATION_BYTES,
						 i, &page_phys);
		if (ret)
			goto fail_memory;
		ret = compas_fb_hw_encode_r1_rgb565_descriptor(
			r1fb.descriptors[i], desc_phys, page_phys,
			COMPAS_FB_WIDTH);
		if (ret)
			goto fail_memory;
	}
	flush_dma_range((void *)r1fb.memory_kseg0, r1fb.memory_bytes);
	info = framebuffer_alloc(0, NULL);
	if (!info) {
		ret = -ENOMEM;
		goto fail_memory;
	}
	r1fb.info = info;
	info->screen_base = (char __iomem *)(0xa0000000UL |
						     r1fb.memory_phys);
	info->screen_size = COMPAS_FB_ALLOCATION_BYTES;
	info->pseudo_palette = r1fb.pseudo_palette;
	info->fbops = &compas_fb_ops;
	info->flags = FBINFO_DEFAULT;
	strlcpy(info->fix.id, "Compas R1 LCD", sizeof(info->fix.id));
	info->fix.smem_start = r1fb.memory_phys;
	info->fix.smem_len = COMPAS_FB_ALLOCATION_BYTES;
	info->fix.type = FB_TYPE_PACKED_PIXELS;
	info->fix.visual = FB_VISUAL_TRUECOLOR;
	info->fix.xpanstep = 0;
	info->fix.ypanstep = COMPAS_FB_HEIGHT;
	info->fix.ywrapstep = 0;
	info->fix.line_length = COMPAS_FB_WIDTH * COMPAS_FB_BYTES_PER_PIXEL;
	info->fix.accel = FB_ACCEL_NONE;
	info->var.xres = COMPAS_FB_WIDTH;
	info->var.yres = COMPAS_FB_HEIGHT;
	info->var.xres_virtual = COMPAS_FB_WIDTH;
	info->var.yres_virtual = COMPAS_FB_VIRTUAL_HEIGHT;
	info->var.bits_per_pixel = 16;
	info->var.pixclock = r1fb.pixelclock_ps;
	info->var.left_margin = panel->left_margin;
	info->var.right_margin = panel->right_margin;
	info->var.upper_margin = panel->upper_margin;
	info->var.lower_margin = panel->lower_margin;
	info->var.hsync_len = panel->hsync_len;
	info->var.vsync_len = panel->vsync_len;
	info->var.sync = FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT;
	info->var.red.offset = 11;
	info->var.red.length = 5;
	info->var.green.offset = 5;
	info->var.green.length = 6;
	info->var.blue.offset = 0;
	info->var.blue.length = 5;
	info->var.transp.length = 0;
	info->var.activate = FB_ACTIVATE_NOW;
	info->var.vmode = FB_VMODE_NONINTERLACED;
	return 0;

fail_memory:
	rmem_free(r1fb.memory_kseg0, r1fb.memory_bytes);
	r1fb.memory_kseg0 = 0;
	return ret;
}

static int validate_panel(const struct lcdc_data *panel)
{
	unsigned int i;

	if (!panel || !panel->name || !panel->power_on || !panel->power_off ||
	    panel->xres != COMPAS_FB_WIDTH || panel->yres != COMPAS_FB_HEIGHT ||
	    panel->refresh != 62 || panel->pixclock != 0 ||
	    panel->left_margin != 24 || panel->right_margin != 24 ||
	    panel->upper_margin != 8 || panel->lower_margin != 14 ||
	    panel->hsync_len != 24 || panel->vsync_len != 5 ||
	    panel->fb_fmt != 1 || panel->lcd_mode != 0 ||
	    panel->out_format != 1 || panel->color_even != 2 ||
	    panel->color_odd != 2 || panel->pix_clk_active != 1 ||
	    panel->de_active_level != 1 || panel->hsync_active_level != 1 ||
	    panel->vsync_active_level != 1 ||
	    panel->optional_ioctl_callback || panel->optional_disable_callback)
		return -EINVAL;
	for (i = 0; i < ARRAY_SIZE(panel->smart_lcd); ++i) {
		if (panel->smart_lcd[i])
			return -EINVAL;
	}
	return 0;
}

static struct module *pin_panel_owner(const struct lcdc_data *panel)
{
	struct module *owner, *power_off_owner;

	/* module_address() requires preemption to be disabled by its caller. */
	preempt_disable();
	owner = __module_address((unsigned long)panel->power_on);
	power_off_owner = __module_address((unsigned long)panel->power_off);
	if (!owner || owner != power_off_owner || !try_module_get(owner))
		owner = NULL;
	preempt_enable();
	return owner;
}

static int release_init_resources(void)
{
	if (r1fb.irq_requested) {
		if (r1fb.mmio) {
			writel(0, r1fb.mmio + COMPAS_FB_HW_REG_INTERRUPT_MASK);
			if (readl(r1fb.mmio + COMPAS_FB_HW_REG_INTERRUPT_MASK) != 0) {
				disable_scanout_irq();
				return -EIO;
			}
		}
		disable_scanout_irq();
		free_irq(COMPAS_FB_IRQ, &r1fb);
		spin_lock_irq(&r1fb.irq_lock);
		r1fb.irq_requested = false;
		r1fb.irq_disabled = false;
		r1fb.irq_fault = false;
		spin_unlock_irq(&r1fb.irq_lock);
	}
	if (r1fb.info) {
		framebuffer_release(r1fb.info);
		r1fb.info = NULL;
	}
	if (r1fb.memory_kseg0) {
		rmem_free(r1fb.memory_kseg0, r1fb.memory_bytes);
		r1fb.memory_kseg0 = 0;
	}
	if (r1fb.mmio) {
		iounmap(r1fb.mmio);
		r1fb.mmio = NULL;
	}
	if (r1fb.memory_region_requested) {
		release_mem_region(COMPAS_FB_MMIO_PHYS, COMPAS_FB_MMIO_SIZE);
		r1fb.memory_region_requested = false;
	}
	disable_clocks();
	if (r1fb.pixel_clk) {
		clk_put(r1fb.pixel_clk);
		r1fb.pixel_clk = NULL;
	}
	if (r1fb.gate_clk) {
		clk_put(r1fb.gate_clk);
		r1fb.gate_clk = NULL;
	}
	release_display_pins();
	return 0;
}

static void quarantine_resources(const char *reason)
{
	r1fb.quarantined = true;
	if (!r1fb.pinned) {
		__module_get(THIS_MODULE);
		r1fb.pinned = true;
	}
	pr_err("soc_fb: retaining resources in quarantine: %s\n", reason);
}

int jzfb_register_lcd(struct lcdc_data *panel)
{
	struct module *panel_owner;
	bool panel_power_attempted = false;
	int ret, original_ret;

	compas_lcdc_abi_assert();
	mutex_lock(&r1fb.op_mutex);
	if (r1fb.quarantined) {
		ret = -EIO;
		goto out;
	}
	if (r1fb.registering || r1fb.initialized || r1fb.fb_registered ||
	    r1fb.panel.name) {
		ret = -EBUSY;
		goto out;
	}
	ret = validate_panel(panel);
	if (ret)
		goto out;
	panel_owner = pin_panel_owner(panel);
	if (!panel_owner) {
		ret = -ENODEV;
		goto out;
	}
	r1fb.panel_owner = panel_owner;
	r1fb.panel_owner_pinned = true;
	r1fb.panel = *panel;
	r1fb.panel_source = panel;
	ret = request_display_pins();
	if (ret)
		goto fail;
	r1fb.gate_clk = clk_get(NULL, "gate_lcd");
	if (IS_ERR(r1fb.gate_clk)) {
		ret = PTR_ERR(r1fb.gate_clk);
		r1fb.gate_clk = NULL;
		goto fail;
	}
	r1fb.pixel_clk = clk_get(NULL, "div_lcd");
	if (IS_ERR(r1fb.pixel_clk)) {
		ret = PTR_ERR(r1fb.pixel_clk);
		r1fb.pixel_clk = NULL;
		goto fail;
	}
	ret = enable_clocks();
	if (ret)
		goto fail;
	if (!request_mem_region(COMPAS_FB_MMIO_PHYS, COMPAS_FB_MMIO_SIZE,
				"soc_fb")) {
		ret = -EBUSY;
		quarantine_resources("DPU MMIO region is already owned");
		goto out;
	}
	r1fb.memory_region_requested = true;
	r1fb.mmio = ioremap(COMPAS_FB_MMIO_PHYS, COMPAS_FB_MMIO_SIZE);
	if (!r1fb.mmio) {
		ret = -ENOMEM;
		quarantine_resources("cannot map DPU registers before idle proof");
		goto out;
	}
	r1fb.hw_ops.context = &r1fb;
	r1fb.hw_ops.read = hw_read;
	r1fb.hw_ops.write = hw_write;
	r1fb.hw_ops.delay_us = hw_delay_us;
	ret = request_irq(COMPAS_FB_IRQ, compas_fb_irq, 0, "soc_fb", &r1fb);
	if (ret) {
		quarantine_resources("cannot claim DPU IRQ before idle proof");
		goto out;
	}
	r1fb.irq_requested = true;
	disable_scanout_irq();
	/* Clocks are live; expose stop ACK while the Linux IRQ stays disabled. */
	writel(interrupt_events(), r1fb.mmio +
	       COMPAS_FB_HW_REG_INTERRUPT_MASK);
	(void)readl(r1fb.mmio + COMPAS_FB_HW_REG_INTERRUPT_MASK);
	ret = establish_idle_locked();
	if (ret) {
		quarantine_resources("initial DPU idle/stop could not be proven");
		goto out;
	}
	if (scanout_irq_faulted()) {
		ret = -EIO;
		goto fail;
	}
	ret = set_pixel_clock();
	if (ret)
		goto fail;
	ret = validate_pixel_clock_rate(true);
	if (ret)
		goto fail;
	ret = clear_pixel_clock_invert();
	if (ret)
		goto fail;
	ret = select_display_pins();
	if (ret)
		goto fail;
	ret = compas_fb_hw_program_r1_tft(&r1fb.hw_ops);
	if (ret)
		goto fail;
	ret = setup_framebuffer(&r1fb.panel);
	if (ret)
		goto fail;
	ret = compas_fb_pan_init(&r1fb.pan, 0);
	if (ret)
		goto fail;
	panel_power_attempted = true;
	/* Keep rollback conservative if power_on partially succeeds then errors. */
	r1fb.panel_powered = true;
	ret = r1fb.panel.power_on(NULL);
	if (ret)
		goto fail_panel_power;
	ret = compas_fb_pan_start(&r1fb.pan);
	if (ret)
		goto fail_panel_power;
	ret = start_first_frame_locked(0);
	if (ret)
		goto fail_active_scanout;
	/* fb core can call our ops while publishing the framebuffer.  Do not
	 * hold op_mutex across that publication: fb core lock order is the
	 * inverse of the driver's operation lock order.  All stateful callbacks
	 * are gated by initialized until publication has completed. */
	r1fb.registering = true;
	mutex_unlock(&r1fb.op_mutex);
	ret = register_framebuffer(r1fb.info);
	mutex_lock(&r1fb.op_mutex);
	r1fb.registering = false;
	if (ret)
		goto fail_active_scanout;
	r1fb.fb_registered = true;
	r1fb.initialized = true;
	r1fb.blanked = false;
	lcd_is_inited = 1;
	if (!r1fb.pinned) {
		__module_get(THIS_MODULE);
		r1fb.pinned = true;
	}
	ret = 0;
	goto out;

fail_active_scanout:
	original_ret = ret;
	ret = stop_scanout_locked();
	if (ret) {
		quarantine_resources("startup scanout could not be stopped safely");
		ret = original_ret;
		goto out;
	}
	ret = original_ret;
fail_panel_power:
	original_ret = ret;
	ret = select_power_off_pins();
	if (ret) {
		quarantine_resources("cannot safely park panel pins after startup");
		goto out;
	}
	if (panel_power_attempted) {
		ret = r1fb.panel.power_off(NULL);
		if (ret) {
			quarantine_resources("panel power-off failed during startup rollback");
			goto out;
		}
		r1fb.panel_powered = false;
	}
	ret = original_ret;
fail:
	original_ret = ret;
	ret = release_init_resources();
	if (ret) {
		quarantine_resources("cannot mask DPU IRQ for safe init cleanup");
		ret = original_ret;
		goto out;
	}
	if (r1fb.panel_owner_pinned) {
		module_put(r1fb.panel_owner);
		r1fb.panel_owner = NULL;
		r1fb.panel_owner_pinned = false;
	}
	memset(&r1fb.panel, 0, sizeof(r1fb.panel));
	r1fb.panel_source = NULL;
	ret = original_ret;
out:
	mutex_unlock(&r1fb.op_mutex);
	return ret;
}
EXPORT_SYMBOL(jzfb_register_lcd);

void jzfb_unregister_lcd(struct lcdc_data *panel)
{
	mutex_lock(&r1fb.op_mutex);
	if (r1fb.panel_source == panel)
		pr_err("soc_fb: refusing panel unregister; scanout and mmap lifetime are permanent\n");
	else
		pr_err("soc_fb: refusing unknown panel unregister request\n");
	mutex_unlock(&r1fb.op_mutex);
}
EXPORT_SYMBOL(jzfb_unregister_lcd);

int slcd_read_data_only(unsigned int command, void *buffer, unsigned int length)
{
	(void)command;
	(void)buffer;
	(void)length;
	return -EOPNOTSUPP;
}
EXPORT_SYMBOL(slcd_read_data_only);

static int __init compas_fb_init(void)
{
	int ret;

	if (frame_num != COMPAS_FB_PAGES || pan_display_sync != 1 ||
	    lcd_is_inited != 0)
		return -EINVAL;
	mutex_init(&r1fb.op_mutex);
	spin_lock_init(&r1fb.irq_lock);
	init_completion(&r1fb.pan_done);
	init_completion(&r1fb.first_frame);
	r1fb.hw_ops.context = &r1fb;
	ret = register_pm_notifier(&compas_fb_pm_notifier);
	if (!ret) {
		__module_get(THIS_MODULE);
		r1fb.pinned = true;
	}
	return ret;
}
module_init(compas_fb_init);

MODULE_DESCRIPTION("Experimental persistent R1 X1600 RGB565 framebuffer");
MODULE_AUTHOR("Compas Player contributors");
MODULE_LICENSE("GPL");
