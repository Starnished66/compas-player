#!/usr/bin/env python3
"""Host regression harness for selected fixed R1 UAC and DMA C functions.

The USB functions are extracted from the supplied kernel source and compiled
with host-side mocks. This checks their behavior without a device or kernel
build. It is intentionally source-driven: changing a tested function changes
the code that the harness compiles.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import tempfile


HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint32_t __le32;
typedef unsigned long gfp_t;
typedef int spinlock_t;
struct usb_ep;
struct usb_request;
typedef void (*complete_fn)(struct usb_ep *, struct usb_request *);

struct usb_request {
    int status;
    unsigned actual, length, zero;
    void *context, *buf;
    complete_fn complete;
};
struct usb_ep {
    const char *name;
    int config_result, enable_result, alloc_fail, queue_result;
    int enabled, disable_count, free_count, dequeue_count;
    struct usb_request storage;
};
struct usb_gadget { struct usb_ep *ep0; };
struct usb_composite_dev { struct usb_gadget *gadget; struct usb_request *req; };
struct usb_configuration { struct usb_composite_dev *cdev; };
struct audio_dev;
struct usb_function { struct usb_configuration *config; struct audio_dev *owner; };
struct usb_ctrlrequest { u8 bRequestType, bRequest; u16 wValue, wIndex, wLength; };
struct uac_ring { spinlock_t lock; unsigned gen; };
struct uac_ep { struct usb_ep *ep; struct usb_request *req; void *buf; unsigned max_psize; int enabled; };
struct audio_dev {
    u8 ac_intf, ac_alt, as_out_intf, as_out_alt, last_alt;
    struct usb_function func;
    struct uac_ep out, fb;
    struct uac_ring ring;
    unsigned rate, bits;
    int format_changed;
    struct usb_ctrlrequest setup_ctrl;
};
struct cntrl_range_lay3 { u32 dMIN, dMAX; u32 dRES; };
typedef int32_t s32;
typedef int16_t s16;
typedef uint64_t dma_addr_t;
struct dma_async_tx_descriptor { int dummy; };
struct virt_dma_chan { int dummy; };
struct ingenic_dma_chan;
struct dma_chan { struct ingenic_dma_chan *owner; };
struct data_chunk { size_t size; s32 src_icg, dst_icg; };
struct dma_interleaved_template {
    int src_inc, dst_inc, src_sgl, dst_sgl;
    unsigned numf, frame_size;
    dma_addr_t src_start, dst_start;
    struct data_chunk *sgl;
};
struct hdma_desc { u32 dcm, dtc; dma_addr_t dsa, dta; u32 sd, drt; };
struct ingenic_dma_chan { struct virt_dma_chan vc; u32 dcm, slave_id; };
struct ingenic_dma_sdesc {
    size_t len;
    int dcs;
    struct dma_async_tx_descriptor vd;
    struct hdma_desc *hw_desc[2];
    unsigned long hw_desc_dma[2];
};

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define USB_DIR_IN 0x80
#define USB_TYPE_CLASS 0x20
#define USB_TYPE_MASK 0x60
#define USB_RECIP_INTERFACE 0x01
#define UAC2_CS_CUR 1
#define UAC2_CS_RANGE 2
#define UAC2_CS_CONTROL_SAM_FREQ 1
#define UAC2_CS_CONTROL_CLOCK_VALID 2
#define USB_OUT_CLK_ID 1
#define ALT_PCM24 1
#define ALT_PCM16 2
#define ALT_PCM32 3
#define ALT_NATIVE 4
#define DEFAULT_RATE 44100
#define GFP_ATOMIC 0
#define INTERLEAVED_MAX_LINES 255
#define INTERLEAVED_MAX_SIZE 0x1e00
#define DCM_SAI 1
#define DCM_DAI 2
#define DCM_RDIL_MAX 3
#define DCM_RDIL_SFT 4
#define DCM_TSZ_AUTO 5
#define DCM_TSZ_SFT 6
#define DCM_STDE 8
#define DCM_LINK 16
#define DCM_TIE 32
#define INGENIC_DMA_REQ_AUTO_TX 1
#define DCS_DES8 8
#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))
#define PHY_TO_DESC_DOA(x) ((u32)(x))
#define ESHUTDOWN 108
#define ECONNRESET 104
#define spin_lock_irqsave(lock, flags) do { (void)(lock); (flags) = 0; } while (0)
#define spin_unlock_irqrestore(lock, flags) do { (void)(lock); (void)(flags); } while (0)
#define dev_err(...) ((void)0)

static unsigned queue_calls;
static int dsd_native_enable = 1;
static const u8 usb_out_alt_bits[] = { 0, 24, 16, 32, 32 };
__RATE_TABLE__

static u16 le16_to_cpu(u16 v) { return v; }
static u32 le32_to_cpu(u32 v) { return v; }
static u32 get_unaligned_le32(const void *p) {
    const u8 *b = p; return (u32)b[0] | (u32)b[1]<<8 | (u32)b[2]<<16 | (u32)b[3]<<24;
}
static void put_unaligned_le32(u32 v, void *p) {
    u8 *b = p; b[0]=v; b[1]=v>>8; b[2]=v>>16; b[3]=v>>24;
}
static void put_unaligned_le16(u16 v, void *p) { u8 *b=p; b[0]=v; b[1]=v>>8; }
static void ring_reset(struct uac_ring *r, unsigned rate) { (void)rate; r->gen++; }
static u32 calc_feed_back(struct uac_ring *r, unsigned rate) { (void)r; return rate; }
static struct audio_dev *func_to_agdev(struct usb_function *f) { return f->owner; }
static void agdev_iso_out_complete(struct usb_ep *ep, struct usb_request *req) { (void)ep; (void)req; }
static void agdev_feedback_complete(struct usb_ep *ep, struct usb_request *req) { (void)ep; (void)req; }
static void uac_ep_stop(struct uac_ep *uep) {
    uep->enabled = 0;
    if (uep->req) {
        uep->ep->dequeue_count++;
        uep->ep->free_count++;
        uep->req = NULL;
    }
    uep->ep->enabled = 0;
    uep->ep->disable_count++;
}
static void afunc_stop(struct audio_dev *a) { uac_ep_stop(&a->out); uac_ep_stop(&a->fb); }
static int config_ep_by_speed(struct usb_gadget *g, struct usb_function *f, struct usb_ep *e) {
    (void)g; (void)f; return e->config_result;
}
static int usb_ep_enable(struct usb_ep *e) {
    if (!e->enable_result) e->enabled = 1;
    return e->enable_result;
}
static struct usb_request *usb_ep_alloc_request(struct usb_ep *e, gfp_t flags) {
    (void)flags; return e->alloc_fail ? NULL : &e->storage;
}
static int usb_ep_queue(struct usb_ep *e, struct usb_request *r, gfp_t flags) {
    (void)r; (void)flags; queue_calls++; return e->queue_result;
}
static void usb_ep_free_request(struct usb_ep *e, struct usb_request *r) { (void)r; e->free_count++; }
static int usb_ep_disable(struct usb_ep *e) { e->enabled = 0; e->disable_count++; return 0; }
static struct ingenic_dma_sdesc dma_alloc_fixture;
static struct hdma_desc dma_desc_fixture[2];
static int dma_alloc_calls;
static struct ingenic_dma_chan *to_ingenic_dma_chan(struct dma_chan *chan) { return chan->owner; }
static struct ingenic_dma_sdesc *ingenic_dma_alloc_swdesc(struct ingenic_dma_chan *dmac, int n) {
    (void)dmac; dma_alloc_calls++;
    if (n > 2) return NULL;
    memset(&dma_alloc_fixture, 0, sizeof dma_alloc_fixture);
    memset(dma_desc_fixture, 0, sizeof dma_desc_fixture);
    for (int i=0;i<n;i++) dma_alloc_fixture.hw_desc[i]=&dma_desc_fixture[i];
    return &dma_alloc_fixture;
}
static struct dma_async_tx_descriptor *vchan_tx_prep(struct virt_dma_chan *vc,
        struct dma_async_tx_descriptor *tx, unsigned long flags) {
    (void)vc; (void)flags; return tx;
}
__IN_COMPLETE__

__UAC_FUNCTIONS__
int dma_stride_tests(void);
int dma_prep_tests(void);
__DMA_FUNCTION__

static struct usb_ep ep0, out_ep, fb_ep;
static struct usb_gadget gadget;
static struct usb_composite_dev cdev;
static struct usb_configuration config;
static struct usb_request ep0_req;
static unsigned char control_buf[512], out_buf[16], fb_buf[16];
static struct audio_dev audio;

static void reset_fixture(void) {
    memset(&ep0, 0, sizeof ep0); memset(&out_ep, 0, sizeof out_ep); memset(&fb_ep, 0, sizeof fb_ep);
    memset(&ep0_req, 0, sizeof ep0_req); memset(control_buf, 0, sizeof control_buf);
    memset(out_buf, 0, sizeof out_buf); memset(fb_buf, 0, sizeof fb_buf); memset(&audio, 0, sizeof audio);
    ep0.name="ep0"; out_ep.name="out"; fb_ep.name="fb";
    gadget.ep0=&ep0; cdev.gadget=&gadget; cdev.req=&ep0_req; config.cdev=&cdev;
    audio.ac_intf=0; audio.as_out_intf=1; audio.rate=44100; audio.bits=16;
    audio.func.config=&config; audio.func.owner=&audio;
    audio.out.ep=&out_ep; audio.out.buf=out_buf; audio.out.max_psize=8;
    audio.fb.ep=&fb_ep; audio.fb.buf=fb_buf; audio.fb.max_psize=4;
    ep0_req.buf=control_buf; queue_calls=0;
}
static struct usb_ctrlrequest request(u8 type, u8 req, u16 value, u16 index, u16 len) {
    struct usb_ctrlrequest cr={type,req,value,index,len}; return cr;
}
static void put_rate(struct usb_request *r, u32 rate) { put_unaligned_le32(rate, r->buf); r->actual=4; r->status=0; }

static void test_control_out_lengths_and_valid_queue(void) {
    unsigned lengths[]={65535,1025,0,3,5};
    reset_fixture();
    for (unsigned i=0;i<ARRAY_SIZE(lengths);i++) {
        struct usb_ctrlrequest cr=request(USB_TYPE_CLASS|USB_RECIP_INTERFACE,UAC2_CS_CUR,
            UAC2_CS_CONTROL_SAM_FREQ<<8,(USB_OUT_CLK_ID<<8)|audio.ac_intf,lengths[i]);
        unsigned before=queue_calls;
        assert(afunc_setup(&audio.func,&cr)==-EOPNOTSUPP);
        assert(queue_calls==before);
    }
    struct usb_ctrlrequest good=request(USB_TYPE_CLASS|USB_RECIP_INTERFACE,UAC2_CS_CUR,
        UAC2_CS_CONTROL_SAM_FREQ<<8,(USB_OUT_CLK_ID<<8)|audio.ac_intf,4);
    assert(afunc_setup(&audio.func,&good)==0);
    assert(queue_calls==1 && ep0_req.length==4 && ep0_req.context==&audio);
    assert(ep0_req.complete==uac_setup_complete);
}
static void test_invalid_control_selectors_and_in_cleanup(void) {
    reset_fixture();
    struct usb_ctrlrequest bad[]={
        {USB_TYPE_CLASS|USB_RECIP_INTERFACE,UAC2_CS_CUR,UAC2_CS_CONTROL_SAM_FREQ<<8,0x0101,4},
        {USB_TYPE_CLASS|USB_RECIP_INTERFACE,UAC2_CS_CUR,UAC2_CS_CONTROL_SAM_FREQ<<8,0x0200,4},
        {USB_TYPE_CLASS|USB_RECIP_INTERFACE,UAC2_CS_CUR,0x0300,0x0100,4},
        {USB_TYPE_CLASS|USB_RECIP_INTERFACE,UAC2_CS_CUR,(UAC2_CS_CONTROL_SAM_FREQ<<8)|1,0x0100,4},
        {USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_INTERFACE,UAC2_CS_CUR,(UAC2_CS_CONTROL_SAM_FREQ<<8)|1,0x0100,4},
        {USB_TYPE_CLASS|USB_RECIP_INTERFACE,UAC2_CS_RANGE,UAC2_CS_CONTROL_SAM_FREQ<<8,0x0100,4},
        {USB_TYPE_CLASS|USB_RECIP_INTERFACE,0x7f,UAC2_CS_CONTROL_SAM_FREQ<<8,0x0100,4},
        {0xa2,UAC2_CS_CUR,UAC2_CS_CONTROL_SAM_FREQ<<8,0x0100,4},
    };
    for (unsigned i=0;i<ARRAY_SIZE(bad);i++) {
        unsigned before=queue_calls;
        assert(afunc_setup(&audio.func,&bad[i])<0);
        assert(queue_calls==before);
    }
    ep0_req.context=(void *)0x1234; ep0_req.complete=agdev_iso_out_complete;
    struct usb_ctrlrequest get=request(USB_DIR_IN|USB_TYPE_CLASS|USB_RECIP_INTERFACE,UAC2_CS_CUR,
        UAC2_CS_CONTROL_SAM_FREQ<<8,0x0100,4);
    assert(afunc_setup(&audio.func,&get)==0 && ep0_req.length==4);
    assert(ep0_req.context==NULL && ep0_req.complete!=NULL && ep0_req.complete!=uac_setup_complete);
    unsigned rate=audio.rate, generation=audio.ring.gen;
    int changed=audio.format_changed;
    ep0_req.complete(&ep0,&ep0_req);
    assert(audio.rate==rate && audio.ring.gen==generation && audio.format_changed==changed);
}
static void test_set_cur_rates_and_unadvertised_rate(void) {
    const u32 rates[]={32000,44100,48000,88200,96000,176400,192000,352800,384000};
    reset_fixture();
    for (unsigned i=0;i<ARRAY_SIZE(rates);i++) {
        audio.rate=1; audio.format_changed=0;
        ep0_req.context=&audio; audio.setup_ctrl=request(0,UAC2_CS_CUR,
            UAC2_CS_CONTROL_SAM_FREQ<<8,0x0100,4);
        put_rate(&ep0_req,rates[i]);
        uac_setup_complete(&ep0,&ep0_req);
        assert(audio.rate==rates[i] && audio.format_changed && audio.ring.gen==i+1);
    }
    audio.rate=48000; audio.format_changed=0;
    unsigned generation=audio.ring.gen;
    put_rate(&ep0_req,12345);
    uac_setup_complete(&ep0,&ep0_req);
    assert(audio.rate==48000 && !audio.format_changed && audio.ring.gen==generation);
}
static void test_incomplete_or_failed_set_cur_is_ignored(void) {
    reset_fixture(); audio.rate=44100; audio.format_changed=0; audio.ring.gen=9;
    audio.setup_ctrl=request(0,UAC2_CS_CUR,UAC2_CS_CONTROL_SAM_FREQ<<8,0x0100,4);
    ep0_req.context=&audio;
    put_rate(&ep0_req,48000); ep0_req.actual=3;
    uac_setup_complete(&ep0,&ep0_req);
    assert(audio.rate==44100 && !audio.format_changed && audio.ring.gen==9);
    put_rate(&ep0_req,48000); ep0_req.actual=5;
    uac_setup_complete(&ep0,&ep0_req);
    assert(audio.rate==44100 && !audio.format_changed && audio.ring.gen==9);
    put_rate(&ep0_req,48000); ep0_req.status=-EIO;
    uac_setup_complete(&ep0,&ep0_req);
    assert(audio.rate==44100 && !audio.format_changed && audio.ring.gen==9);
}
static void test_endpoint_start_rollbacks(void) {
    reset_fixture();
    out_ep.enable_result=-EIO;
    assert(uac_ep_start(&audio,&audio.out,agdev_iso_out_complete)==-EIO);
    assert(!audio.out.enabled && !audio.out.req && out_ep.disable_count==0);
    reset_fixture(); out_ep.alloc_fail=1;
    assert(uac_ep_start(&audio,&audio.out,agdev_iso_out_complete)==-ENOMEM);
    assert(!audio.out.enabled && !audio.out.req && !out_ep.enabled && out_ep.disable_count==1);
    reset_fixture(); out_ep.queue_result=-EIO;
    assert(uac_ep_start(&audio,&audio.out,agdev_iso_out_complete)==-EIO);
    assert(!audio.out.enabled && !audio.out.req && !out_ep.enabled);
    assert(out_ep.free_count==1 && out_ep.disable_count==1);
}
static void test_feedback_start_failure_stops_out_endpoint(void) {
    reset_fixture(); fb_ep.queue_result=-EIO;
    assert(afunc_set_alt(&audio.func,audio.as_out_intf,ALT_PCM24)==-EIO);
    assert(audio.as_out_alt==0);
    assert(!audio.out.enabled && !audio.out.req && !out_ep.enabled);
    assert(out_ep.dequeue_count==1 && out_ep.free_count==1 && out_ep.disable_count==2);
    assert(!audio.fb.enabled && !audio.fb.req && !fb_ep.enabled);
}

int main(void) {
    test_control_out_lengths_and_valid_queue();
    test_invalid_control_selectors_and_in_cleanup();
    test_set_cur_rates_and_unadvertised_rate();
    test_incomplete_or_failed_set_cur_is_ignored();
    test_endpoint_start_rollbacks();
    test_feedback_start_failure_stops_out_endpoint();
    dma_stride_tests();
    dma_prep_tests();
    puts("USB UAC and DMA safety harness passed");
    return 0;
}
'''


def extract_function(source: str, name: str) -> str:
    signature = re.search(
        rf"\b(?:static\s+)?(?:int|void|struct\s+\w+\s*\*)\s*{re.escape(name)}\s*\([^;]*?\)\s*\{{",
        source,
        re.S,
    )
    if not signature:
        raise ValueError(f"Could not find function definition: {name}")
    opening = signature.end() - 1
    depth = 0
    state = "code"
    escaped = False
    i = opening
    while i < len(source):
        char = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state == "code":
            if char == "/" and nxt == "*":
                state = "block_comment"; i += 1
            elif char == "/" and nxt == "/":
                state = "line_comment"; i += 1
            elif char == '"': state = "string"
            elif char == "'": state = "char"
            elif char == "{": depth += 1
            elif char == "}":
                depth -= 1
                if depth == 0:
                    return source[signature.start():i + 1]
        elif state == "block_comment":
            if char == "*" and nxt == "/": state = "code"; i += 1
        elif state == "line_comment":
            if char == "\n": state = "code"
        elif state in ("string", "char"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        i += 1
    raise ValueError(f"Unclosed function body: {name}")


def dma_expression(source: str) -> str:
    match = re.search(r"desc->sd\s*=\s*(.*?);", source, re.S)
    if not match:
        raise ValueError("Could not find the interleaved DMA desc->sd assignment")
    expression = " ".join(match.group(1).split())
    if "chunk->dst_icg" not in expression or "chunk->src_icg" not in expression:
        raise ValueError("Found desc->sd assignment does not encode both source and destination gaps")
    return expression


def extract_rate_table(source: str) -> str:
    match = re.search(
        r"static\s+const\s+struct\s+cntrl_range_lay3\s+usb_out_rate_range\[\]\s*=\s*\{(.*?)\};",
        source,
        re.S,
    )
    if not match:
        raise ValueError("Could not find usb_out_rate_range in UAC source")
    rates = re.findall(r"\bRATE\((\d+)\)", match.group(1))
    if not rates:
        raise ValueError("usb_out_rate_range contains no RATE entries")
    entries = ", ".join("{" + rate + ", " + rate + ", 0}" for rate in rates)
    return "static const struct cntrl_range_lay3 usb_out_rate_range[] = {" + entries + "};"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=pathlib.Path, required=True, help="fixed-source/f_uac_sa.c")
    parser.add_argument("--dma-source", type=pathlib.Path, required=True, help="fixed-source/ingenic_dma.c")
    args = parser.parse_args()
    source = args.source.read_text()
    names = [
        "uac_ep_start", "afunc_set_alt", "uac_setup_complete", "afunc_setup",
    ]
    try:
        in_complete = extract_function(source, "uac_setup_in_complete")
        helper = "static void uac_setup_in_complete(struct usb_ep *, struct usb_request *);"
        names.insert(-1, "uac_setup_in_complete")
    except ValueError:
        in_complete = "static void uac_setup_in_complete(struct usb_ep *ep, struct usb_request *req) { (void)ep; (void)req; }"
        helper = ""
    functions = "\n\n".join(extract_function(source, name) for name in names)
    rate_table = extract_rate_table(source)
    dma_source = args.dma_source.read_text()
    dma_function = extract_function(dma_source, "ingenic_dma_prep_interleaved_dma")
    dma = dma_expression(dma_function)
    usb = HARNESS.replace("__UAC_FUNCTIONS__", functions)
    usb = usb.replace("__IN_COMPLETE__", helper if helper else in_complete)
    usb = usb.replace("__RATE_TABLE__", rate_table)
    usb = usb.replace("__DMA_FUNCTION__", dma_function)
    usb += "static u32 encode_dma_stride(struct data_chunk *chunk) { struct hdma_desc d; struct hdma_desc *desc=&d; desc->sd = " + dma + "; return desc->sd; }\n"
    usb += r'''
int dma_stride_tests(void) {
    struct data_chunk c;
    c.src_icg=-640; c.dst_icg=0;
    assert(encode_dma_stride(&c)==(u16)-640);
    c.src_icg=0; c.dst_icg=-640;
    assert(encode_dma_stride(&c)==((u32)(u16)-640<<16));
    c.src_icg=37; c.dst_icg=-91;
    assert(encode_dma_stride(&c)==(((u32)(u16)-91<<16)|(u16)37));
    c.src_icg=-91; c.dst_icg=37;
    assert(encode_dma_stride(&c)==(((u32)(u16)37<<16)|(u16)-91));
    return 0;
}
static struct dma_chan test_chan;
static struct ingenic_dma_chan test_dmac;
static struct data_chunk test_chunk;
static struct dma_interleaved_template test_xt;
static void reset_dma(int src, int dst) {
    memset(&test_chan,0,sizeof test_chan); memset(&test_dmac,0,sizeof test_dmac);
    memset(&test_chunk,0,sizeof test_chunk); memset(&test_xt,0,sizeof test_xt);
    test_chan.owner=&test_dmac; test_chunk.size=8; test_chunk.src_icg=src; test_chunk.dst_icg=dst;
    test_xt.src_inc=test_xt.dst_inc=test_xt.src_sgl=test_xt.dst_sgl=1;
    test_xt.numf=1; test_xt.frame_size=1; test_xt.src_start=0x1000; test_xt.dst_start=0x2000;
    test_xt.sgl=&test_chunk;
}
int dma_prep_tests(void) {
    reset_dma(32768,0); dma_alloc_calls=0;
    assert(ingenic_dma_prep_interleaved_dma(&test_chan,&test_xt,0)==NULL && dma_alloc_calls==0);
    reset_dma(-32769,0); assert(ingenic_dma_prep_interleaved_dma(&test_chan,&test_xt,0)==NULL);
    reset_dma(0,32768); assert(ingenic_dma_prep_interleaved_dma(&test_chan,&test_xt,0)==NULL);
    reset_dma(0,-32769); assert(ingenic_dma_prep_interleaved_dma(&test_chan,&test_xt,0)==NULL);
    reset_dma(-640,0);
    assert(ingenic_dma_prep_interleaved_dma(&test_chan,&test_xt,0)!=NULL);
    assert(dma_desc_fixture[0].sd==(u16)-640);
    reset_dma(0,-640);
    assert(ingenic_dma_prep_interleaved_dma(&test_chan,&test_xt,0)!=NULL);
    assert(dma_desc_fixture[0].sd==((u32)(u16)-640<<16));
    reset_dma(-32768,32767);
    assert(ingenic_dma_prep_interleaved_dma(&test_chan,&test_xt,0)!=NULL);
    assert(dma_desc_fixture[0].sd==((u32)(u16)32767<<16 | (u16)-32768));
    reset_dma(32767,-32768);
    assert(ingenic_dma_prep_interleaved_dma(&test_chan,&test_xt,0)!=NULL);
    assert(dma_desc_fixture[0].sd==((u32)(u16)-32768<<16 | (u16)32767));
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="uac-safety-") as temp:
        cfile = pathlib.Path(temp) / "harness.c"
        binary = pathlib.Path(temp) / "harness"
        cfile.write_text(usb)
        subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                        "-Wno-unused-function",
                        "-o", str(binary), str(cfile)], check=True)
        subprocess.run([str(binary)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
