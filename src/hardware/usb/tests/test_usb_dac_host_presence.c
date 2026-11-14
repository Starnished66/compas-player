#define USB_MODE_CONTROL_ANDROID0_DIR "android0"
#define USB_MODE_CONTROL_ADB_DEMO_DIR "adb_demo"
#define USB_MODE_CONTROL_UDC_STATE_DIR "udc"
#include "../usb_mode_control.c"
#include <assert.h>

static void put(const char *path, const char *value) {
    FILE *f = fopen(path, "w");
    assert(f && fputs(value, f) >= 0 && fclose(f) == 0);
}
int main(void) {
    assert(mkdir("android0", 0700) == 0);
    assert(mkdir("adb_demo", 0700) == 0);
    assert(mkdir("udc", 0700) == 0);
    assert(mkdir("udc/test-controller", 0700) == 0);
    put("android0/UDC", "test-controller\n");
    assert(!usb_mode_control_host_connected());
    const char *negative[] = {"not attached\n", "powered\n", "attached\n", "unknown\n", "configured junk\n"};
    for (size_t i = 0; i < sizeof(negative)/sizeof(negative[0]); ++i) {
        put("udc/test-controller/state", negative[i]);
        assert(!usb_mode_control_host_connected());
    }
    const char *positive[] = {"configured\n", "suspended\n", "address\n", "addressed\n"};
    for (size_t i = 0; i < sizeof(positive)/sizeof(positive[0]); ++i) {
        put("udc/test-controller/state", positive[i]);
        assert(usb_mode_control_host_connected());
    }
    put("android0/UDC", "../test-controller\n");
    assert(!usb_mode_control_host_connected());
    put("android0/UDC", "\n");
    put("adb_demo/UDC", "test-controller\n");
    assert(usb_mode_control_host_connected());
    puts("USB DAC data-host presence tests passed");
}
