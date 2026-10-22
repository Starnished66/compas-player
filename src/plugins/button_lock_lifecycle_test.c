/* Real Lua/plugin-manager lifecycle regression; run with the host harness. */
#include "plugin_manager.h"
#include "hw_buttons.h"
#include "lvgl/lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

void install_thread_crash_altstack(void) {}
void main_loop_discard_elapsed_time(void) {}

static void write_plugin(const char *body) {
    FILE *f = fopen("music/.plugins/Test.lua", "w");
    assert(f);
    assert(fputs("plugin.define({id='test.button_lock', name='Test', version='1', api_min=18})\n", f) >= 0);
    assert(fputs(body, f) >= 0);
    assert(fclose(f) == 0);
}

int main(void) {
    lv_init();
    assert(mkdir("music", 0700) == 0);
    assert(mkdir("music/.plugins", 0700) == 0);
    write_plugin("plugin.configure_button_lock(true)\nerror('intentional load failure')\n");
    plugin_manager_init();
    assert(!hw_buttons_button_lock_is_enabled());
    plugin_manager_deinit();

    write_plugin("plugin.configure_button_lock(true)\n"
                 "guard = setmetatable({}, {__gc=function() pcall(plugin.configure_button_lock, true) end})\n");
    plugin_manager_init();
    assert(hw_buttons_button_lock_is_enabled());
    plugin_manager_deinit();
    assert(!hw_buttons_button_lock_is_enabled());
    assert(!hw_buttons_button_lock_is_locked());
    /* A second plugin cannot disarm or take over the active owner's lock. */
    for (int attempt = 0; attempt < 2; attempt++) {
        FILE *other = fopen("music/.plugins/ZZOther.lua", "w");
        assert(other);
        assert(fputs("plugin.define({id='test.other', name='Other', version='1', api_min=18})\n", other) >= 0);
        assert(fputs(attempt ? "plugin.configure_button_lock(true)\n" :
                               "plugin.configure_button_lock(false)\n", other) >= 0);
        assert(fclose(other) == 0);
        plugin_manager_init();
        assert(hw_buttons_button_lock_is_enabled());
        plugin_manager_deinit();
        assert(!hw_buttons_button_lock_is_enabled());
    }
    puts("Button-lock plugin lifecycle: PASS");
    return 0;
}
