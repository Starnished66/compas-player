#!/usr/bin/env python3
"""Source-extracted checks that failed experimental radio shutdown vetoes suspend."""
import pathlib
import re
import subprocess
import tempfile
import unittest

SOURCE=pathlib.Path(__file__).resolve().parents[2]/'src/hardware/power_suspend.c'
HARNESS=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
typedef unsigned long pthread_t;
#define F_OK 0
#define SUBPROCESS_RADIO_TIMEOUT_MS 120000
#define RADIO_UI_ACTION_TIMEOUT_MS 5000
#define BATTERY_EXTERNAL_POWER_DISCONNECTED 1
static void mock_log(const char *fmt, ...) {(void)fmt;}
#define SUSPEND_LOG(...) mock_log(__VA_ARGS__)
static int experimental,prepare_status,end_status,process_ok,power_samples;
static int mem_count,blank_count,restore_wifi,restore_bt,legacy_wifi_off,legacy_bt_off,legacy_bt_suspend;
static int prepare_count,end_count,thread_attempts,sleep_count;
static void *(*created_func)(void *);
static void *created_arg;
static bool usb_after_prepare,in_recovery;
static bool db_log_enabled(void) {return false;}
static int battery_get_external_power_state(void) {return usb_after_prepare && power_samples++ ? 2 : 1;}
static bool wifi_control_is_enabled(void) {return true;}
static bool bt_control_is_powered(void) {return true;}
static void wifi_control_disable(void) {++legacy_wifi_off;}
static void bt_control_disable(void) {++legacy_bt_off;}
static int access(const char *p,int m) {(void)p;(void)m; return experimental?0:-1;}
static bool subprocess_run_checked_group(char *const a[],char *out,unsigned long n,int t,int *status) {
 (void)out;(void)n;assert(t==(in_recovery?SUBPROCESS_RADIO_TIMEOUT_MS:RADIO_UI_ACTION_TIMEOUT_MS));
 if(!strcmp(a[1],"suspend-prepare")) {++prepare_count;*status=prepare_status;}
 else {assert(!strcmp(a[1],"suspend-end"));++end_count;*status=end_status;}
 return process_ok;
}
static bool subprocess_run(char *const a[],char *out,unsigned long n) {(void)out;(void)n;assert(!strcmp(a[0],"/usr/bin/bt_suspend"));++legacy_bt_suspend;return true;}
static void write_sysfs(const char *p,const char *v) {(void)p;assert(!strcmp(v,"4"));++blank_count;}
static bool write_sysfs_checked(const char *p,const char *v,int *error) {(void)p;assert(!strcmp(v,"mem"));*error=0;++mem_count;return true;}
static unsigned int monotonic_ms(void) {return 0;}
static unsigned int suspend_duration_ms_now(void) {return 0;}
static void log_suspend_diagnostics(unsigned int n,bool ok,int e) {(void)n;(void)ok;(void)e;}
static void radio_restore_perform(bool w,bool b) {assert(!experimental || end_count); restore_wifi+=w;restore_bt+=b;}
static void *radio_restore_thread_func(void *arg) {(void)arg;return 0;}
static int pthread_create(pthread_t *t,const void *attrs,void *(*f)(void *),void *arg) {(void)t;(void)attrs;created_func=f;created_arg=arg;++thread_attempts;return 1;}
static void usleep(unsigned int n) {assert(n==1000000);++sleep_count;in_recovery=true;}
static int pthread_detach(pthread_t t) {(void)t;return 0;}
__FUNCTIONS__
static void reset(void) {
 thread_attempts=sleep_count=0;created_func=NULL;created_arg=NULL;in_recovery=false;
 experimental=process_ok=1; prepare_status=end_status=power_samples=0;usb_after_prepare=false;
 mem_count=blank_count=restore_wifi=restore_bt=legacy_wifi_off=legacy_bt_off=legacy_bt_suspend=prepare_count=end_count=0;
}
int main(void) {
 reset();prepare_status=1;power_suspend_now();assert(!mem_count&&!blank_count&&prepare_count==1&&end_count==1&&restore_wifi==1&&restore_bt==1);
 reset();process_ok=0;power_suspend_now();assert(!mem_count&&!restore_wifi&&!restore_bt&&!end_count&&thread_attempts==1&&created_func==radio_recovery_thread_func);
 process_ok=1;created_func(created_arg);assert(prepare_count==2&&end_count==1&&restore_wifi==1&&restore_bt==1&&sleep_count==1);
 reset();process_ok=0;radio_recovery_thread_func((void *)3);assert(!end_count&&!restore_wifi&&!restore_bt&&prepare_count==3&&sleep_count==3);
 reset();power_suspend_now();assert(mem_count==1&&blank_count==1&&end_count==1&&restore_wifi==1&&restore_bt==1&&!legacy_wifi_off&&legacy_bt_off==1&&!legacy_bt_suspend);
 reset();end_status=1;power_suspend_now();assert(mem_count==1&&!restore_wifi&&!restore_bt&&thread_attempts==1&&created_func==radio_recovery_thread_func);
 end_status=0;created_func(created_arg);assert(prepare_count==1&&end_count==2&&restore_wifi==1&&restore_bt==1);
 reset();prepare_status=1;end_status=1;power_suspend_now();assert(!mem_count&&!restore_wifi&&thread_attempts==1);
 prepare_status=0;end_status=0;created_func(created_arg);assert(prepare_count==1&&end_count==2&&restore_wifi==1);
 reset();usb_after_prepare=true;power_suspend_now();assert(!mem_count&&end_count==1&&restore_wifi==1&&restore_bt==1);
 reset();experimental=0;power_suspend_now();assert(mem_count==1&&legacy_wifi_off==1&&legacy_bt_off==1&&legacy_bt_suspend==1&&!prepare_count&&!end_count);
}
'''


def extract(source,name):
    m=re.search(r'(?:static bool |static int |static void |static void \* |void )'+re.escape(name)+r'\(',source)
    if not m:raise ValueError(name)
    b=source.index('{',m.start());depth=0
    for i in range(b,len(source)):
        if source[i]=='{':depth+=1
        elif source[i]=='}':
            depth-=1
            if not depth:return source[m.start():i+1]
    raise ValueError(name)


class RadioSuspendIntegration(unittest.TestCase):
    def test_actual_suspend_function_checks_exit_and_restores_gate(self):
        source=SOURCE.read_text();s=HARNESS.replace('__FUNCTIONS__','\n'.join(extract(source,n) for n in ['experimental_radio_action','queue_radio_restore','radio_recovery_thread_func','queue_radio_recovery','power_suspend_now']))
        with tempfile.TemporaryDirectory(prefix='radio-suspend-host-') as d:
            p=pathlib.Path(d);(p/'check.c').write_text(s)
            subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror',str(p/'check.c'),'-o',str(p/'check')],check=True)
            subprocess.run([str(p/'check')],check=True)

if __name__=='__main__':unittest.main()
