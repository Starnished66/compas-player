#!/usr/bin/env python3
"""Compile the real continuous-pan callback against stateful host stubs."""

from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "firmware/kernel/display-experimental/compas_fb_driver.c"

PREFIX = r"""
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint32_t u32;
#define COMPAS_FB_HEIGHT 800U
#define COMPAS_FB_HW_R1_PAGE_BYTES 768000U
#define COMPAS_FB_HW_ADOPT_POLLS 100U
#define COMPAS_FB_HW_ADOPT_FAST_RETRIES 4U
#define COMPAS_FB_HW_ADOPT_FAST_DELAY_US 25U
#define COMPAS_FB_LIVE_PAN_TIMEOUT_MS 100U
#define COMPAS_FB_HW_DESC_SLOT_BYTES 64U
#define COMPAS_FB_HW_DESC_ALIGN 64U
#define COMPAS_FB_HW_PHYS_LIMIT 0x20000000U
#define COMPAS_FB_HW_REG_STATUS 0x2004U
#define COMPAS_FB_HW_REG_INTERRUPT_FLAG 0x2010U
#define COMPAS_FB_HW_REG_CLEAR_STATUS 0x2008U
#define COMPAS_FB_HW_REG_INTERRUPT_MASK 0x200cU
#define COMPAS_FB_HW_REG_TFT_STATUS 0x9014U
#define COMPAS_FB_HW_UNDERRUN (1U << 8)
#define COMPAS_FB_HW_SRD_END (1U << 1)
#define COMPAS_FB_HW_FRAME_END (1U << 17)
#define COMPAS_FB_HW_STOP_ACK (1U << 7)
#define COMPAS_FB_HW_TFT_UNDERRUN (1U << 0)
#define THIS_MODULE ((void *)0)
typedef int irqreturn_t;
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define COMPAS_FB_IRQ 39

struct mutex { int unused; };
struct spinlock { int unused; };
struct completion { int unused; };
struct compas_fb_hw_live_fence { u32 events, adopted; };
struct compas_fb_pan_state {
	unsigned int committed_page, submitted_page, generation;
	bool pending, uncertain, running;
};
struct compas_fb_hw_ops { int unused; };
struct fb_var_screeninfo { unsigned int yoffset; bool invalid; };
struct fb_info { struct fb_var_screeninfo var; };
struct compas_fb_device {
	struct mutex op_mutex;
	struct spinlock irq_lock;
	struct completion pan_done;
	struct completion first_frame;
	struct compas_fb_pan_state pan;
	struct compas_fb_hw_ops hw_ops;
	void *mmio;
	unsigned long memory_phys;
	void *descriptors[2];
	unsigned int committed_page, pixelclock_ps;
	u32 pan_events;
	bool initialized, blanked, quarantined, starting;
	bool irq_fault;
	bool irq_requested, irq_disabled;
	int first_frame_error;
	int pan_error;
};

static struct compas_fb_device r1fb;
static unsigned long underrun_count;
static int calls[64], call_count;
enum { CALL_DISABLE = 1, CALL_ARM, CALL_BEGIN, CALL_DRAIN, CALL_WMB,
	CALL_START, CALL_WAIT, CALL_SAMPLE, CALL_COMPLETE, CALL_ENABLE, CALL_STOP,
	CALL_QUARANTINE };
static int injected_arm, injected_start, injected_prepare, injected_sample, injected_complete;
static int injected_wakeup_timeout;
static int injected_wakeup_expire;
static int injected_stop, inject_fault_on_disable, raw_status, raw_flags, raw_tft;
static int arm_calls, stop_calls, quarantine_calls, clear_calls, hw_write_calls;
static unsigned int seen_begin_page, seen_token;
static unsigned long seen_descriptor;
static unsigned long seen_target_page;
static unsigned int seen_page_bytes;
static int prepare_calls, sample_calls, wakeup_calls;
static int sample_again_count;
static unsigned long sample_advance;
static u32 irq_remaining, irq_writes[8], raw_mask, mask_writes[8];
static int irq_write_count, mask_write_count, complete_calls;

#define mutex_lock(p) ((void)(p))
#define mutex_unlock(p) ((void)(p))
#define spin_lock_irqsave(p, f) do { (void)(p); (void)sizeof(f); } while (0)
#define spin_unlock_irqrestore(p, f) do { (void)(p); (void)sizeof(f); } while (0)
#define local_irq_save(f) do { (void)sizeof(f); } while (0)
#define local_irq_restore(f) do { (void)sizeof(f); } while (0)
#define wmb() do { calls[call_count++] = CALL_WMB; } while (0)
#define pr_err(...) ((void)0)

static void record_call(int call) { calls[call_count++] = call; }
static int compas_fb_check_var(struct fb_var_screeninfo *var, unsigned int pclk)
{ (void)pclk; return var->invalid ? -EINVAL : 0; }
static bool scanout_irq_faulted(void) { return r1fb.irq_fault; }
static void disable_scanout_irq(void)
{
	record_call(CALL_DISABLE);
	if (inject_fault_on_disable) r1fb.irq_fault = true;
}
static int arm_hardware_events(void)
{ ++arm_calls; record_call(CALL_ARM); raw_mask = COMPAS_FB_HW_FRAME_END |
	COMPAS_FB_HW_UNDERRUN | COMPAS_FB_HW_STOP_ACK; return injected_arm; }
static void enable_scanout_irq(void) { record_call(CALL_ENABLE); }
static unsigned long jiffies;
static unsigned long msecs_to_jiffies(unsigned int ms) { return ms; }
static int time_after_eq(unsigned long a, unsigned long b) { return a >= b; }
static unsigned long wait_for_completion_timeout(struct completion *c, unsigned long t)
{ (void)c; (void)t; ++wakeup_calls; record_call(CALL_WAIT); if (injected_wakeup_timeout) { jiffies += 100; return 0; } if (injected_wakeup_expire) { jiffies += 100; return 1; } ++jiffies; return 1; }
static void reinit_completion(struct completion *c) { (void)c; }
static void drain_cpu_writes(void) { record_call(CALL_DRAIN); }
static u32 interrupt_events(void) { return COMPAS_FB_HW_FRAME_END | COMPAS_FB_HW_UNDERRUN | COMPAS_FB_HW_STOP_ACK; }
static void complete(struct completion *c) { (void)c; ++complete_calls; }
static int compas_fb_pan_timeout(struct compas_fb_pan_state *s, unsigned int token)
{ (void)token; s->pending = false; return -ETIMEDOUT; }
static void disable_irq_nosync(int irq) { (void)irq; }
static void pr_warn_ratelimited(const char *format, ...) { (void)format; }
static void hw_delay_us(void *context, u32 usec) { (void)context; (void)usec; ++jiffies; }
static int compas_fb_pan_begin(struct compas_fb_pan_state *state,
			       unsigned int page, unsigned int *token)
{
	record_call(CALL_BEGIN); seen_begin_page = page;
	if (state->pending) return -EBUSY;
	state->pending = true; state->submitted_page = page;
	*token = 17; return 0;
}
static int compas_fb_hw_start_rdma(struct compas_fb_hw_ops *ops,
				   unsigned long desc)
{
	(void)ops; record_call(CALL_START); seen_descriptor = desc;
	return injected_start;
}
static int compas_fb_hw_prepare_live_fence(struct compas_fb_hw_ops *ops,
		struct compas_fb_hw_live_fence *fence)
{
	(void)ops; (void)fence; ++prepare_calls; record_call(CALL_ARM);
	return injected_prepare;
}
static int compas_fb_hw_sample_live_fence(struct compas_fb_hw_ops *ops,
		unsigned long desc, unsigned long page, unsigned int bytes,
		u32 events, struct compas_fb_hw_live_fence *fence)
{
	(void)ops; (void)events; (void)fence; ++sample_calls; record_call(CALL_SAMPLE);
	seen_descriptor = desc; seen_target_page = page; seen_page_bytes = bytes;
	jiffies += sample_advance;
	if (sample_again_count > 0) { --sample_again_count; return -EAGAIN; }
	return injected_sample;
}
static int compas_fb_pan_complete(struct compas_fb_pan_state *state,
				  unsigned int token)
{
	record_call(CALL_COMPLETE); seen_token = token;
	if (injected_complete) return injected_complete;
	state->pending = false; state->committed_page = state->submitted_page;
	return 0;
}
static void compas_fb_pan_stop_requested(struct compas_fb_pan_state *state)
{ state->uncertain = true; state->pending = false; }
static u32 readl(void *address)
{
	uintptr_t offset = (uintptr_t)address - (uintptr_t)r1fb.mmio;
	if (offset == COMPAS_FB_HW_REG_STATUS) return (u32)raw_status;
	if (offset == COMPAS_FB_HW_REG_INTERRUPT_FLAG) return (u32)raw_flags;
	if (offset == COMPAS_FB_HW_REG_TFT_STATUS) return (u32)raw_tft;
	if (offset == COMPAS_FB_HW_REG_INTERRUPT_MASK) return raw_mask;
	return 0;
}
static void writel(u32 value, void *address)
{
	uintptr_t offset = (uintptr_t)address - (uintptr_t)r1fb.mmio;
	if (offset == COMPAS_FB_HW_REG_CLEAR_STATUS) {
		if (irq_write_count < 8) irq_writes[irq_write_count++] = value;
		raw_flags = (int)irq_remaining;
	} else if (offset == COMPAS_FB_HW_REG_INTERRUPT_MASK) {
		if (mask_write_count < 8) mask_writes[mask_write_count++] = value;
		raw_mask = value;
	}
}
static int stop_scanout_locked(void)
{
	++stop_calls; record_call(CALL_STOP);
	compas_fb_pan_stop_requested(&r1fb.pan);
	return injected_stop;
}
static void quarantine_resources(const char *reason)
{ (void)reason; ++quarantine_calls; r1fb.quarantined = true; record_call(CALL_QUARANTINE); }
"""

SUFFIX = r"""
static void expect(int condition, const char *message)
{
	if (!condition) { fprintf(stderr, "FAIL: %s\n", message); exit(1); }
}
static void reset_case(void)
{
	static unsigned int descriptor[2];
	static unsigned char fake_mmio[0x10000];
	r1fb = (struct compas_fb_device){0};
	r1fb.initialized = true;
	r1fb.committed_page = 0;
	r1fb.pan.committed_page = 0;
	r1fb.memory_phys = 0x01000000UL;
	r1fb.descriptors[0] = &descriptor[0];
	r1fb.descriptors[1] = &descriptor[1];
	r1fb.mmio = fake_mmio;
	call_count = arm_calls = stop_calls = quarantine_calls = 0;
	prepare_calls = sample_calls = wakeup_calls = sample_again_count = 0;
	irq_write_count = complete_calls = 0; irq_remaining = 0;
	mask_write_count = 0; raw_mask = 0; underrun_count = 0;
	clear_calls = hw_write_calls = 0;
	injected_arm = injected_start = injected_prepare = injected_sample = injected_complete = 0;
	injected_stop = inject_fault_on_disable = 0;
	injected_wakeup_timeout = injected_wakeup_expire = 0; jiffies = 0;
	sample_advance = 0;
	raw_status = raw_flags = raw_tft = 0;
	seen_begin_page = seen_token = 0;
	seen_descriptor = seen_target_page = 0; seen_page_bytes = 0;
}
static int has_call(int wanted)
{
	int i; for (i = 0; i < call_count; ++i) if (calls[i] == wanted) return 1;
	return 0;
}
static int call_index(int wanted)
{
	int i; for (i = 0; i < call_count; ++i) if (calls[i] == wanted) return i;
	return -1;
}
int main(void)
{
	struct fb_info info = {0};
	struct fb_var_screeninfo request = {0};
	int ret, expected;
	unsigned long descriptor_target;

	/* Live pan adopts and fences the new page without stopping scanout. */
	reset_case(); request.yoffset = COMPAS_FB_HEIGHT;
	ret = compas_fb_pan_display(&request, &info);
	expected = (int)((uintptr_t)r1fb.descriptors[1] & 0x1fffffffU);
	descriptor_target = (unsigned long)r1fb.descriptors[1] & 0x1fffffffU;
	expect(ret == 0 && request.yoffset == COMPAS_FB_HEIGHT &&
	       info.var.yoffset == COMPAS_FB_HEIGHT &&
	       r1fb.committed_page == 1 && r1fb.pan.committed_page == 1,
	       "successful pan commits userspace and core page");
	expect(seen_begin_page == 1 && seen_token == 17 &&
	       seen_descriptor == descriptor_target &&
	       seen_target_page == r1fb.memory_phys + COMPAS_FB_HW_R1_PAGE_BYTES &&
	       seen_page_bytes == COMPAS_FB_HW_R1_PAGE_BYTES && sample_calls == 1,
	       "begin/start/fence receive target descriptor and page");
	expect(call_index(CALL_BEGIN) < call_index(CALL_START) &&
	       call_index(CALL_START) < call_index(CALL_ENABLE) &&
	       call_index(CALL_ENABLE) < call_index(CALL_SAMPLE) &&
	       call_index(CALL_SAMPLE) < call_index(CALL_COMPLETE),
	       "target begins, starts, fences, completes, then IRQs re-enable");
	expect(arm_calls == 1 && stop_calls == 0 && clear_calls == 0 &&
	       hw_write_calls == 0 && !has_call(CALL_STOP),
	       "success keeps scanout running and does not stop or clear underrun");
	expect(expected == (int)descriptor_target, "test descriptor address is representable");

	/* A same-page request is a true no-op with no MMIO or IRQ operation. */
	reset_case(); r1fb.committed_page = 1; r1fb.pan.committed_page = 1;
	request = (struct fb_var_screeninfo){ .yoffset = COMPAS_FB_HEIGHT };
	info.var.yoffset = COMPAS_FB_HEIGHT;
	ret = compas_fb_pan_display(&request, &info);
	expect(ret == 0 && call_count == 0 && arm_calls == 0 && stop_calls == 0,
	       "same-page request avoids MMIO, stop, and IRQ operations");

	/* An invalid mode is rejected before locks or hardware operations. */
	reset_case(); request = (struct fb_var_screeninfo){
		.yoffset = COMPAS_FB_HEIGHT, .invalid = true };
	info.var.yoffset = 0;
	ret = compas_fb_pan_display(&request, &info);
	expect(ret == -EINVAL && call_count == 0 && arm_calls == 0 && stop_calls == 0,
	       "invalid mode leaves hardware untouched");

	/* Fence failure cannot advance the page; stop and quarantine contain it. */
	reset_case(); request = (struct fb_var_screeninfo){ .yoffset = COMPAS_FB_HEIGHT };
	info.var.yoffset = 0; injected_sample = -ETIMEDOUT;
	ret = compas_fb_pan_display(&request, &info);
	expect(ret == -ETIMEDOUT && info.var.yoffset == 0 &&
	       r1fb.committed_page == 0 && r1fb.pan.committed_page == 0 &&
	       stop_calls == 1 && quarantine_calls == 1 && r1fb.quarantined,
	       "helper failure stops and quarantines without page advance");

	/* Failed containment retains the original stop error and uncertain state. */
	reset_case(); request = (struct fb_var_screeninfo){ .yoffset = COMPAS_FB_HEIGHT };
	info.var.yoffset = 0; injected_sample = -ETIMEDOUT;
	injected_stop = -EIO;
	ret = compas_fb_pan_display(&request, &info);
	expect(ret == -EIO && r1fb.pan.uncertain && r1fb.quarantined &&
	       info.var.yoffset == 0 && r1fb.committed_page == 0,
	       "failed stop retains uncertain resources and preserves page");

	/* An IRQ fault arriving during disable is caught before event arming. */
	reset_case(); request = (struct fb_var_screeninfo){ .yoffset = COMPAS_FB_HEIGHT };
	info.var.yoffset = 0;
	inject_fault_on_disable = 1;
	ret = compas_fb_pan_display(&request, &info);
	expect(ret == -EIO && arm_calls == 0 && stop_calls == 1 &&
	       quarantine_calls == 1 && info.var.yoffset == 0,
	       "post-disable IRQ fault contains before nonzero event mask");

	/* A completion that arrives before sleeping is consumed without loss. */
	reset_case(); r1fb.pan.pending = true; sample_again_count = 5;
	ret = wait_live_pan(1, &(struct compas_fb_hw_live_fence){0});
	expect(ret == 0 && sample_calls == 6 && wakeup_calls == 1,
	       "pending fence waits once and observes completion after wake");

	/* The absolute deadline bounds repeated pending samples and wakeups. */
	reset_case(); r1fb.pan.pending = true; sample_again_count = 100; injected_wakeup_timeout = 1;
	ret = wait_live_pan(1, &(struct compas_fb_hw_live_fence){0});
	expect(ret == -ETIMEDOUT && wakeup_calls == 1 && sample_calls == 6,
	       "missing final completion gets one deadline sample then times out");
	reset_case(); r1fb.pan.pending = true; sample_again_count = 5;
	injected_wakeup_expire = 1;
	ret = wait_live_pan(1, &(struct compas_fb_hw_live_fence){0});
	expect(ret == 0 && wakeup_calls == 1 && sample_calls == 6,
	       "coherent completion in the one final deadline sample is accepted");
	reset_case(); r1fb.pan.pending = true; sample_advance = 100;
	ret = wait_live_pan(1, &(struct compas_fb_hw_live_fence){0});
	expect(ret == 0 && sample_calls == 1,
	       "coherent sample that crosses deadline is accepted");

	/* IRQ fault and recorded pan error prevent a successful fence/commit. */
	reset_case(); r1fb.pan.pending = true; r1fb.irq_fault = true;
	ret = wait_live_pan(1, &(struct compas_fb_hw_live_fence){0});
	expect(ret == -EIO && sample_calls == 0,
	       "IRQ fault prevents hardware sample");
	reset_case(); r1fb.pan.pending = true; r1fb.pan_error = -EIO;
	ret = wait_live_pan(1, &(struct compas_fb_hw_live_fence){0});
	expect(ret == -EIO && sample_calls == 0,
	       "pan error prevents hardware sample");

	/* The real IRQ handler latches STATUS before acknowledging FRAME_END. */
	reset_case();
	r1fb.pan.pending = true;
	raw_flags = COMPAS_FB_HW_FRAME_END;
	raw_status = COMPAS_FB_HW_SRD_END;
	irq_remaining = 0;
	expect(compas_fb_irq(39, &r1fb) == IRQ_HANDLED &&
	       (r1fb.pan_events & (COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END)) ==
	       (COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END) &&
	       complete_calls == 1 && irq_write_count == 1 &&
	       irq_writes[0] == COMPAS_FB_HW_FRAME_END,
	       "IRQ latches DMA state before clearing display end and wakes pan");

	/* An underrun while pending is counted/cleared but cannot fail or wake the pan. */
	reset_case(); r1fb.pan.pending = true;
	raw_flags = COMPAS_FB_HW_UNDERRUN; irq_remaining = 0;
	expect(compas_fb_irq(39, &r1fb) == IRQ_HANDLED &&
	       r1fb.pan_error == 0 &&
	       (r1fb.pan_events & COMPAS_FB_HW_UNDERRUN) &&
	       complete_calls == 0 && irq_write_count == 1 &&
	       irq_writes[0] == COMPAS_FB_HW_UNDERRUN,
	       "underrun during pending pan is acknowledged without error or completion");

	/* With real EOD and frame-end, underrun is only additional evidence. */
	reset_case(); r1fb.pan.pending = true;
	raw_flags = COMPAS_FB_HW_UNDERRUN | COMPAS_FB_HW_FRAME_END;
	raw_status = COMPAS_FB_HW_SRD_END; irq_remaining = 0;
	expect(compas_fb_irq(39, &r1fb) == IRQ_HANDLED &&
	       r1fb.pan_error == 0 &&
	       (r1fb.pan_events & (COMPAS_FB_HW_UNDERRUN |
				  COMPAS_FB_HW_SRD_END |
				  COMPAS_FB_HW_FRAME_END)) ==
		(COMPAS_FB_HW_UNDERRUN | COMPAS_FB_HW_SRD_END |
		 COMPAS_FB_HW_FRAME_END) && complete_calls == 1 &&
	       irq_write_count == 1 &&
	       irq_writes[0] == (COMPAS_FB_HW_UNDERRUN | COMPAS_FB_HW_FRAME_END),
	       "underrun alongside genuine ends remains non-fatal and is W1C-cleared");

	/* STATUS may carry bit 8 even when the flag register only reports EOD. */
	reset_case(); r1fb.pan.pending = true;
	raw_flags = COMPAS_FB_HW_FRAME_END;
	raw_status = COMPAS_FB_HW_UNDERRUN;
	expect(compas_fb_irq(39, &r1fb) == IRQ_HANDLED && r1fb.pan_error == 0 &&
	       (r1fb.pan_events & COMPAS_FB_HW_UNDERRUN) && irq_write_count == 1 &&
	       irq_writes[0] == (COMPAS_FB_HW_UNDERRUN | COMPAS_FB_HW_FRAME_END),
	       "STATUS underrun is counted as evidence and W1C-cleared with EOD");

	/* A sustained underrun is removed from the interrupt mask without
	 * faulting the IRQ or completing a pan that has no real frame end. */
	reset_case(); r1fb.pan.pending = true;
	raw_flags = COMPAS_FB_HW_UNDERRUN;
	irq_remaining = COMPAS_FB_HW_UNDERRUN;
	expect(compas_fb_irq(39, &r1fb) == IRQ_HANDLED && !r1fb.irq_fault &&
	       r1fb.pan_error == 0 && r1fb.pan.pending && complete_calls == 0 &&
	       stop_calls == 0 && quarantine_calls == 0 &&
	       underrun_count == 1 && irq_write_count == 3 &&
	       mask_write_count == 1 &&
	       raw_mask == (COMPAS_FB_HW_FRAME_END | COMPAS_FB_HW_STOP_ACK) &&
	       mask_writes[0] == (COMPAS_FB_HW_FRAME_END | COMPAS_FB_HW_STOP_ACK),
	       "persistent underrun masks only bit 8 and leaves the pan pending");
	(void)arm_hardware_events();
	expect(raw_mask == (COMPAS_FB_HW_FRAME_END | COMPAS_FB_HW_UNDERRUN |
			    COMPAS_FB_HW_STOP_ACK),
	       "next pan re-arms underrun alongside frame end and STOP_ACK");

	/* A sticky completion source still faults and fails a pending fence. */
	reset_case(); r1fb.pan.pending = true;
	raw_flags = COMPAS_FB_HW_FRAME_END;
	irq_remaining = COMPAS_FB_HW_FRAME_END;
	expect(compas_fb_irq(39, &r1fb) == IRQ_HANDLED && r1fb.irq_fault &&
	       r1fb.pan_error == -EIO && complete_calls >= 1 && irq_write_count == 3,
	       "sticky interrupt remains fatal despite non-fatal underrun policy");

	puts("continuous pan callback paths passed");
	return 0;
}
"""


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[start : pos + 1]
    raise AssertionError("function body did not close")


class ContinuousPanTest(unittest.TestCase):
    def test_extracted_callback_paths(self):
        source = SOURCE.read_text()
        wait_helper = extract_function(source, "static int wait_live_pan(")
        function = extract_function(source, "static int compas_fb_pan_display(")
        irq_handler = extract_function(source, "static irqreturn_t compas_fb_irq(")
        with tempfile.TemporaryDirectory() as temp_dir:
            harness = Path(temp_dir) / "continuous_pan.c"
            binary = Path(temp_dir) / "continuous_pan"
            harness.write_text(PREFIX + wait_helper + function + irq_handler + SUFFIX)
            subprocess.run(
                ["cc", "-std=c99", "-Wall", "-Wextra", "-Werror", str(harness), "-o", str(binary)],
                check=True,
            )
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
