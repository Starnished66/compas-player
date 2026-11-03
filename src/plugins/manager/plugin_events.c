/* plugin.on() events, intervals, remote stream resolver polling and the notify_* entry points. */
#include "../../ui/text/i18n/i18n.h"
#include "../../ui/application/gui.h"
#include "../../audio/output/audio_output.h"
#include "../../hardware/device/headphone_status.h"
#include "../lua/plugin_lua_runtime.h"
#include <string.h>
#include "plugin_manager_internal.h"

/* Shared by every numeric remote-track field below -- rejects (rather than
 * silently wrapping/truncating via an unsigned cast, which is what a bare
 * "(uint32_t) luaL_optnumber(...)" would otherwise do) a negative value, a
 * NaN, an infinity, or anything past max_value. A plain lua_Number
 * comparison against a finite max_value already correctly rejects NaN
 * (every comparison against NaN is false) and +-infinity (always outside
 * any finite range) with no separate isnan()/isfinite() check needed. */
static lua_Number check_bounded_number(lua_State * L, int table_index, const char * field, const char * fn_name, lua_Number max_value) {
    lua_getfield(L, table_index, field);
    lua_Number val = luaL_optnumber(L, -1, 0);
    lua_pop(L, 1);
    if (!(val >= 0) || !(val <= max_value)) {
        luaL_error(L, "%s: %s must be a finite number between 0 and %.0f", fn_name, field, (double) max_value);
    }
    return val;
}

/* Shared link fields for queue descriptors and asynchronous resolver answers. */
static void parse_remote_stream_fields(lua_State * L, int index, remote_track_meta_t * out,
                                      const char * fn, bool require_url) {
    lua_getfield(L, index, "stream_url");
    const char * url = luaL_optstring(L, -1, "");
    if (require_url && !url[0]) luaL_error(L, "%s: stream_url must not be empty", fn);
    if (strlen(url) >= sizeof(out->stream_url)) luaL_error(L, "%s: stream_url is too long", fn);
    snprintf(out->stream_url, sizeof(out->stream_url), "%s", url);
    lua_pop(L, 1);
    lua_getfield(L, index, "codec");
    const char * codec = luaL_optstring(L, -1, "");
    if (codec[0] && strcasecmp(codec, "mp3") && strcasecmp(codec, "flac") && strcasecmp(codec, "aac"))
        luaL_error(L, "%s: unknown codec '%s' -- must be mp3, flac, aac, or omitted", fn, codec);
    if (strlen(codec) >= sizeof(out->codec)) luaL_error(L, "%s: codec is too long", fn);
    snprintf(out->codec, sizeof(out->codec), "%s", codec);
    lua_pop(L, 1);
    out->sample_rate = (unsigned int) check_bounded_number(L, index, "sample_rate", fn, 1000000.0);
    out->bit_depth = (unsigned int) check_bounded_number(L, index, "bit_depth", fn, 64.0);
    out->channels = (unsigned int) check_bounded_number(L, index, "channels", fn, 64.0);
    out->bitrate_kbps = (unsigned int) check_bounded_number(L, index, "bitrate_kbps", fn, 100000.0);
    lua_getfield(L, index, "url_ttl_s");
    if (!lua_isnil(L, -1)) {
        lua_Integer ttl = luaL_checkinteger(L, -1);
        if (ttl < 1 || ttl > 86400) luaL_error(L, "%s: url_ttl_s must be between 1 and 86400", fn);
        out->url_ttl_s = (uint32_t) ttl;
    }
    lua_pop(L, 1);
}

/* Reads one remote_track_meta_t out of the Lua table at stack index
 * table_index (a plain {provider=..., track_id=..., stream_url=..., ...}
 * value, not a class/metatable of any kind). provider/track_id/stream_url
 * identify the track; stream_url may be omitted for on-demand resolution.
 * Other fields default to empty/zero/true(verify_tls),
 * matching plugin.http_request()'s own required-vs-optional-field style
 * above. Raises a Lua error (via luaL_error, which never returns) on any
 * oversized or missing-required field rather than silently truncating --
 * same reasoning as l_plugin_http_request()'s own header-length fix. */
static void parse_remote_track_table(lua_State * L, int table_index, remote_track_meta_t * out, const char * fn_name) {
    memset(out, 0, sizeof(*out));

    lua_getfield(L, table_index, "provider");
    const char * provider = luaL_checkstring(L, -1);
    if (provider[0] == '\0') luaL_error(L, "%s: provider must not be empty", fn_name);
    if (strlen(provider) >= sizeof(out->provider)) luaL_error(L, "%s: provider is too long", fn_name);
    snprintf(out->provider, sizeof(out->provider), "%s", provider);
    lua_pop(L, 1);

    lua_getfield(L, table_index, "track_id");
    const char * track_id = luaL_checkstring(L, -1);
    if (track_id[0] == '\0') luaL_error(L, "%s: track_id must not be empty", fn_name);
    if (strlen(track_id) >= sizeof(out->track_id)) luaL_error(L, "%s: track_id is too long", fn_name);
    snprintf(out->track_id, sizeof(out->track_id), "%s", track_id);
    lua_pop(L, 1);

    /* Reuses remote_track_make_key()'s own validation (non-empty, fits,
     * and -- the part a plain non-empty/length check above can't catch --
     * no '/' or control character in either field, which would otherwise
     * let two different (provider, track_id) pairs produce the exact same
     * "remote://..." key and collide in Favorites/History/play-count).
     * Only the validity is used here; the key itself is recomputed
     * on-demand wherever it's actually needed (remote_track.c). */
    {
        char key[256];
        if (!remote_track_make_key(out->provider, out->track_id, key, sizeof(key))) {
            luaL_error(L, "%s: provider/track_id must not contain '/' or control characters, and must fit the synthetic key", fn_name);
        }
    }

    parse_remote_stream_fields(L, table_index, out, fn_name, false);

#define OPT_STR_FIELD(field, name) \
    lua_getfield(L, table_index, name); \
    const char * field##_val = luaL_optstring(L, -1, ""); \
    if (strlen(field##_val) >= sizeof(out->field)) luaL_error(L, "%s: " name " is too long", fn_name); \
    snprintf(out->field, sizeof(out->field), "%s", field##_val); \
    lua_pop(L, 1);

    OPT_STR_FIELD(title, "title")
    OPT_STR_FIELD(artist, "artist")
    OPT_STR_FIELD(album, "album")
    OPT_STR_FIELD(artwork_url, "artwork_url")
#undef OPT_STR_FIELD

    /* Bounds are generous (well past anything a real audio stream would
     * ever declare) -- these are display-only fields (format badge,
     * quality metadata) except duration_ms, which also seeds the decoder's
     * total_frames for a remote MP3/AAC stream (audio_decoder_open.c's decoder_open());
     * the point is rejecting garbage (negative/NaN/absurdly large) before
     * it's cast to an unsigned type below, not modeling real codec limits. */
    out->duration_ms = (uint32_t) check_bounded_number(L, table_index, "duration_ms", fn_name, 24.0 * 3600.0 * 1000.0);

    lua_getfield(L, table_index, "replaygain_db");
    if (!lua_isnil(L, -1)) {
        double gain = luaL_checknumber(L, -1);
        /* +-100 dB is already an absurd amount of gain -- this is purely a
         * NaN/infinity/garbage-value guard (see resolve_replaygain()'s own
         * comment for how this feeds into playback volume), not a
         * meaningful real-world limit. */
        if (!(gain >= -100.0 && gain <= 100.0)) luaL_error(L, "%s: replaygain_db must be a finite number between -100 and 100", fn_name);
        out->has_replaygain = true;
        out->replaygain_db = gain;
    }
    lua_pop(L, 1);

    lua_getfield(L, table_index, "verify_tls");
    out->verify_tls = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
    lua_pop(L, 1);
    lua_State * owner = plugin_lua_main_state(L);
    plugin_instance_t * inst = plugin_instance_for_state(owner);
    if (!inst || inst->disposing || inst->aborted)
        luaL_error(L, "%s: plugin is no longer active", fn_name);
    out->resolver_owner = (uintptr_t) owner;
    snprintf(out->resolver_plugin, sizeof(out->resolver_plugin), "%s", inst->id);
    out->resolve_stream = plugin_remote_resolver_registered(owner);
}

static int l_plugin_play_remote(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    remote_track_meta_t track;
    parse_remote_track_table(L, 1, &track, "plugin.play_remote");
    gui_plugin_play_remote_tracks(&track, 1, 0);
    return 0;
}

static int l_plugin_queue_remote_list(lua_State * L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int start = (int) luaL_optinteger(L, 2, 1) - 1;

    lua_Unsigned raw_n = lua_rawlen(L, 1);
    int n = (raw_n > (lua_Unsigned) PLUGIN_MAX_LIST_ITEMS) ? PLUGIN_MAX_LIST_ITEMS : (int) raw_n;
    if (n <= 0) return 0;
    if (start < 0) start = 0;
    if (start >= n) start = n - 1;

    /* remote_track_meta_t is a few KB each (mostly its bounded string
     * fields) -- a fixed PLUGIN_MAX_LIST_ITEMS-sized static array here
     * would be a ~2MB permanent footprint on this device regardless of how
     * many tracks a given call actually queues. Sized to n (the real,
     * already-capped count) instead, and allocated as Lua userdata rather
     * than malloc()'d: parse_remote_track_table() below can luaL_error()
     * (longjmp) mid-loop on a malformed entry, and a plain malloc()'d
     * buffer would leak in that case since nothing downstream of a longjmp
     * ever runs our own free() -- Lua's GC owns and reclaims userdata
     * regardless of how the call unwinds, once nothing pushed after it on
     * the stack (nothing is, here) keeps it live past this function. */
    remote_track_meta_t * tracks = (remote_track_meta_t *) lua_newuserdata(L, sizeof(remote_track_meta_t) * (size_t) n);
    for (int i = 0; i < n; i++) {
        lua_rawgeti(L, 1, i + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        parse_remote_track_table(L, lua_gettop(L), &tracks[i], "plugin.queue_remote_list");
        lua_pop(L, 1);
    }

    gui_plugin_play_remote_tracks(tracks, n, start);
    return 0;
}

/* Plugin errors are English. A known reason becomes the player's own
 * translated message; otherwise the plugin text is kept only for English. */
static const char * remote_stream_error_text(const char * reason, const char * error) {
    static const struct { const char * reason; const char * text; } reasons[] = {
        { "subscription", N_("Not available in your subscription") },
        { "unavailable", N_("This track is unavailable") },
        { "quality", N_("This quality is unavailable for this track") },
        { "protected", N_("This track is copy-protected") },
        { "network", N_("Could not reach the streaming service") },
        { "auth", N_("Sign in to the streaming service again") },
    };
    if (reason)
        for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++)
            if (!strcmp(reason, reasons[i].reason)) return reasons[i].text;
    const char * language = i18n_get_language();
    bool english = !language || !language[0] || !strncmp(language, "en", 2);
    return english && error && error[0] ? error : N_("Could not get a stream link");
}

static int l_plugin_provide_remote_stream(lua_State * L) {
    lua_Integer id = luaL_checkinteger(L, 1);
    if (id <= 0) return luaL_error(L, "plugin.provide_remote_stream: invalid request id");
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_State * owner = plugin_lua_main_state(L);
    plugin_instance_t * inst = plugin_instance_for_state(owner);
    if (!inst || inst->disposing || inst->aborted) {
        lua_pushboolean(L, false);
        return 1;
    }
    lua_getfield(L, 2, "error");
    const char * error = luaL_optstring(L, -1, NULL);
    lua_getfield(L, 2, "reason");
    const char * reason = luaL_optstring(L, -1, NULL);
    if ((error && error[0]) || (reason && reason[0])) {
        lua_pushboolean(L, remote_track_stream_provide((uintptr_t) owner, (uint64_t) id, NULL,
                                                       remote_stream_error_text(reason, error)));
    } else {
        remote_track_meta_t stream = {0};
        parse_remote_stream_fields(L, 2, &stream, "plugin.provide_remote_stream", true);
        lua_pushboolean(L, remote_track_stream_provide((uintptr_t) owner, (uint64_t) id, &stream, NULL));
    }
    return 1;
}

typedef struct {
    lua_State * L;
    int ref;
} plugin_event_subscriber_t;

static plugin_event_subscriber_t plugin_event_subscribers[PLUGIN_EVENT_COUNT][PLUGIN_MAX_EVENT_SUBSCRIBERS];
int plugin_event_subscriber_count[PLUGIN_EVENT_COUNT];

/* Event dispatch iterator: advances *i over idx's subscribers, pushes the next
 * live one's callback and returns its state, or NULL when done. The count is
 * re-read each step because a handler may call plugin.on(). Pair every hit
 * with plugin_call_report(). */
static lua_State * plugin_event_next(plugin_event_t idx, int * i) {
    while (*i < plugin_event_subscriber_count[idx]) {
        const plugin_event_subscriber_t * sub = &plugin_event_subscribers[idx][(*i)++];
        if (plugin_push_callback(sub->L, sub->ref)) return sub->L;
    }
    return NULL;
}
/* Callback ref of L's remote_stream_needed subscriber, or LUA_NOREF. */
static int plugin_remote_resolver_ref(lua_State * L) {
    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_REMOTE_STREAM_NEEDED]; i++)
        if (plugin_event_subscribers[PLUGIN_EVENT_REMOTE_STREAM_NEEDED][i].L == L)
            return plugin_event_subscribers[PLUGIN_EVENT_REMOTE_STREAM_NEEDED][i].ref;
    return LUA_NOREF;
}

bool plugin_remote_resolver_registered(lua_State * L) {
    return plugin_remote_resolver_ref(L) != LUA_NOREF;
}

/* UI/Lua-thread dispatch only. The audio thread queues ids, never Lua pointers
 * or callbacks; reload invalidates all ids before any state is closed. */
void plugin_remote_stream_poll(void) {
    remote_stream_request_t request;
    for (int budget = 0; budget < 4 && remote_track_stream_take_request(&request); budget++) {
        lua_State * L = (lua_State *) request.owner;
        plugin_instance_t * inst = plugin_instance_for_state(L);
        if (!inst || inst->disposing || inst->aborted) {
            remote_track_stream_provide(request.owner, request.request_id, NULL, N_("The streaming plugin is not available"));
            continue;
        }
        int ref = plugin_remote_resolver_ref(L);
        if (ref == LUA_NOREF) {
            remote_track_stream_provide(request.owner, request.request_id, NULL, N_("The streaming plugin is not available"));
            continue;
        }
        int top = lua_gettop(L);
        if (!plugin_push_callback(L, ref)) continue;
        lua_pushstring(L, request.provider);
        lua_pushstring(L, request.track_id);
        lua_pushinteger(L, (lua_Integer) request.request_id);
        if (plugin_call(L, 3, 0, 0) != LUA_OK) {
            remote_track_stream_provide(request.owner, request.request_id, NULL, N_("Could not get a stream link"));
        }
        if (!inst->aborted) lua_settop(L, top);
    }
}

int plugin_last_volume_percent = -1;
static bool plugin_battery_snapshot_valid = false;
static plugin_battery_info_t plugin_last_battery_info;
bool plugin_output_snapshot_valid;
typedef struct {
    int route, requested_route, headphones;
    bool active, bluetooth;
    char bluetooth_codec[32];
} plugin_output_snapshot_t;
static plugin_output_snapshot_t plugin_last_output_snapshot;

static void plugin_output_snapshot(plugin_output_snapshot_t * s) {
    audio_output_info_t out = {0};
#ifndef HOST_BUILD
    audio_output_get_info(&out);
#else
    out.route = AUDIO_OUTPUT_ROUTE_WIRED;
    out.requested_route = AUDIO_OUTPUT_ROUTE_WIRED;
#endif
    s->route = out.active ? (int)out.route : -1;
    s->requested_route = (int)out.requested_route;
    s->active = out.active;
    s->bluetooth = gui_shell_is_bt_audio_connected();
    (void)gui_shell_get_bt_audio_codec(s->bluetooth_codec, sizeof(s->bluetooth_codec));
    s->headphones = (int)get_headphone_state();
}

static void push_output_snapshot(lua_State * L, const plugin_output_snapshot_t * s) {
    lua_newtable(L);
    lua_pushstring(L, s->route >= 0 && s->route < 3 ? plugin_route_names[s->route] : "inactive"); lua_setfield(L, -2, "route");
    lua_pushstring(L, s->requested_route >= 0 && s->requested_route < 3 ? plugin_route_names[s->requested_route] : "unknown"); lua_setfield(L, -2, "requested_route");
    lua_pushboolean(L, s->active); lua_setfield(L, -2, "active");
    lua_pushboolean(L, s->bluetooth); lua_setfield(L, -2, "bluetooth_connected");
    if (s->bluetooth_codec[0]) lua_pushstring(L, s->bluetooth_codec);
    else lua_pushnil(L);
    lua_setfield(L, -2, "bluetooth_codec");
    lua_pushinteger(L, s->headphones); lua_setfield(L, -2, "headphone_state");
}

/* Field by field: struct assignment need not copy padding, so memcmp could
 * report a change that never happened. */
static bool plugin_output_snapshot_equal(const plugin_output_snapshot_t * a,
                                         const plugin_output_snapshot_t * b) {
    return a->route == b->route && a->requested_route == b->requested_route &&
           a->headphones == b->headphones && a->active == b->active &&
           a->bluetooth == b->bluetooth &&
           strcmp(a->bluetooth_codec, b->bluetooth_codec) == 0;
}

void plugin_output_poll(void) {
    if (plugin_event_subscriber_count[PLUGIN_EVENT_OUTPUT_CHANGED] == 0) return;
    plugin_output_snapshot_t now = {0};
    plugin_output_snapshot(&now);
    if (!plugin_output_snapshot_valid) {
        plugin_last_output_snapshot = now;
        plugin_output_snapshot_valid = true;
        return;
    }
    if (plugin_output_snapshot_equal(&now, &plugin_last_output_snapshot)) return;
    plugin_output_snapshot_t previous = plugin_last_output_snapshot;
    plugin_last_output_snapshot = now;
    lua_State * sl;
    for (int i = 0; (sl = plugin_event_next(PLUGIN_EVENT_OUTPUT_CHANGED, &i)); ) {
        push_output_snapshot(sl, &now);
        push_output_snapshot(sl, &previous);
        plugin_call_report(sl, 2, false, "output_changed handler error");
    }
}

static int l_plugin_on(lua_State * L) {
    const char * event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    plugin_event_t idx;
    if (strcmp(event, "track_started") == 0) idx = PLUGIN_EVENT_TRACK_STARTED;
    else if (strcmp(event, "paused") == 0) idx = PLUGIN_EVENT_PAUSED;
    else if (strcmp(event, "resumed") == 0) idx = PLUGIN_EVENT_RESUMED;
    else if (strcmp(event, "stopped") == 0) idx = PLUGIN_EVENT_STOPPED;
    else if (strcmp(event, "screen_woke") == 0) idx = PLUGIN_EVENT_SCREEN_WOKE;
    else if (strcmp(event, "queue_exhausted") == 0) idx = PLUGIN_EVENT_QUEUE_EXHAUSTED;
    else if (strcmp(event, "volume_changed") == 0) idx = PLUGIN_EVENT_VOLUME_CHANGED;
    else if (strcmp(event, "battery_changed") == 0) idx = PLUGIN_EVENT_BATTERY_CHANGED;
    else if (strcmp(event, "suspending") == 0) idx = PLUGIN_EVENT_SUSPENDING;
    else if (strcmp(event, "system_resumed") == 0) idx = PLUGIN_EVENT_SYSTEM_RESUMED;
    else if (strcmp(event, "screenshot_saved") == 0) idx = PLUGIN_EVENT_SCREENSHOT_SAVED;
    else if (strcmp(event, "screenshot_failed") == 0) idx = PLUGIN_EVENT_SCREENSHOT_FAILED;
    else if (strcmp(event, "output_changed") == 0) idx = PLUGIN_EVENT_OUTPUT_CHANGED;
    else if (strcmp(event, "button_lock_changed") == 0) idx = PLUGIN_EVENT_BUTTON_LOCK_CHANGED;
    else if (strcmp(event, "remote_stream_needed") == 0) idx = PLUGIN_EVENT_REMOTE_STREAM_NEEDED;
    else if (strcmp(event, "network_changed") == 0) idx = PLUGIN_EVENT_NETWORK_CHANGED;
    else return luaL_error(L, "plugin.on: unknown event '%s'", event);

    if (idx == PLUGIN_EVENT_REMOTE_STREAM_NEEDED && plugin_remote_resolver_registered(plugin_lua_main_state(L)))
        return luaL_error(L, "plugin.on: only one remote_stream_needed resolver per plugin is allowed");

    if (plugin_event_subscriber_count[idx] >= PLUGIN_MAX_EVENT_SUBSCRIBERS) {
        return luaL_error(L, "plugin.on: too many subscribers registered for \"%s\" (max %d)", event,
                           PLUGIN_MAX_EVENT_SUBSCRIBERS);
    }

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    plugin_event_subscriber_t * sub = &plugin_event_subscribers[idx][plugin_event_subscriber_count[idx]++];
    sub->L = plugin_lua_main_state(L);
    sub->ref = ref;
    if (idx == PLUGIN_EVENT_REMOTE_STREAM_NEEDED) {
        /* A reloaded plugin takes over the queue its previous instance built. */
        plugin_instance_t * inst = plugin_instance_for_state(sub->L);
        if (inst && inst->defined) remote_track_stream_adopt_owner(inst->id, (uintptr_t) sub->L);
    }
    return 0;
}

plugin_interval_t plugin_intervals[PLUGIN_MAX_INTERVALS];

static int l_plugin_set_interval(lua_State * L) {
    double seconds = luaL_checknumber(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    int slot = -1;
    for (int i = 0; i < PLUGIN_MAX_INTERVALS; i++) {
        if (!plugin_intervals[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        return luaL_error(L, "plugin.set_interval: too many active intervals (max %d)", PLUGIN_MAX_INTERVALS);
    }

    uint32_t period_ms = (uint32_t) (seconds * 1000.0);
    if (period_ms < PLUGIN_INTERVAL_MIN_MS) period_ms = PLUGIN_INTERVAL_MIN_MS;

    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    plugin_intervals[slot].L = plugin_lua_main_state(L);
    plugin_intervals[slot].ref = ref;
    plugin_intervals[slot].active = true;

    gui_plugin_set_interval(slot, period_ms);
    lua_pushinteger(L, slot + 1);
    return 1;
}

static int l_plugin_clear_interval(lua_State * L) {
    lua_Integer handle = luaL_checkinteger(L, 1);
    int slot = (int) handle - 1;
    if (slot < 0 || slot >= PLUGIN_MAX_INTERVALS || !plugin_intervals[slot].active) return 0;

    plugin_unref(plugin_intervals[slot].L, plugin_intervals[slot].ref);
    plugin_intervals[slot].active = false;
    plugin_intervals[slot].L = NULL;
    gui_plugin_clear_interval(slot);
    return 0;
}

void discard_failed_plugin_events(lua_State * L, bool aborted) {
    remote_track_stream_cancel_owner((uintptr_t) L);
    for (int event = 0; event < PLUGIN_EVENT_COUNT; event++) {
        int out = 0;
        for (int i = 0; i < plugin_event_subscriber_count[event]; i++) {
            plugin_event_subscriber_t item = plugin_event_subscribers[event][i];
            if (failed_load_owner_matches(item.L, L)) {
                if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, item.ref);
                continue;
            }
            plugin_event_subscribers[event][out++] = item;
        }
        memset(&plugin_event_subscribers[event][out], 0,
               (size_t)(plugin_event_subscriber_count[event] - out) * sizeof(plugin_event_subscribers[event][0]));
        plugin_event_subscriber_count[event] = out;
    }
}

void discard_failed_plugin_intervals(lua_State * L, bool aborted) {
    for (int i = 0; i < PLUGIN_MAX_INTERVALS; i++) {
        if (!plugin_intervals[i].active || !failed_load_owner_matches(plugin_intervals[i].L, L)) continue;
        if (!aborted) luaL_unref(L, LUA_REGISTRYINDEX, plugin_intervals[i].ref);
        plugin_intervals[i].active = false;
        plugin_intervals[i].L = NULL;
        plugin_intervals[i].ref = LUA_NOREF;
        gui_plugin_clear_interval(i);
    }
}

void plugin_manager_notify_track_started(const char * title, const char * artist, const char * album,
                                          double duration_seconds, const char * provider, const char * track_id) {
    lua_State * sl;
    for (int i = 0; (sl = plugin_event_next(PLUGIN_EVENT_TRACK_STARTED, &i)); ) {
        lua_pushstring(sl, title ? title : "");
        lua_pushstring(sl, artist ? artist : "");
        lua_pushstring(sl, album ? album : "");
        lua_pushnumber(sl, duration_seconds);
        lua_pushstring(sl, provider ? provider : "");
        lua_pushstring(sl, track_id ? track_id : "");
        plugin_call_report(sl, 6, false, "track_started handler error");
    }
}

/* Shared by plugin_manager_notify_paused()/_resumed()/_stopped() below --
 * the three zero-argument events. */
static void notify_event_no_args(plugin_event_t idx, const char * kind) {
    lua_State * sl;
    for (int i = 0; (sl = plugin_event_next(idx, &i)); ) {
        plugin_call_report(sl, 0, false, "%s handler error", kind);
    }
}

void plugin_manager_notify_paused(void) {
    notify_event_no_args(PLUGIN_EVENT_PAUSED, "paused");
}

void plugin_manager_notify_resumed(void) {
    notify_event_no_args(PLUGIN_EVENT_RESUMED, "resumed");
}

void plugin_manager_notify_stopped(void) {
    notify_event_no_args(PLUGIN_EVENT_STOPPED, "stopped");
}

void plugin_manager_notify_screen_woke(void) {
    notify_event_no_args(PLUGIN_EVENT_SCREEN_WOKE, "screen_woke");
}

void plugin_manager_notify_button_lock_changed(bool locked) {
    lua_State * sl;
    for (int i = 0; (sl = plugin_event_next(PLUGIN_EVENT_BUTTON_LOCK_CHANGED, &i)); ) {
        lua_pushboolean(sl, locked);
        plugin_call_report(sl, 1, false, "button_lock_changed handler error");
    }
}

void plugin_manager_notify_queue_exhausted(int direction) {
    lua_State * sl;
    for (int i = 0; (sl = plugin_event_next(PLUGIN_EVENT_QUEUE_EXHAUSTED, &i)); ) {
        lua_pushnumber(sl, direction);
        plugin_call_report(sl, 1, false, "queue_exhausted handler error");
    }
}

void plugin_manager_notify_volume_changed(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (percent == plugin_last_volume_percent) return;
    plugin_last_volume_percent = percent;

    lua_State * sl;
    for (int i = 0; (sl = plugin_event_next(PLUGIN_EVENT_VOLUME_CHANGED, &i)); ) {
        lua_pushinteger(sl, percent);
        plugin_call_report(sl, 1, false, "volume_changed handler error");
    }
}

static bool battery_info_equal(const plugin_battery_info_t * a, const plugin_battery_info_t * b) {
    return a->level == b->level && a->charging == b->charging && a->full == b->full &&
           a->external_power == b->external_power;
}

void plugin_manager_poll_battery(void) {
    plugin_battery_info_t current = plugin_read_battery_info();
    if (!plugin_battery_snapshot_valid) {
        plugin_last_battery_info = current;
        plugin_battery_snapshot_valid = true;
        return;
    }
    if (battery_info_equal(&current, &plugin_last_battery_info)) return;
    plugin_last_battery_info = current;

    lua_State * sl;
    for (int i = 0; (sl = plugin_event_next(PLUGIN_EVENT_BATTERY_CHANGED, &i)); ) {
        push_battery_info(sl, &current);
        plugin_call_report(sl, 1, false, "battery_changed handler error");
    }
}

/* Cached Wi-Fi state, written only by the UI thread through
 * plugin_manager_set_network_state(); the getter reads it without any
 * subprocess or socket access. */
static bool plugin_network_connected;
static bool plugin_network_wifi_enabled;
static bool plugin_network_valid;

static void push_network_info(lua_State * L) {
    lua_createtable(L, 0, 2);
    lua_pushboolean(L, plugin_network_connected);
    lua_setfield(L, -2, "connected");
    lua_pushboolean(L, plugin_network_wifi_enabled);
    lua_setfield(L, -2, "wifi_enabled");
}

int l_plugin_network_status(lua_State * L) {
    push_network_info(L);
    return 1;
}

bool plugin_manager_has_network_subscribers(void) {
    for (int i = 0; i < plugin_event_subscriber_count[PLUGIN_EVENT_NETWORK_CHANGED]; i++) {
        plugin_instance_t * inst = plugin_instance_for_state(
            plugin_event_subscribers[PLUGIN_EVENT_NETWORK_CHANGED][i].L);
        if (!inst || !inst->aborted) return true;
    }
    return false;
}

void plugin_manager_set_network_state(bool connected, bool wifi_enabled) {
    if (!wifi_enabled) connected = false;
    if (plugin_network_valid && connected == plugin_network_connected &&
        wifi_enabled == plugin_network_wifi_enabled) return;
    bool first = !plugin_network_valid;
    plugin_network_connected = connected;
    plugin_network_wifi_enabled = wifi_enabled;
    plugin_network_valid = true;
    /* The first sample is only the baseline. It is always {false, enabled}
     * because it is taken before any query returns; the first query reporting
     * connected then arrives as a normal change event. */
    if (first && !connected) return;

    lua_State * sl;
    for (int i = 0; (sl = plugin_event_next(PLUGIN_EVENT_NETWORK_CHANGED, &i)); ) {
        push_network_info(sl);
        plugin_call_report(sl, 1, false, "network_changed handler error");
    }
}

void plugin_manager_notify_suspending(void) {
    notify_event_no_args(PLUGIN_EVENT_SUSPENDING, "suspending");
}

void plugin_manager_notify_system_resumed(void) {
    notify_event_no_args(PLUGIN_EVENT_SYSTEM_RESUMED, "system_resumed");
}

static void notify_event_string(plugin_event_t idx, const char * kind, const char * value) {
    lua_State * sl;
    for (int i = 0; (sl = plugin_event_next(idx, &i)); ) {
        lua_pushstring(sl, value ? value : "");
        plugin_call_report(sl, 1, false, "%s handler error", kind);
    }
}

void plugin_manager_notify_screenshot_saved(const char * path) {
    notify_event_string(PLUGIN_EVENT_SCREENSHOT_SAVED, "screenshot_saved", path);
}

void plugin_manager_notify_screenshot_failed(const char * reason) {
    notify_event_string(PLUGIN_EVENT_SCREENSHOT_FAILED, "screenshot_failed", reason);
}

void plugin_manager_interval_fired(int slot) {
    if (slot < 0 || slot >= PLUGIN_MAX_INTERVALS || !plugin_intervals[slot].active) return;

    lua_State * L = plugin_intervals[slot].L;
    if (!plugin_push_callback(L, plugin_intervals[slot].ref)) return;
    plugin_call_report(L, 0, false, "set_interval callback error");
}

const luaL_Reg plugin_events_funcs[] = {
    { "network_status",            l_plugin_network_status },
    { "play_remote",               l_plugin_play_remote },
    { "queue_remote_list",         l_plugin_queue_remote_list },
    { "provide_remote_stream",     l_plugin_provide_remote_stream },
    { "on",                        l_plugin_on },
    { "set_interval",              l_plugin_set_interval },
    { "clear_interval",            l_plugin_clear_interval },
    { NULL, NULL }
};
