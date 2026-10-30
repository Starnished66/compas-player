#include "plugin_archive_bindings.h"
#include "../../lua/plugin_lua_runtime.h"
#include "../../storage/plugin_reserved_dirs.h"

#include "lauxlib.h"
#include "lualib.h"
#include "../../../core/text/zip_reader.h"
#include "../../../library/artwork/cache/image_thumb.h"
#include "../../../core/ui/display/startup_splash.h"
#include "../../../core/system/runtime/board_config.h"
#include <lvgl/lvgl.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

static void free_bytes(void * mem) {
    free(mem);
}

typedef struct {
    const unsigned char * data;
    size_t len;
    zip_entry_info info;
} zip_read_lua_ctx;

static int zip_read_push_lua(lua_State * L) {
    zip_read_lua_ctx * ctx = lua_touserdata(L, 1);
    if (ctx->data) lua_pushlstring(L, (const char *) ctx->data, ctx->len);
    else lua_pushlstring(L, "", 0);

    lua_createtable(L, 0, 3);
    lua_pushinteger(L, ctx->info.method);
    lua_setfield(L, -2, "method");
    lua_pushinteger(L, (lua_Integer) ctx->info.compressed);
    lua_setfield(L, -2, "compressed");
    lua_pushinteger(L, (lua_Integer) ctx->info.uncompressed);
    lua_setfield(L, -2, "uncompressed");
    return 2;
}

/* Path goes through plugin_lua_check_external_path so reserved paths raise an error instead of returning nil */
int l_plugin_zip_read(lua_State * L) {
    const char * path = plugin_lua_check_external_path(L, 1, "plugin.zip_read");
    const char * entry = luaL_checkstring(L, 2);

    /* Reserve the closure slot before the native buffer exists. */
    if (!lua_checkstack(L, 2)) return luaL_error(L, "stack overflow");
    lua_pushcfunction(L, zip_read_push_lua);

    unsigned char * data = NULL;
    size_t data_len = 0;
    zip_entry_info info;
    zip_status st = zip_read_entry(path, entry, &data, &data_len, &info);
    if (st != ZIP_OK) {
        lua_pop(L, 1);
        free(data);
        lua_pushnil(L);
        lua_pushstring(L, zip_status_reason(st));
        return 2;
    }

    zip_read_lua_ctx ctx = { data, data_len, info };
    return plugin_lua_pcall_then_cleanup(L, &ctx, 2, free_bytes, data);
}

typedef struct {
    char ** names;
    size_t count;
} zip_list_lua_ctx;

static void zip_list_lua_cleanup(void * mem) {
    zip_list_lua_ctx * ctx = mem;
    zip_free_names(ctx->names, ctx->count);
    ctx->names = NULL;
    ctx->count = 0;
}

static int zip_list_push_lua(lua_State * L) {
    zip_list_lua_ctx * ctx = lua_touserdata(L, 1);
    lua_createtable(L, (int) ctx->count, 0);
    for (size_t i = 0; i < ctx->count; i++) {
        lua_pushstring(L, ctx->names[i]);
        lua_rawseti(L, -2, (int) (i + 1));
    }
    return 1;
}

int l_plugin_zip_list(lua_State * L) {
    const char * path = plugin_lua_check_external_path(L, 1, "plugin.zip_list");

    if (!lua_checkstack(L, 2)) return luaL_error(L, "stack overflow");
    lua_pushcfunction(L, zip_list_push_lua);

    zip_list_lua_ctx ctx = { NULL, 0 };
    zip_status st = zip_list_entries(path, &ctx.names, &ctx.count);
    if (st != ZIP_OK) {
        lua_pop(L, 1);
        zip_list_lua_cleanup(&ctx);
        lua_pushnil(L);
        lua_pushstring(L, zip_status_reason(st));
        return 2;
    }

    return plugin_lua_pcall_then_cleanup(L, &ctx, 1, zip_list_lua_cleanup, &ctx);
}

/* plugin.zip_image_async(zip_path, entry, dest_path, max_w, max_h, callback):
 * reads an image member of a ZIP (an EPUB cover or illustration), scales it
 * to fit max_w x max_h and writes an LVGL .bin image lv_image can draw from
 * the file with no decode. The read and decode run on a worker; the callback
 * gets (dest_path) or (nil, reason) from plugin_manager_poll(). One job at a
 * time across all plugins: decoding can take a few MB. */
#define PLUGIN_ZIP_IMAGE_MAX_BYTES (3u * 1024u * 1024u)
#define PLUGIN_ZIP_IMAGE_MAX_SIDE 800
#define PLUGIN_ZIP_IMAGE_STACK_SIZE (4 * 1024 * 1024) /* same as the album cover decode worker */

typedef struct {
    bool active;
    atomic_bool done;
    atomic_bool cancel;
    pthread_t thread;
    lua_State * L;  /* plugin main state: a coroutine that started the job may be collected */
    int callback_ref;
    char zip_path[PATH_MAX];
    char entry[512];
    char dest_path[PATH_MAX];
    int max_w;
    int max_h;
    bool source_is_file;
    bool wallpaper;
    bool startup_install;
    bool ok;
    const char * error; /* static string */
} plugin_zip_image_t;

static plugin_zip_image_t plugin_zip_image;

static bool plugin_zip_image_cancelled(void * user) {
    return atomic_load(&((plugin_zip_image_t *) user)->cancel);
}

/* Weak: tests that #include this file link without main.c, which
 * defines the real hook. */
extern void install_thread_crash_altstack(void) __attribute__((weak));

static void * plugin_zip_image_thread(void * arg) {
#ifdef __linux__
    (void) prctl(PR_SET_NAME, "pluginimg");
#endif
    if (install_thread_crash_altstack) install_thread_crash_altstack(); /* see its own comment (main.c) */
    plugin_zip_image_t * job = arg;
    if (job->startup_install) {
        job->ok = startup_splash_install(job->zip_path[0] ? job->zip_path : NULL,
                                         BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT, &job->error);
        atomic_store(&job->done, true);
        return NULL;
    }
    unsigned char * data = NULL;
    size_t len = 0;
    if (job->source_is_file) {
        int fd = open(job->zip_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
        struct stat st;
        if (fd < 0) {
            job->error = "could not open image";
        } else if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
            job->error = "not a regular file";
        } else if (st.st_size < 0 || (uint64_t) st.st_size > PLUGIN_ZIP_IMAGE_MAX_BYTES) {
            job->error = "image too large";
        } else {
            len = (size_t) st.st_size;
            data = malloc(len ? len : 1);
            if (!data) {
                job->error = "nomem";
            } else {
                size_t off = 0;
                while (off < len && !plugin_zip_image_cancelled(job)) {
                    ssize_t n = read(fd, data + off, len - off);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) break;
                    off += (size_t) n;
                }
                if (plugin_zip_image_cancelled(job)) job->error = "cancelled";
                else if (off != len) job->error = "could not read image";
            }
        }
        if (fd >= 0) close(fd);
    } else {
        zip_entry_info info;
        zip_status st = zip_read_entry_limited(job->zip_path, job->entry, PLUGIN_ZIP_IMAGE_MAX_BYTES,
                                               PLUGIN_ZIP_IMAGE_MAX_BYTES, &data, &len, &info);
        if (st != ZIP_OK) job->error = zip_status_reason(st);
    }
    if (!job->error && plugin_zip_image_cancelled(job)) {
        job->error = "cancelled";
    } else if (!job->error) {
        if (job->wallpaper)
            job->ok = image_wallpaper_write_bin(data, len, job->max_w, job->max_h, job->dest_path,
                                                ARTWORK_PRIO_THUMBNAIL, plugin_zip_image_cancelled, job,
                                                &job->error);
        else
            job->ok = image_thumb_write_bin(data, len, job->max_w, job->max_h, job->dest_path,
                                            ARTWORK_PRIO_THUMBNAIL, plugin_zip_image_cancelled, job,
                                            &job->error);
    }
    free(data);
    atomic_store(&job->done, true);
    return NULL;
}

static bool path_has_bin_extension(const char * path) {
    size_t n = strlen(path);
    return n > 4 && strcmp(path + n - 4, ".bin") == 0;
}

/* Startup splash override. The loader consumes a board-native RGB565 frame,
 * so its dimensions are the firmware constants even when LVGL is rotated. */
static int plugin_image_thumbnail_start(lua_State * L, const char * source_path, const char * entry,
                                       const char * dest_path, int max_w, int max_h,
                                       int callback_index, bool source_is_file, bool wallpaper,
                                       bool startup_install);

int l_plugin_set_startup_image_async(lua_State * L) {
    const char * path = NULL;
    if (!lua_isnoneornil(L, 1))
        path = plugin_lua_check_external_path(L, 1, "plugin.set_startup_image_async");
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (path && strlen(path) >= sizeof(plugin_zip_image.zip_path))
        return luaL_error(L, "plugin.set_startup_image_async: path too long");
    return plugin_image_thumbnail_start(L, path, NULL, "", BOARD_SCREEN_WIDTH,
                                        BOARD_SCREEN_HEIGHT, 2, true, false, true);
}

int l_plugin_has_startup_image(lua_State * L) {
    lua_pushboolean(L, startup_splash_configured());
    return 1;
}

/* Starts the shared ZIP/file thumbnail job after each Lua API has validated
 * its own argument layout and emitted any API-specific argument errors. */
static int plugin_image_thumbnail_start(lua_State * L, const char * source_path, const char * entry,
                                       const char * dest_path, int max_w, int max_h,
                                       int callback_index, bool source_is_file, bool wallpaper,
                                       bool startup_install) {
    if (dest_path[0] && plugin_path_in_program_dirs(dest_path))
        return luaL_error(L, "the program folders (.apps, .apps-data, .emulators) are reserved");
    if (plugin_zip_image.active) return plugin_lua_push_error(L, "busy");

    plugin_zip_image_t * job = &plugin_zip_image;
    memset(job, 0, sizeof(*job));
    atomic_init(&job->done, false);
    atomic_init(&job->cancel, false);
    if (source_path) snprintf(job->zip_path, sizeof(job->zip_path), "%s", source_path);
    if (entry) snprintf(job->entry, sizeof(job->entry), "%s", entry);
    snprintf(job->dest_path, sizeof(job->dest_path), "%s", dest_path);
    job->max_w = max_w;
    job->max_h = max_h;
    job->source_is_file = source_is_file;
    job->wallpaper = wallpaper;
    job->startup_install = startup_install;
    lua_pushvalue(L, callback_index);
    job->callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    job->L = plugin_lua_main_state(L);

    pthread_attr_t attr;
    bool attr_ok = pthread_attr_init(&attr) == 0;
    if (attr_ok) pthread_attr_setstacksize(&attr, PLUGIN_ZIP_IMAGE_STACK_SIZE);
    int rc = pthread_create(&job->thread, attr_ok ? &attr : NULL, plugin_zip_image_thread, job);
    if (attr_ok) pthread_attr_destroy(&attr);
    if (rc != 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, job->callback_ref);
        job->callback_ref = LUA_NOREF;
        job->L = NULL;
        return plugin_lua_push_error(L, "could not start image worker");
    }
    job->active = true;
    lua_pushboolean(L, true);
    return 1;
}

int l_plugin_zip_image_async(lua_State * L) {
    const char * zip_path = plugin_lua_check_external_path(L, 1, "plugin.zip_image_async");
    const char * entry = luaL_checkstring(L, 2);
    const char * dest_path = plugin_lua_check_external_path(L, 3, "plugin.zip_image_async");
    lua_Integer max_w = luaL_checkinteger(L, 4);
    lua_Integer max_h = luaL_checkinteger(L, 5);
    luaL_checktype(L, 6, LUA_TFUNCTION);
    if (max_w < 16 || max_w > PLUGIN_ZIP_IMAGE_MAX_SIDE || max_h < 16 || max_h > PLUGIN_ZIP_IMAGE_MAX_SIDE)
        return luaL_error(L, "plugin.zip_image_async: max_w/max_h must be 16..%d", PLUGIN_ZIP_IMAGE_MAX_SIDE);
    if (strlen(zip_path) >= sizeof(plugin_zip_image.zip_path) || strlen(entry) >= sizeof(plugin_zip_image.entry) ||
        strlen(dest_path) >= sizeof(plugin_zip_image.dest_path) - 8)
        return luaL_error(L, "plugin.zip_image_async: path too long");
    if (!path_has_bin_extension(dest_path))
        return luaL_error(L, "plugin.zip_image_async: dest_path must end in .bin");
    return plugin_image_thumbnail_start(L, zip_path, entry, dest_path, (int) max_w, (int) max_h, 6, false, false, false);
}

int l_plugin_image_thumbnail_async(lua_State * L) {
    const char * source_path = plugin_lua_check_external_path(L, 1, "plugin.image_thumbnail_async");
    const char * dest_path = plugin_lua_check_external_path(L, 2, "plugin.image_thumbnail_async");
    lua_Integer max_w = luaL_checkinteger(L, 3);
    lua_Integer max_h = luaL_checkinteger(L, 4);
    luaL_checktype(L, 5, LUA_TFUNCTION);
    if (max_w < 16 || max_w > PLUGIN_ZIP_IMAGE_MAX_SIDE || max_h < 16 || max_h > PLUGIN_ZIP_IMAGE_MAX_SIDE)
        return luaL_error(L, "plugin.image_thumbnail_async: max_w/max_h must be 16..%d", PLUGIN_ZIP_IMAGE_MAX_SIDE);
    if (strlen(source_path) >= sizeof(plugin_zip_image.zip_path) || strlen(dest_path) >= sizeof(plugin_zip_image.dest_path) - 8)
        return luaL_error(L, "plugin.image_thumbnail_async: path too long");
    if (!path_has_bin_extension(dest_path)) return luaL_error(L, "plugin.image_thumbnail_async: dest_path must end in .bin");
    return plugin_image_thumbnail_start(L, source_path, NULL, dest_path, (int) max_w, (int) max_h, 5, true, false, false);
}

int l_plugin_image_wallpaper_async(lua_State * L) {
    const char * source_path = plugin_lua_check_external_path(L, 1, "plugin.image_wallpaper_async");
    const char * dest_path = plugin_lua_check_external_path(L, 2, "plugin.image_wallpaper_async");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (strlen(source_path) >= sizeof(plugin_zip_image.zip_path) ||
        strlen(dest_path) >= sizeof(plugin_zip_image.dest_path) - 8)
        return luaL_error(L, "plugin.image_wallpaper_async: path too long");
    if (!path_has_bin_extension(dest_path))
        return luaL_error(L, "plugin.image_wallpaper_async: dest_path must end in .bin");
    lv_display_t * display = lv_display_get_default();
    int width = display ? lv_display_get_horizontal_resolution(display) : BOARD_SCREEN_WIDTH;
    int height = display ? lv_display_get_vertical_resolution(display) : BOARD_SCREEN_HEIGHT;
    if (width <= 0 || height <= 0)
        return luaL_error(L, "plugin.image_wallpaper_async: display dimensions unavailable");
    return plugin_image_thumbnail_start(L, source_path, NULL, dest_path, width, height, 3, true, true, false);
}

/* Board-native counterpart to image_wallpaper_async, for startup splash
 * images which are read before LVGL applies its display rotation. */
int l_plugin_image_startup_async(lua_State * L) {
    const char * source_path = plugin_lua_check_external_path(L, 1, "plugin.image_startup_async");
    const char * dest_path = plugin_lua_check_external_path(L, 2, "plugin.image_startup_async");
    luaL_checktype(L, 3, LUA_TFUNCTION);
    if (strlen(source_path) >= sizeof(plugin_zip_image.zip_path) ||
        strlen(dest_path) >= sizeof(plugin_zip_image.dest_path) - 8)
        return luaL_error(L, "plugin.image_startup_async: path too long");
    if (!path_has_bin_extension(dest_path))
        return luaL_error(L, "plugin.image_startup_async: dest_path must end in .bin");
    return plugin_image_thumbnail_start(L, source_path, NULL, dest_path,
                                        BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT,
                                        3, true, true, false);
}

/* Delivers a finished job's callback (skipped when cancelled or the plugin
 * was aborted) and frees the slot. */
void plugin_archive_bindings_poll(plugin_archive_guarded_call_fn guarded_call,
                                  plugin_archive_owner_aborted_fn owner_is_aborted) {
    plugin_zip_image_t * job = &plugin_zip_image;
    if (!job->active || !atomic_load(&job->done)) return;
    pthread_join(job->thread, NULL);
    job->active = false;
    lua_State * L = job->L;
    int ref = job->callback_ref;
    job->L = NULL;
    job->callback_ref = LUA_NOREF;
    if (owner_is_aborted && owner_is_aborted(L)) return; /* frozen state: never touched again */
    if (!atomic_load(&job->cancel)) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
        if (job->startup_install) {
            lua_pushboolean(L, job->ok);
            if (job->ok) lua_pushnil(L);
            else lua_pushstring(L, job->error ? job->error : "could not set startup image");
        } else if (job->ok) {
            lua_pushstring(L, job->dest_path);
            lua_pushnil(L);
        } else {
            lua_pushnil(L);
            lua_pushstring(L, job->error ? job->error : "could not read image");
        }
        if (guarded_call(L, 2, 0, 0) != LUA_OK) {
            const char * err = lua_tostring(L, -1);
            fprintf(stderr, "[plugins] zip_image_async callback error: %s\n", err ? err : "unknown error");
            lua_pop(L, 1);
        }
    }
    luaL_unref(L, LUA_REGISTRYINDEX, ref);
}

/* Stops the worker at its next cancellation point and waits. It does not
 * invoke Lua callbacks; registry refs are released only for safe states. */
static void plugin_zip_image_cancel(bool aborted,
                                    plugin_archive_owner_aborted_fn owner_is_aborted) {
    plugin_zip_image_t * job = &plugin_zip_image;
    if (!job->active) return;
    atomic_store(&job->cancel, true);
    pthread_join(job->thread, NULL);
    job->active = false;
    if (owner_is_aborted) aborted = owner_is_aborted(job->L);
    if (job->L && !aborted) luaL_unref(job->L, LUA_REGISTRYINDEX, job->callback_ref);
    job->L = NULL;
    job->callback_ref = LUA_NOREF;
}

void plugin_archive_bindings_clear_owner(lua_State * owner, bool aborted) {
    if (!plugin_zip_image.active || plugin_zip_image.L != owner) return;
    plugin_zip_image_cancel(aborted, NULL);
}

void plugin_archive_bindings_cancel_all(plugin_archive_owner_aborted_fn owner_is_aborted) {
    if (!plugin_zip_image.active) return;
    plugin_zip_image_cancel(false, owner_is_aborted);
}

bool plugin_archive_bindings_has_work(void) {
    return plugin_zip_image.active;
}
