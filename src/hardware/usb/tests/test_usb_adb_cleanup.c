#include "../usb_mode_control.c"
#include <assert.h>
static bool supervisor, daemon_alive, stubborn;
static int stopped, killed;
bool subprocess_run(char * const argv[], char * out, size_t size) {
    (void)out; (void)size;
    assert(!strcmp(argv[0], "/etc/init.d/adb/S440adb") && !strcmp(argv[1], "stop"));
    ++stopped;
    return true;
}
bool subprocess_is_running(const char * name) {
    return !strcmp(name, "adbserver.sh") ? supervisor : daemon_alive;
}
void subprocess_kill_all_matching(const char * name) {
    ++killed;
    if (stubborn) return;
    if (!strcmp(name, "adbserver.sh")) supervisor = false;
    else { assert(!supervisor); daemon_alive = false; }
}
int main(void) {
    supervisor = daemon_alive = true;
    assert(stop_usb_adb());
    assert(stopped == 1 && killed == 2 && !supervisor && !daemon_alive);
    supervisor = daemon_alive = stubborn = true;
    assert(!stop_usb_adb());
    assert(supervisor && daemon_alive);
    puts("USB ADB supervisor cleanup and failed-termination guard passed");
}
