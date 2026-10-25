/* Real Lua/plugin-manager integration for provider-owned remote stream links. */
#include "plugin_manager.h"
#include "remote_track.h"
#include "lvgl/lvgl.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void install_thread_crash_altstack(void) {}
void main_loop_discard_elapsed_time(void) {}
bool main_loop_discard_pointer_input(void) { return false; }

static int captured_count;
static int captured_start;
static uint64_t test_generation;
static remote_track_meta_t captured_first;

void __wrap_gui_plugin_play_remote_tracks(const remote_track_meta_t *tracks, int count, int start_index) {
    assert(tracks && count > 0);
    captured_count = count;
    captured_start = start_index;
    captured_first = tracks[0];
    char key[256];
    assert(remote_track_make_key(tracks[start_index].provider, tracks[start_index].track_id,
                                 key, sizeof(key)));
    assert(remote_track_meta_set_all(tracks, count));
    remote_track_stream_begin_playback(key, ++test_generation);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(text, f) >= 0);
    assert(fclose(f) == 0);
}

static bool read_bool(const char *path) {
    char buf[32] = {0};
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(buf, sizeof(buf), f) && strcmp(buf, "true\n") == 0;
    fclose(f);
    return ok;
}

static uint64_t read_id(const char *path) {
    FILE *f = fopen(path, "r");
    assert(f);
    unsigned long long id = 0;
    assert(fscanf(f, "%llu", &id) == 1);
    fclose(f);
    return (uint64_t)id;
}

static void write_legacy_plugin(void) {
    write_file("music/.plugins/Legacy.lua",
        "plugin.play_remote({provider='legacy', track_id='upfront', title='Legacy title', "
        "stream_url='https://legacy.invalid/live.mp3', codec='mp3'})\n");
}

static void write_owner_plugin(void) {
    write_file("music/.plugins/ZOwner.lua",
        "plugin.define({id='test.remote.owner', name='Owner', version='1', api_min=17})\n"
        "local tracks = {}\n"
        "for i=1,500 do\n"
        "  local id = tostring(i)\n"
        "  if i==1 then id='good' elseif i==2 then id='error' elseif i==3 then id='cancel' end\n"
        "  tracks[i]={provider='test',track_id=id,title=(i==1 and 'Stable title' or 'Track '..id),"
        "artist='Stable artist',album='Stable album',duration_ms=123000,replaygain_db=-4.5}\n"
        "end\n"
        "local ttl_ok = pcall(plugin.play_remote,{provider='bad',track_id='ttl',url_ttl_s=0})\n"
        "local codec_ok = pcall(plugin.play_remote,{provider='bad',track_id='codec',codec='opus'})\n"
        "local t={io.open('validation.txt','w')}\n"
        "t[1]:write((not ttl_ok and not codec_ok) and 'true\\n' or 'false\\n'); t[1]:close()\n"
        "plugin.on('remote_stream_needed', function(provider, id, request_id)\n"
        "  if id=='error' then plugin.provide_remote_stream(request_id,{error='resolver says unavailable'})\n"
        "  else local f=io.open('request_id.txt','w'); f:write(tostring(request_id)); f:close() end\n"
        "end)\n"
        "plugin.queue_remote_list(tracks,1)\n"
        "local good_events=0\n"
        "plugin.on('track_started', function(title,artist,album,duration,provider,id)\n"
        "  if provider=='test' and id=='good' then\n"
        "    good_events=good_events+1\n"
        "    local f=io.open('request_id.txt','r'); local rid=tonumber(f:read('*a')); f:close()\n"
        "    if good_events==1 then\n"
        "      local ok=plugin.provide_remote_stream(rid,{stream_url='https://cdn.invalid/good.flac',"
        "codec='flac',sample_rate=96000,bit_depth=24,channels=2,bitrate_kbps=2500})\n"
        "      local o=io.open('owner_result.txt','w'); o:write(tostring(ok)..'\\n'); o:close()\n"
        "    else\n"
        "      plugin.play_remote({provider='replacement',track_id='new',title='Replacement'})\n"
        "      local ok=plugin.provide_remote_stream(rid,{stream_url='https://cdn.invalid/stale.mp3'})\n"
        "      local o=io.open('replacement_result.txt','w'); o:write(tostring(ok)..'\\n'); o:close()\n"
        "    end\n"
        "  end\n"
        "end)\n");
}

static void write_other_plugin(void) {
    write_file("music/.plugins/AOther.lua",
        "plugin.define({id='test.remote.other', name='Other', version='1', api_min=17})\n"
        "plugin.on('track_started', function(title,artist,album,duration,provider,id)\n"
        "  if provider=='test' and id=='good' then\n"
        "    local f=io.open('request_id.txt','r'); local rid=tonumber(f:read('*a')); f:close()\n"
        "    local ok=plugin.provide_remote_stream(rid,{stream_url='https://evil.invalid/steal.mp3'})\n"
        "    local o=io.open('wrong_owner_result.txt','w'); o:write(tostring(ok)..'\\n'); o:close()\n"
        "  end\n"
        "end)\n");
}

int main(void) {
    lv_init();
    assert(mkdir("music", 0700) == 0);
    assert(mkdir("music/.plugins", 0700) == 0);

    write_legacy_plugin();
    plugin_manager_init();
    assert(captured_count == 1 && captured_start == 0);
    assert(strcmp(captured_first.provider, "legacy") == 0);
    assert(strcmp(captured_first.stream_url, "https://legacy.invalid/live.mp3") == 0);
    assert(!captured_first.resolve_stream);
    plugin_manager_deinit();
    remove("music/.plugins/Legacy.lua");
    assert(remote_track_meta_set_all(NULL, 0));

    write_other_plugin();
    write_owner_plugin();
    plugin_manager_init();
    assert(captured_count == 500 && captured_start == 0);
    assert(strcmp(captured_first.provider, "test") == 0);
    assert(strcmp(captured_first.track_id, "good") == 0);
    assert(captured_first.resolve_stream && captured_first.resolver_owner != 0);
    assert(captured_first.stream_url[0] == '\0');
    assert(captured_first.duration_ms == 123000 && captured_first.has_replaygain &&
           captured_first.replaygain_db == -4.5);
    assert(read_bool("validation.txt"));

    const char *good_path = "remote://test/good";
    assert(remote_track_stream_prefetch(good_path, test_generation));
    plugin_manager_poll(); /* dispatch owner resolver; it deliberately holds its response */
    assert(read_id("request_id.txt") > 0);
    plugin_manager_notify_track_started("Stable title", "Stable artist", "Stable album", 123.0,
                                        "test", "good");
    FILE *f = fopen("wrong_owner_result.txt", "r");
    assert(f);
    char result[16] = {0};
    assert(fgets(result, sizeof(result), f)); fclose(f);
    assert(strcmp(result, "false\n") == 0);
    assert(read_bool("owner_result.txt"));
    remote_track_meta_t updated;
    assert(remote_track_meta_copy_for_path(good_path, &updated));
    assert(strcmp(updated.stream_url, "https://cdn.invalid/good.flac") == 0);
    assert(strcmp(updated.codec, "flac") == 0 && updated.sample_rate == 96000 &&
           updated.bit_depth == 24 && updated.channels == 2 && updated.bitrate_kbps == 2500);
    assert(strcmp(updated.title, "Stable title") == 0 &&
           strcmp(updated.artist, "Stable artist") == 0 && strcmp(updated.album, "Stable album") == 0 &&
           updated.duration_ms == 123000 && updated.replaygain_db == -4.5);

    const char *error_path = "remote://test/error";
    assert(remote_track_stream_prefetch(error_path, test_generation));
    plugin_manager_poll();
    char error[128] = {0};
    assert(remote_track_stream_error_for_path(error_path, test_generation, error, sizeof(error)));
    assert(strcmp(error, "resolver says unavailable") == 0);

    /* Invalidate a genuinely pending answer by replacing the queue. */
    const char *replacement_target = "remote://test/4";
    assert(remote_track_stream_prefetch(replacement_target, test_generation));
    plugin_manager_poll();
    uint64_t pending_replacement_id = read_id("request_id.txt");
    assert(pending_replacement_id > 0);
    plugin_manager_notify_track_started("Stable title", "Stable artist", "Stable album", 123.0,
                                        "test", "good");
    f = fopen("replacement_result.txt", "r");
    assert(f && fgets(result, sizeof(result), f)); fclose(f);
    assert(strcmp(result, "false\n") == 0);

    /* Capture an outstanding native request, unload its owner, then prove the
     * old owner/id pair can no longer answer it before a new owner is loaded. */
    remote_stream_request_t pending_unload;
    const char *replacement_path = "remote://replacement/new";
    assert(remote_track_stream_prefetch(replacement_path, test_generation));
    assert(remote_track_stream_take_request(&pending_unload));
    assert(pending_unload.request_id > 0 && pending_unload.owner != 0);
    plugin_manager_deinit();
    remote_track_meta_t late_answer = {0};
    snprintf(late_answer.stream_url, sizeof(late_answer.stream_url), "https://cdn.invalid/late.mp3");
    assert(!remote_track_stream_provide(pending_unload.owner, pending_unload.request_id,
                                        &late_answer, NULL));
    plugin_manager_init();
    assert(captured_count == 500);
    plugin_manager_deinit();

    puts("Remote stream plugin-manager integration: PASS");
    return 0;
}
