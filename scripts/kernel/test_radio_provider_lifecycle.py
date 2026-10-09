#!/usr/bin/env python3
"""Compile exact provider transition functions with host-only MMC/GPIO stubs."""
import argparse
import pathlib
import re
import subprocess
import tempfile
import unittest
import sys

PREAMBLE = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t u16;
struct device { int unused; };
struct platform_device { struct device dev; };
struct notifier_block { int unused; };
struct work_struct { int unused; };
struct mmc_compas_sdio_guard { int unused; };
struct mmc_compas_sdio_identity { u16 function2_vendor,function2_device; bool valid,maker_tuple_present,azurewave; unsigned char maker_tuple_value; };
enum mmc_compas_sdio_end_policy { MMC_COMPAS_SDIO_PRESERVE, MMC_COMPAS_SDIO_FORCE, MMC_COMPAS_SDIO_DISCARD };
__IDENTITY_ENUM__
static struct platform_device pdev;
static struct platform_device *radio_pdev=&pdev;
static struct work_struct probe_work;
static int radio_lock,tx_lock;
static bool operational,suspending,transitioning,wifi_on,bt_on,manual_clocks_owned;
static bool pm_off_safe,wifi_fault,core_off;
static int wifi_errno,off_error,begin_calls,begin_fail_after;
static enum identity_state identity_state;
static int identity_errno;
static unsigned int chipvendor;
static u16 sdio_vendor,sdio_device;
static const char *identity_profile,*identity_source;
static char *profile;
static int wl_reg_on=35,bt_reg_on=36;
static int lpo_refs, detect_on,detect_off,detect_error, save_error,restore_error;
static int begin_error, pending_polls,guard_count, force_scans, discard_scans,sleep_polls;
static struct mmc_compas_sdio_guard mock_guard;
static struct mmc_compas_sdio_identity mock_id;
#define CARD_WAIT_MS 60
#define NOTIFY_OK 0
#define NOTIFY_BAD 1
#define PM_SUSPEND_PREPARE 1
#define PM_HIBERNATION_PREPARE 2
#define PM_RESTORE_PREPARE 3
#define PM_POST_SUSPEND 4
#define PM_POST_HIBERNATION 5
#define PM_POST_RESTORE 6
#define dev_err(dev,...) ((void)(dev))
static void mutex_lock(int *m) { assert(!*m); *m=1; }
static void mutex_unlock(int *m) { assert(*m); *m=0; }
static void ingenic_rtc32k_enable(void) { ++lpo_refs; }
static void ingenic_rtc32k_disable(void) { assert(lpo_refs>0); --lpo_refs; }
static void gpio_set_value_cansleep(int pin,int on) { (void)pin; (void)on; }
static void msleep(int ms) { if(ms==20) { assert(!guard_count && !radio_lock); ++sleep_polls; } }
static int jzmmc_manual_detect(int index,int on) { assert(index==0); if(on) {++detect_on; return detect_error;} ++detect_off; return 0; }
static int begin_guard(struct mmc_compas_sdio_guard **out) { assert(tx_lock && !radio_lock); ++begin_calls; if(begin_error || (begin_fail_after && begin_calls>=begin_fail_after))return begin_error?begin_error:-EBUSY; assert(!guard_count); ++guard_count; *out=&mock_guard; return 0; }
static int mmc_compas_sdio_identity(struct mmc_compas_sdio_guard *g,struct mmc_compas_sdio_identity *id) { assert(g && guard_count); if(pending_polls>0) {--pending_polls; return -ENOMEDIUM;} *id=mock_id; return 0; }
static int mmc_compas_sdio_power(struct mmc_compas_sdio_guard *g,bool restore) { assert(g && guard_count); return restore?restore_error:save_error; }
static int mmc_compas_sdio_set_off(struct mmc_compas_sdio_guard *g,bool off) { assert(g && guard_count); if(off_error)return off_error; core_off=off; return 0; }
static void mmc_compas_sdio_end(struct mmc_compas_sdio_guard *g,enum mmc_compas_sdio_end_policy p) { assert(g && guard_count && !radio_lock); --guard_count; if(p==MMC_COMPAS_SDIO_FORCE)++force_scans; if(p==MMC_COMPAS_SDIO_DISCARD)++discard_scans; }
static void schedule_work(struct work_struct *w) { (void)w; }
__FUNCTIONS__
static void reset(void) {
 operational=true; suspending=transitioning=wifi_on=bt_on=manual_clocks_owned=false;
 pm_off_safe=true; wifi_fault=core_off=false; wifi_errno=off_error=begin_calls=begin_fail_after=0;
 identity_state=ID_PENDING; identity_errno=0; chipvendor=0; sdio_vendor=sdio_device=0;
 identity_profile=identity_source=NULL; profile="auto";
 lpo_refs=detect_on=detect_off=detect_error=save_error=restore_error=begin_error=pending_polls=guard_count=force_scans=discard_scans=sleep_polls=0;
 radio_lock=tx_lock=0;
 mock_id=(struct mmc_compas_sdio_identity){.function2_vendor=0x02d0,.function2_device=0xa9a6,.valid=true,.maker_tuple_present=true,.azurewave=true,.maker_tuple_value=1};
}
static void identity_and_bt(void) {
 reset(); assert(bt_set_block(NULL,false)==-EAGAIN && !bt_on && !lpo_refs);
 operational=false; assert(bt_set_block(NULL,false)==-EAGAIN); assert(!bt_set_block(NULL,true)); operational=true;
 mock_id.azurewave=false; mock_id.maker_tuple_present=false;
 assert(publish_identity(&mock_id)==-ENODEV && identity_state==ID_PENDING);
 profile="ap6212a"; assert(!publish_identity(&mock_id)); assert(identity_state==ID_READY && !strcmp(identity_source,"profile") && chipvendor==0);
 assert(!bt_set_block(NULL,false) && bt_on && lpo_refs==1);
 assert(!bt_set_block(NULL,false) && lpo_refs==1);
 suspending=true; assert(bt_set_block(NULL,false)==-EBUSY); assert(!bt_set_block(NULL,true) && !lpo_refs);
 reset(); assert(!publish_identity(&mock_id)); mock_id.azurewave=false; mock_id.maker_tuple_value=2;
 assert(publish_identity(&mock_id)==-ENODEV);
 reset(); profile="ap6212a"; mock_id.azurewave=false; mock_id.maker_tuple_value=0;
 assert(mock_id.maker_tuple_present && !publish_identity(&mock_id));
 assert(identity_state==ID_READY && chipvendor==0 && !strcmp(identity_source,"profile"));
 reset(); profile="ap6212a"; mock_id.azurewave=false; mock_id.maker_tuple_present=false;
 mock_id.maker_tuple_value=0; assert(!publish_identity(&mock_id));
 assert(identity_state==ID_READY && chipvendor==0 && !strcmp(identity_source,"profile"));
 reset(); profile="ap6212a"; mock_id.azurewave=false; mock_id.maker_tuple_value=2;
 assert(publish_identity(&mock_id)==-ENODEV);
 reset(); profile="ap6212a"; assert(publish_identity(&mock_id)==-ENODEV);
}
static void cycles_and_coexistence(void) {
 reset(); pm_off_safe=false; assert(radio_pm_notify(NULL,PM_SUSPEND_PREPARE,NULL)==NOTIFY_BAD); pm_off_safe=true; pending_polls=2;
 assert(!wifi_transition(true)); assert(identity_state==ID_READY && wifi_on && lpo_refs==1 && detect_on==1 && sleep_polls==2 && !guard_count);
 assert(!bt_set_block(NULL,false) && lpo_refs==2);
 assert(!wifi_transition(false) && !wifi_on && bt_on && lpo_refs==1 && core_off && pm_off_safe);
 assert(radio_pm_notify(NULL,PM_SUSPEND_PREPARE,NULL)==NOTIFY_BAD && !suspending);
 assert(!bt_set_block(NULL,true) && !lpo_refs);
 for(int i=0;i<3;i++) { assert(!wifi_transition(true)); assert(!wifi_transition(false)); }
 assert(detect_on==1 && !detect_off && manual_clocks_owned && !lpo_refs);
 assert(radio_pm_notify(NULL,PM_SUSPEND_PREPARE,NULL)==NOTIFY_OK && suspending);
 assert(wifi_transition(true)==-EBUSY && !wifi_on);
 radio_pm_notify(NULL,PM_POST_SUSPEND,NULL); assert(!suspending);
 assert(!wifi_transition(true)); save_error=-EBUSY;
 assert(wifi_transition(false)==-EBUSY && wifi_on && lpo_refs==1 && !guard_count);
 assert(radio_pm_notify(NULL,PM_SUSPEND_PREPARE,NULL)==NOTIFY_BAD);
 save_error=0; assert(!wifi_transition(false) && !lpo_refs);
 transitioning=true; assert(radio_pm_notify(NULL,PM_SUSPEND_PREPARE,NULL)==NOTIFY_BAD);
}
static void failures(void) {
 reset(); detect_error=-1; assert(wifi_transition(true)==-ENODEV && !wifi_on && !lpo_refs && identity_state==ID_ERROR && !manual_clocks_owned);
 reset(); pending_polls=100; assert(wifi_transition(true)==-ETIMEDOUT); assert(!wifi_on && !lpo_refs && detect_on==1 && detect_off==1 && !manual_clocks_owned && !guard_count);
 assert(wifi_transition(true)==-ENODEV && detect_on==1);
 reset(); restore_error=-EIO; assert(wifi_transition(true)==-EIO && !wifi_on && !lpo_refs && detect_off==1);
 reset(); begin_error=-EBUSY; assert(wifi_transition(true)==-EBUSY && !detect_on && !lpo_refs && !transitioning);
 reset(); mock_id.function2_device=0x4343; assert(wifi_transition(true)==-ENODEV && !wifi_on && !lpo_refs && detect_off==1);
}
static void fault_and_missing_card_recovery(void) {
 reset(); begin_error=-EBUSY; assert(wifi_transition(true)==-EBUSY && !wifi_on && pm_off_safe);
 assert(radio_pm_notify(NULL,PM_SUSPEND_PREPARE,NULL)==NOTIFY_OK);
 reset(); begin_error=-ENODEV; assert(wifi_transition(true)==-ENODEV && !wifi_on && pm_off_safe);
 assert(radio_pm_notify(NULL,PM_SUSPEND_PREPARE,NULL)==NOTIFY_OK);
 reset(); assert(!wifi_transition(true)); assert(!wifi_transition(false)); restore_error=-EIO;
 assert(wifi_transition(true)==-EIO && wifi_fault && wifi_errno==-EIO && identity_state==ID_READY && !wifi_on && core_off);
 assert(!bt_set_block(NULL,false) && bt_on && lpo_refs==1);
 assert(wifi_transition(true)==-ENODEV); assert(!bt_set_block(NULL,true));
 reset(); assert(!wifi_transition(true)); assert(!wifi_transition(false)); mock_id.function2_device=0x4343;
 assert(wifi_transition(true)==-ENODEV && identity_state==ID_ERROR && wifi_fault);
 assert(bt_set_block(NULL,false)==-ENODEV);
 reset(); pending_polls=100; begin_fail_after=4;
 assert(wifi_transition(true)==-EBUSY && wifi_on && lpo_refs==1 && wifi_fault && !pm_off_safe);
 assert(radio_pm_notify(NULL,PM_SUSPEND_PREPARE,NULL)==NOTIFY_BAD);
 begin_fail_after=0; save_error=-ENOMEDIUM;
 assert(!wifi_transition(false) && !wifi_on && !lpo_refs && core_off && pm_off_safe);
 assert(radio_pm_notify(NULL,PM_SUSPEND_PREPARE,NULL)==NOTIFY_OK);
 reset(); off_error=-EBUSY; assert(wifi_transition(true)==-EBUSY && !wifi_on && !pm_off_safe);
 assert(radio_pm_notify(NULL,PM_SUSPEND_PREPARE,NULL)==NOTIFY_BAD);
}
int main(void) {identity_and_bt(); cycles_and_coexistence(); failures(); fault_and_missing_card_recovery(); puts("provider lifecycle checks passed");}
'''


def extract(source,name):
    match=re.search(r'static (?:int|void) '+re.escape(name)+r'\(',source)
    if not match: raise ValueError(f'missing {name}')
    brace=source.index('{',match.start()); depth=0
    for i in range(brace,len(source)):
        if source[i]=='{':depth+=1
        elif source[i]=='}':
            depth-=1
            if not depth:return source[match.start():i+1]
    raise ValueError(name)


def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--source',type=pathlib.Path);args=ap.parse_args()
    if args.source:
        source=args.source.read_text()
    else:
        # Full-context patch reproduces the exact provider source without an external build tree.
        patch=pathlib.Path(__file__).resolve().parents[2]/'firmware/kernel/module-patches/compas-radio-lifecycle.patch'
        source='\n'.join(line[1:] for line in patch.read_text().splitlines() if (line.startswith('+') and not line.startswith('+++')) or line.startswith(' '))
    names=['publish_wifi_failure','publish_identity','bt_power','bt_set_block','transition_begin','transition_end','energize_guarded','rollback_guarded','wifi_transition','radio_pm_notify']
    enum=re.search(r'enum identity_state \{[^}]+\};',source).group(0)
    harness=PREAMBLE.replace('__IDENTITY_ENUM__',enum).replace('__FUNCTIONS__','\n'.join(extract(source,n) for n in names))
    with tempfile.TemporaryDirectory(prefix='radio-provider-host-') as d:
        p=pathlib.Path(d);(p/'check.c').write_text(harness)
        subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',str(p/'check.c'),'-o',str(p/'check')],check=True)
        subprocess.run([str(p/'check')],check=True)

class ProviderLifecycleChecks(unittest.TestCase):
    def test_current_patch_transition_functions(self):
        subprocess.run([sys.executable, str(pathlib.Path(__file__).resolve())], check=True)

if __name__=='__main__':main()
