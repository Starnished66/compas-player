/* Player layout registry and the XML loader behind it. See player_layouts.h
 * for the model and docs/reference/PLAYER_LAYOUTS.md for the widget-name contract. */
#include "player_layouts.h"
#include "../../../core/system/runtime/board_config.h"
#include "../../text/rendering/fallback_font.h"
#include "../../assets/assets.h"
#include "../../../core/persistence/settings.h"
#include "../../appearance/gui_theme.h"
#include "../../widgets/builders/screen_builders.h"

#include "../../../../third_party/lv_xml/src/others/xml/lv_xml.h"
#include "../../../../third_party/lv_xml/src/others/xml/lv_xml_compat.h"
#include "lvgl/src/misc/lv_anim_timeline_private.h"

#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

#ifdef HOST_BUILD
  #define PLUGIN_LAYOUT_DIR "./music/.plugins/player_layouts"
#else
  #define PLUGIN_LAYOUT_DIR "/data/mnt/sd_0/.plugins/player_layouts"
#endif

#define MAX_LAYOUTS 64
#define MAX_LAYOUT_PATH 512
#define MAX_XML_BYTES (256 * 1024)
#define XML_CACHE_ENTRIES 2
/* The one XML component name in use. Only one XML layout exists at a time:
 * the previous one is unregistered before the next is registered. */
#define XML_COMPONENT_NAME "compas_player_layout"

typedef struct {
    player_layout_info_t info;
    player_layout_create_fn create;
    player_layout_timeline_fn timeline;
    char path[MAX_LAYOUT_PATH];
    bool session;       /* registered by a plugin for this session */
    bool from_callback; /* ... from a callback rather than while loading, so it outlives plugin resets */
    char plugin_owner[PLAYER_LAYOUT_ACTION_MAX];
} layout_entry_t;

static layout_entry_t entries[MAX_LAYOUTS];
static int entry_count;
static char session_selected[PLAYER_LAYOUT_ID_MAX];
static bool xml_engine_ready;
static bool xml_globals_ready;
static bool xml_component_registered;
static bool active_xml_layout;
typedef struct {
    char path[PATH_MAX];
    char * data;
    size_t size;
    dev_t device;
    ino_t inode;
    time_t mtime_sec;
    long mtime_nsec;
    uint64_t last_use;
} xml_cache_entry_t;
static xml_cache_entry_t xml_cache[XML_CACHE_ENTRIES];
static uint64_t xml_cache_clock;
#ifdef PLAYER_LAYOUTS_CACHE_TEST
static unsigned int xml_cache_test_disk_reads;
#endif

static void xml_cache_clear(void) {
    for (size_t i = 0; i < XML_CACHE_ENTRIES; ++i) {
        free(xml_cache[i].data);
        memset(&xml_cache[i], 0, sizeof(xml_cache[i]));
    }
}
static player_layout_interactions_t active_interactions = {
    .lyrics_enabled = true,
    .artist_action = "library",
    .album_action = "library",
};
static player_layout_action_handler_t action_handler;
static lv_obj_t * active_root;
static char active_owner[PLAYER_LAYOUT_ACTION_MAX];
typedef struct prepared_xml_timeline_t {
    struct prepared_xml_timeline_t * next;
    lv_anim_timeline_t * timeline;
    uint32_t original_delay;
    uint32_t original_repeat_delay;
    uint32_t anim_count;
    uint32_t style_anim_count;
    struct {
        player_layouts_timeline_observer_cb_t callback;
        void * user_data;
        uint32_t act_time;
        uint32_t registration_id;
        bool has_act_time;
    } observers[4];
    uint32_t observer_registration_id;
    bool notifying_observers;
    bool owned_style_timeline;
    struct { uint32_t start_time, duration; } originals[];
} prepared_xml_timeline_t;
static prepared_xml_timeline_t * prepared_xml_timelines;
#define MAX_TIMELINE_SUPPRESSIONS 64
typedef struct {
    lv_anim_timeline_t * timeline;
    lv_obj_t * obj;
    lv_style_prop_t prop;
    lv_style_selector_t selector;
} timeline_suppression_t;
static timeline_suppression_t timeline_suppressions[MAX_TIMELINE_SUPPRESSIONS];
/* Timeline hook of the layout created last, for exported layouts. */
static player_layout_timeline_fn active_timeline_fn;

extern player_settings_t current_settings;

/* The widget images every layout may name, as file stems of assets resolved
 * through asset_path() so theme overrides apply. The C builder references the
 * same files, so LVGL's image cache holds each of them once. */
typedef struct {
    const char * name;
    const char * relative_path;
} layout_image_t;

static const layout_image_t layout_images[] = {
    { "default_cover", "playing_plane/default_cover_565.png" },
    { "collect_out", "playing_plane/collect_out.png" },
    { "collect_in", "playing_plane/collect_in.png" },
    { "quality_waveform", "playing_plane/quality_waveform.png" },
    { "btn_prev", "playing_plane/btn_prev.png" },
    { "btn_prev_s", "playing_plane/btn_prev_s.png" },
    { "btn_next", "playing_plane/btn_next.png" },
    { "btn_next_s", "playing_plane/btn_next_s.png" },
    { "btn_play", "playing_plane/btn_play.png" },
    { "btn_pause", "playing_plane/btn_pause.png" },
    { "ic_more", "playing_plane/ic_more.png" },
    { "order", "playing_plane/order.png" },
    { "loop", "playing_plane/loop.png" },
    { "single", "playing_plane/single.png" },
    { "random", "playing_plane/random.png" },
    { "btn_back", "sub_back/btn_back.png" },
    /* Pressed variants and the rest of the stock Now Playing artwork. Files
     * missing from a theme just draw nothing. */
    { "btn_play_s", "playing_plane/btn_play_s.png" },
    { "btn_pause_s", "playing_plane/btn_pause_s.png" },
    { "ic_more_s", "playing_plane/ic_more_s.png" },
    { "collect_out_s", "playing_plane/collect_out_s.png" },
    { "order_s", "playing_plane/order_s.png" },
    { "loop_s", "playing_plane/loop_s.png" },
    { "single_s", "playing_plane/single_s.png" },
    { "random_s", "playing_plane/random_s.png" },
    { "topbar_bg", "playing_plane/topbar_bg.png" },
    { "bottom_panel", "playing_plane/buttom.png" }, /* sic: the stock file name is misspelled */
    { "progress", "playing_plane/progress.png" },
    { "progress_bg", "playing_plane/progress_bg.png" },
    { "speed", "playing_plane/speed.png" },
    { "speed_s", "playing_plane/speed_s.png" },
    { "dlna", "playing_plane/dlna.png" },
};

#define LAYOUT_FOLDER_IMAGES_MAX 32
#define LAYOUT_FOLDER_IMAGE_NAME_MAX 64

static bool id_is_valid(const char * id) {
    if (!id || !id[0] || strlen(id) >= PLAYER_LAYOUT_ID_MAX) return false;
    for (const char * p = id; *p; ++p) {
        if (!isalnum((unsigned char) *p) && *p != '_' && *p != '.' && *p != '-') return false;
    }
    return strcmp(id, PLAYER_LAYOUT_ID_DEFAULT) != 0;
}

bool player_layouts_id_is_valid(const char * id) {
    return id_is_valid(id);
}

static void ensure_builtin_entry(void) {
    if (entry_count > 0) return;
    layout_entry_t * e = &entries[entry_count++];
    memset(e, 0, sizeof(*e));
    snprintf(e->info.id, sizeof(e->info.id), "%s", PLAYER_LAYOUT_ID_DEFAULT);
    snprintf(e->info.name, sizeof(e->info.name), "Default");
    e->info.kind = PLAYER_LAYOUT_BUILTIN_C;
}

int player_layouts_count(void) {
    ensure_builtin_entry();
    return entry_count;
}

const player_layout_info_t * player_layouts_get(int index) {
    ensure_builtin_entry();
    return index >= 0 && index < entry_count ? &entries[index].info : NULL;
}

static bool layout_preview_variant_suffix(const char * suffix) {
    if (!suffix || (*suffix != '@' && *suffix != '_')) return false;
    const char * p = suffix + 1;
    const char * digits = p;
    while (*p >= '0' && *p <= '9') p++;
    if (p == digits || *p++ != 'x') return false;
    digits = p;
    while (*p >= '0' && *p <= '9') p++;
    return p != digits && *p == '\0';
}

static bool layout_preview_png_valid(const char * path) {
    static const unsigned char signature[8] = { 0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a };
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    unsigned char header[24];
    size_t used = 0;
    bool valid = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                 st.st_size >= (off_t) sizeof(header) && st.st_size <= 512 * 1024;
    while (valid && used < sizeof(header)) {
        ssize_t n = read(fd, header + used, sizeof(header) - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { valid = false; break; }
        used += (size_t) n;
    }
    close(fd);
    if (!valid || used != sizeof(header) || memcmp(header, signature, sizeof(signature)) != 0 ||
        header[8] != 0 || header[9] != 0 || header[10] != 0 || header[11] != 13 ||
        memcmp(header + 12, "IHDR", 4) != 0) return false;
    uint32_t width = ((uint32_t)header[16] << 24) | ((uint32_t)header[17] << 16) |
                     ((uint32_t)header[18] << 8) | header[19];
    uint32_t height = ((uint32_t)header[20] << 24) | ((uint32_t)header[21] << 16) |
                      ((uint32_t)header[22] << 8) | header[23];
    return width > 0 && width <= 240 && height > 0 && height <= 400;
}

bool player_layouts_get_preview(int index, char * out, size_t size) {
    ensure_builtin_entry();
    if (!out || !size) return false;
    out[0] = '\0';
    if (index < 0 || index >= entry_count) return false;
    const layout_entry_t * entry = &entries[index];
    char path[MAX_LAYOUT_PATH];
    if (entry->info.kind != PLAYER_LAYOUT_XML_FILE) {
        char relative[PLAYER_LAYOUT_ID_MAX + 32];
        int n = snprintf(relative, sizeof(relative), "player_layouts/%s.png", entry->info.id);
        if (n <= 0 || n >= (int)sizeof(relative)) return false;
        const char * resolved = asset_path_plain(relative);
        if (!resolved || snprintf(path, sizeof(path), "%s", resolved) >= (int)sizeof(path)) return false;
    } else {
        const char * slash = strrchr(entry->path, '/');
        const char * leaf = slash ? slash + 1 : entry->path;
        size_t leaf_len = strlen(leaf);
        if (leaf_len <= 4 || strcasecmp(leaf + leaf_len - 4, ".xml") != 0) return false;
        size_t stem_len = leaf_len - 4;
        char stem[PLAYER_LAYOUT_ID_MAX + 32];
        if (stem_len >= sizeof(stem)) return false;
        memcpy(stem, leaf, stem_len); stem[stem_len] = '\0';
        char * sep = strrchr(stem, '@');
        char * under = strrchr(stem, '_');
        if (!sep || (under && under > sep)) sep = under;
        if (sep && layout_preview_variant_suffix(sep)) *sep = '\0';
        int n = slash ? snprintf(path, sizeof(path), "%.*s/%s.png", (int)(slash - entry->path),
                                  entry->path, stem) : snprintf(path, sizeof(path), "%s.png", stem);
        if (n <= 0 || n >= (int)sizeof(path)) return false;

        /* The sidecar must live beside the resolved XML file. This also
         * rejects a sibling path that escapes through a symlinked file. */
        char xml_real[PATH_MAX], png_real[PATH_MAX];
        if (!realpath(entry->path, xml_real) || !realpath(path, png_real)) return false;
        char * xml_slash = strrchr(xml_real, '/');
        char * png_slash = strrchr(png_real, '/');
        if (!xml_slash || !png_slash || (size_t)(xml_slash - xml_real) != (size_t)(png_slash - png_real) ||
            strncmp(xml_real, png_real, (size_t)(xml_slash - xml_real)) != 0) return false;
    }
    if (!layout_preview_png_valid(path)) return false;
    int n = snprintf(out, size, "%s", path);
    if (n <= 0 || (size_t)n >= size) { out[0] = '\0'; return false; }
    return true;
}

int player_layouts_find(const char * id) {
    ensure_builtin_entry();
    if (!id) return -1;
    for (int i = 0; i < entry_count; ++i) {
        if (strcmp(entries[i].info.id, id) == 0) return i;
    }
    return -1;
}

static bool path_is_within_root(const char * path, const char * root) {
    size_t root_len = strlen(root);
    return strncmp(path, root, root_len) == 0 && path[root_len] == '/';
}

bool player_layouts_canonical_id(const char * id, char * out, size_t size) {
    if (!out || !size) return false;
    out[0] = '\0';
    if (!id || (strcmp(id, PLAYER_LAYOUT_ID_DEFAULT) != 0 && !id_is_valid(id))) return false;

    ensure_builtin_entry();
    int index = player_layouts_find(id);
    if (index < 0) return false;
    const layout_entry_t * entry = &entries[index];
    const char * canonical_id = entry->info.id;

    if (entry->info.kind == PLAYER_LAYOUT_XML_FILE && entry->path[0]) {
        char plugin_root[MAX_LAYOUT_PATH];
        char real_root[PATH_MAX], real_file[PATH_MAX];
#ifdef HOST_BUILD
        snprintf(plugin_root, sizeof(plugin_root), "./music/.plugins");
#else
        snprintf(plugin_root, sizeof(plugin_root), "/data/mnt/sd_0/.plugins");
#endif
        if (realpath(plugin_root, real_root) && realpath(entry->path, real_file) &&
            path_is_within_root(real_file, real_root)) {
            const char * session_alias = NULL;
            const char * selected_alias = NULL;
            for (int i = 1; i < entry_count; ++i) {
                const layout_entry_t * candidate = &entries[i];
                if (!candidate->session || candidate->info.kind != PLAYER_LAYOUT_XML_FILE ||
                    !candidate->path[0] || !id_is_valid(candidate->info.id)) continue;
                char candidate_real[PATH_MAX];
                if (!realpath(candidate->path, candidate_real) ||
                    !path_is_within_root(candidate_real, real_root) || strcmp(candidate_real, real_file) != 0)
                    continue;
                if (strcmp(candidate->info.id, session_selected) == 0) {
                    selected_alias = candidate->info.id;
                    break;
                }
                if (!session_alias) session_alias = candidate->info.id;
            }
            if (selected_alias) canonical_id = selected_alias;
            else if (session_alias) canonical_id = session_alias;
        }
    }

    int written = snprintf(out, size, "%s", canonical_id);
    if (written < 0 || (size_t) written >= size) {
        out[0] = '\0';
        return false;
    }
    return true;
}

static void remove_entry(int index) {
    memmove(&entries[index], &entries[index + 1], (size_t) (entry_count - index - 1) * sizeof(entries[0]));
    --entry_count;
}

/* Appends or replaces in place, keeping table order stable for the Settings
 * list. Returns NULL when the table is full. */
static layout_entry_t * upsert_entry(const char * id, const char * name, player_layout_kind_t kind) {
    int index = player_layouts_find(id);
    layout_entry_t * e;
    if (index >= 0) {
        e = &entries[index];
    } else {
        if (entry_count >= MAX_LAYOUTS) return NULL;
        e = &entries[entry_count++];
    }
    memset(e, 0, sizeof(*e));
    snprintf(e->info.id, sizeof(e->info.id), "%s", id);
    snprintf(e->info.name, sizeof(e->info.name), "%s", name && name[0] ? name : id);
    e->info.kind = kind;
    return e;
}

static void reset_interactions(void) {
    active_interactions = (player_layout_interactions_t){
        .lyrics_enabled = true,
        .artist_action = "library",
        .album_action = "library",
    };
}

static void invalidate_active_layout(void) {
    active_root = NULL;
    active_owner[0] = '\0';
    reset_interactions();
}

static bool action_name_is_valid(const char * action, bool allow_builtin) {
    if (!action || !action[0] || strlen(action) >= PLAYER_LAYOUT_ACTION_MAX) return false;
    if (allow_builtin && (strcmp(action, "library") == 0 || strcmp(action, "none") == 0)) return true;
    if (!allow_builtin && (strcmp(action, "library") == 0 || strcmp(action, "none") == 0)) return false;
    for (const char * p = action; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    }
    return true;
}

const player_layout_interactions_t * player_layouts_get_interactions(void) {
    return &active_interactions;
}

void player_layouts_set_action_handler(player_layout_action_handler_t handler) {
    action_handler = handler;
}

bool player_layouts_set_plugin_owner(const char * layout_id, const char * owner) {
    if (!layout_id || !owner || !id_is_valid(layout_id) || !owner[0] ||
        strlen(owner) >= PLAYER_LAYOUT_ACTION_MAX) return false;
    for (const char * p = owner; *p; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) return false;
    }
    int index = player_layouts_find(layout_id);
    if (index < 0 || !entries[index].session || entries[index].info.kind != PLAYER_LAYOUT_XML_FILE) return false;
    snprintf(entries[index].plugin_owner, sizeof(entries[index].plugin_owner), "%s", owner);
    return true;
}

void player_layouts_clear_plugin_owner(const char * owner) {
    if (!owner || !owner[0]) return;
    bool active_match = strcmp(active_owner, owner) == 0;
    for (int i = 0; i < entry_count; ++i) {
        if (strcmp(entries[i].plugin_owner, owner) == 0) entries[i].plugin_owner[0] = '\0';
    }
    if (active_match) active_owner[0] = '\0';
}

bool player_layouts_dispatch_action(const char * action) {
    return player_layouts_dispatch_action_from(active_root, action);
}

bool player_layouts_dispatch_action_from(lv_obj_t * target, const char * action) {
    if (!active_root || !target || !active_owner[0] || !action_handler ||
        !action_name_is_valid(action, false)) return false;
    bool belongs_to_active = false;
    for (lv_obj_t * obj = target; obj; obj = lv_obj_get_parent(obj)) {
        if (obj == active_root) {
            belongs_to_active = true;
            break;
        }
    }
    if (!belongs_to_active) return false;
    action_handler(active_owner, action);
    return true;
}

static void layout_action_event_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    lv_obj_t * target = lv_event_get_target(e);
    const char * action = lv_event_get_user_data(e);
    if (target && action) player_layouts_dispatch_action_from(target, action);
}

static void active_root_delete_cb(lv_event_t * e) {
    if (lv_event_get_target(e) == active_root) invalidate_active_layout();
}

bool player_layouts_register_c_ex(const char * id, const char * display_name, player_layout_create_fn create,
                                  player_layout_timeline_fn timeline) {
    ensure_builtin_entry();
    if (!id_is_valid(id) || !create) return false;
    int existing = player_layouts_find(id);
    if (existing >= 0 && entries[existing].info.kind != PLAYER_LAYOUT_EXPORTED_C) return false;
    layout_entry_t * e = upsert_entry(id, display_name, PLAYER_LAYOUT_EXPORTED_C);
    if (!e) return false;
    e->create = create;
    e->timeline = timeline;
    return true;
}

bool player_layouts_register_c(const char * id, const char * display_name, player_layout_create_fn create) {
    return player_layouts_register_c_ex(id, display_name, create, NULL);
}

static bool same_id(const char * a, const char * b) {
    return strcmp(a ? a : "", b ? b : "") == 0;
}

const char * player_layouts_effective_id(void) {
    ensure_builtin_entry();
    if (session_selected[0] && player_layouts_find(session_selected) >= 0) return session_selected;
    if (current_settings.player_layout[0] && player_layouts_find(current_settings.player_layout) >= 0)
        return current_settings.player_layout;
    return PLAYER_LAYOUT_ID_DEFAULT;
}

bool player_layouts_session_register_xml(const char * id, const char * display_name, const char * path,
                                         bool from_callback) {
    ensure_builtin_entry();
    if (!id_is_valid(id) || !path || !path[0] || strlen(path) >= MAX_LAYOUT_PATH) return false;
    int existing = player_layouts_find(id);
    if (existing >= 0 && !entries[existing].session && entries[existing].info.kind != PLAYER_LAYOUT_XML_FILE) return false;
    /* A runtime registration of a discovered file upgrades that entry in
     * place. Also discard other automatically discovered aliases of the same
     * canonical XML, except IDs selected in Settings or for this session. */
    char registered_real[PATH_MAX];
    if (realpath(path, registered_real)) {
        for (int i = entry_count - 1; i >= 1; --i) {
            if (i == existing || entries[i].info.kind != PLAYER_LAYOUT_XML_FILE || entries[i].session ||
                !entries[i].path[0] || strcmp(entries[i].info.id, current_settings.player_layout) == 0 ||
                strcmp(entries[i].info.id, session_selected) == 0) continue;
            char discovered_real[PATH_MAX];
            if (realpath(entries[i].path, discovered_real) && strcmp(registered_real, discovered_real) == 0)
                remove_entry(i);
        }
    }
    existing = player_layouts_find(id);
    /* Replaces an entry of the same id in place: a reload registers it again. */
    layout_entry_t * e = upsert_entry(id, display_name, PLAYER_LAYOUT_XML_FILE);
    if (!e) return false;
    e->session = true;
    e->from_callback = from_callback;
    snprintf(e->path, sizeof(e->path), "%s", path);
    return true;
}

bool player_layouts_session_select_xml(const char * id, const char * display_name, const char * path,
                                       bool from_callback) {
    char before[PLAYER_LAYOUT_ID_MAX];
    snprintf(before, sizeof(before), "%s", player_layouts_effective_id());
    if (!player_layouts_session_register_xml(id, display_name, path, from_callback)) return false;
    snprintf(session_selected, sizeof(session_selected), "%s", id);
    return !same_id(before, player_layouts_effective_id());
}

void player_layouts_session_reset(void) {
    invalidate_active_layout();
    for (int i = entry_count - 1; i >= 1; --i) {
        if (entries[i].session && !entries[i].from_callback) remove_entry(i);
    }
    if (session_selected[0] && player_layouts_find(session_selected) < 0) session_selected[0] = '\0';
}

bool player_layouts_session_clear_selection(void) {
    if (!session_selected[0]) return false;
    char before[PLAYER_LAYOUT_ID_MAX];
    snprintf(before, sizeof(before), "%s", player_layouts_effective_id());
    session_selected[0] = '\0';
    return !same_id(before, player_layouts_effective_id());
}

bool player_layouts_session_clear_plugin_selection(const char * owner) {
    if (!owner || !owner[0] || !session_selected[0]) return false;
    int index = player_layouts_find(session_selected);
    if (index < 0 || !entries[index].plugin_owner[0] || strcmp(entries[index].plugin_owner, owner) != 0)
        return false;
    return player_layouts_session_clear_selection();
}

/* ---- XML file discovery ---- */

typedef struct {
    char id[PLAYER_LAYOUT_ID_MAX];
    char name[PLAYER_LAYOUT_NAME_MAX];
    char path[MAX_LAYOUT_PATH];
} found_layout_t;

static int found_cmp(const void * a, const void * b) {
    return strcmp(((const found_layout_t *) a)->id, ((const found_layout_t *) b)->id);
}

/* `foo@480x800.xml` is a board variant of `foo.xml`, never an entry itself. */
static bool file_is_board_variant(const char * name) {
    const char * at = strrchr(name, '@');
    if (at) return true;
    size_t len = strlen(name);
    if (len < 8 || strcasecmp(name + len - 4, ".xml") != 0) return false;
    const char * end = name + len - 4;
    const char * p = end;
    while (p > name && isdigit((unsigned char) p[-1])) --p;
    if (p == end || p == name || p[-1] != 'x') return false;
    const char * x = p - 1;
    p = x;
    while (p > name && isdigit((unsigned char) p[-1])) --p;
    if (p == x || p == name || p[-1] != '_') return false;
    return true;
}

static void scan_directory(const char * dir, const char * id_prefix, const char * display_prefix,
                           found_layout_t * found, int * found_count, int max_found,
                           const char * containment_root, bool skip_developer_examples) {
    if (!dir || !dir[0]) return;
    DIR * d = opendir(dir);
    if (!d) return;
    struct dirent * ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".xml") != 0) continue;
        if (ent->d_name[0] == '.' || file_is_board_variant(ent->d_name)) continue;
        char stem[PLAYER_LAYOUT_ID_MAX], id[PLAYER_LAYOUT_ID_MAX];
        if (len - 4 >= sizeof(stem)) continue;
        memcpy(stem, ent->d_name, len - 4);
        stem[len - 4] = '\0';
        /* Developer examples are source-tree fixtures, not shipped layouts.
         * Keep this limited to the native stock/override directories so a
         * plugin or user-provided layout can still use the same filename. */
        if (skip_developer_examples && strncmp(stem, "example_", 8) == 0) continue;
        int id_len = id_prefix ? snprintf(id, sizeof(id), "%s%s", id_prefix, stem) :
                                  snprintf(id, sizeof(id), "%s", stem);
        if (id_len < 0 || id_len >= (int) sizeof(id)) continue;
        if (!id_is_valid(id)) continue;
        char path[MAX_LAYOUT_PATH];
        if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int) sizeof(path)) continue;
        struct stat st;
        if (containment_root) {
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        } else if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (containment_root) {
            char real_root[PATH_MAX], real_file[PATH_MAX];
            if (!realpath(containment_root, real_root) || !realpath(path, real_file)) continue;
            size_t root_len = strlen(real_root);
            if (strncmp(real_file, real_root, root_len) != 0 || real_file[root_len] != '/') continue;
            if (strlen(real_file) >= sizeof(path)) continue;
            snprintf(path, sizeof(path), "%s", real_file);
        }

        /* A later directory replaces an earlier file of the same id. */
        int slot = -1;
        for (int i = 0; i < *found_count; ++i) {
            if (strcmp(found[i].id, id) == 0) { slot = i; break; }
        }
        if (slot < 0) {
            if (*found_count >= max_found) {
                /* Keep the lexically first IDs if discovery exceeds its
                 * bounded scratch table, independent of readdir order. */
                slot = 0;
                for (int i = 1; i < *found_count; ++i)
                    if (strcmp(found[i].id, found[slot].id) > 0) slot = i;
                if (strcmp(id, found[slot].id) >= 0) continue;
            } else slot = (*found_count)++;
        }
        snprintf(found[slot].id, sizeof(found[slot].id), "%s", id);
        snprintf(found[slot].name, sizeof(found[slot].name), "%s%s", display_prefix ? display_prefix : "", stem);
        if (id_prefix)
            for (char * p = found[slot].name; *p; ++p) if (*p == '_') *p = ' ';
        snprintf(found[slot].path, sizeof(found[slot].path), "%s", path);
    }
    closedir(d);
}

static void join_layout_dir(char * out, size_t size, const char * root) {
    snprintf(out, size, "%splayer_layouts", root);
}

/* Directories, later wins: the stock asset root (read-only on device, the
 * repo's assets/theme2 on host), the writable asset override root, and the
 * plugins folder on the SD card. */
static void rescan_xml_layouts(void) {
    for (int i = entry_count - 1; i >= 1; --i) {
        if (entries[i].info.kind == PLAYER_LAYOUT_XML_FILE && !entries[i].session) remove_entry(i);
    }

    found_layout_t found[MAX_LAYOUTS];
    int found_count = 0;
    char dir[MAX_LAYOUT_PATH];
    join_layout_dir(dir, sizeof(dir), asset_stock_root());
    scan_directory(dir, NULL, NULL, found, &found_count, MAX_LAYOUTS, NULL, true);
    if (asset_override_root()) {
        join_layout_dir(dir, sizeof(dir), asset_override_root());
        scan_directory(dir, NULL, NULL, found, &found_count, MAX_LAYOUTS, NULL, true);
    }
    /* Legacy plugin layouts retain their historical bare stem IDs/priority. */
    scan_directory(PLUGIN_LAYOUT_DIR, NULL, NULL, found, &found_count, MAX_LAYOUTS, NULL, false);
    char plugin_root[MAX_LAYOUT_PATH];
#ifdef HOST_BUILD
    snprintf(plugin_root, sizeof(plugin_root), "./music/.plugins");
#else
    snprintf(plugin_root, sizeof(plugin_root), "/data/mnt/sd_0/.plugins");
#endif
    char root_real[PATH_MAX];
    if (realpath(plugin_root, root_real)) {
        scan_directory(root_real, "plugin.", "", found, &found_count, MAX_LAYOUTS, root_real, false);
        DIR * pd = opendir(root_real);
        if (pd) {
            struct dirent * pe;
            while ((pe = readdir(pd)) != NULL) {
                if (pe->d_name[0] == '.') continue;
                char bundle[MAX_LAYOUT_PATH], prefix[PLAYER_LAYOUT_ID_MAX];
                int bn = snprintf(bundle, sizeof(bundle), "%s/%s", root_real, pe->d_name);
                struct stat st;
                if (bn <= 0 || bn >= (int) sizeof(bundle) || lstat(bundle, &st) != 0 ||
                    !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
                    strcmp(pe->d_name, "player_layouts") == 0) continue;
                char layout_dir[MAX_LAYOUT_PATH];
                int ln = snprintf(layout_dir, sizeof(layout_dir), "%s/player_layouts", bundle);
                if (ln <= 0 || ln >= (int) sizeof(layout_dir)) continue;
                struct stat layout_st;
                if (lstat(layout_dir, &layout_st) != 0 || !S_ISDIR(layout_st.st_mode) || S_ISLNK(layout_st.st_mode)) continue;
                int pn = snprintf(prefix, sizeof(prefix), "plugin.%s.", pe->d_name);
                if (pn <= 0 || pn >= (int) sizeof(prefix)) continue;
                scan_directory(layout_dir, prefix, "", found, &found_count, MAX_LAYOUTS, root_real, false);
            }
            closedir(pd);
        }
    }
    qsort(found, (size_t) found_count, sizeof(found[0]), found_cmp);

    for (int i = 0; i < found_count; ++i) {
        /* Never shadow a C layout or a plugin's session layout of the same id. */
        if (player_layouts_find(found[i].id) >= 0) continue;
        bool duplicate = false;
        for (int j = 1; j < entry_count; ++j) {
            if (entries[j].session && entries[j].path[0]) {
                char a[PATH_MAX], b[PATH_MAX];
                if (realpath(entries[j].path, a) && realpath(found[i].path, b) && strcmp(a, b) == 0) duplicate = true;
            }
        }
        /* A selected discovery ID remains a valid saved/session choice even
         * when a plugin also registers the same file under a historical ID.
         * Keep that alias so the selection survives the next rescan. */
        bool selected_alias = strcmp(found[i].id, current_settings.player_layout) == 0 ||
                              strcmp(found[i].id, session_selected) == 0;
        if (duplicate && !selected_alias) continue;
        layout_entry_t * e = upsert_entry(found[i].id, found[i].name, PLAYER_LAYOUT_XML_FILE);
        if (!e) break;
        snprintf(e->path, sizeof(e->path), "%s", found[i].path);
    }
}

void player_layouts_rescan(void) {
    xml_cache_clear();
    ensure_builtin_entry();
    rescan_xml_layouts();
}

void player_layouts_init(player_layout_create_fn builtin_create) {
    xml_cache_clear();
    ensure_builtin_entry();
    entries[0].create = builtin_create;
    rescan_xml_layouts();
}

/* ---- XML creation ---- */

static bool xml_stat_matches(const xml_cache_entry_t * entry, const char * key,
                             const struct stat * st) {
    return entry->data && strcmp(entry->path, key) == 0 &&
           entry->device == st->st_dev && entry->inode == st->st_ino &&
           entry->size == (size_t) st->st_size && entry->mtime_sec == st->st_mtime &&
           entry->mtime_nsec == st->st_mtim.tv_nsec;
}

static char * read_text_file(const char * path) {
    struct stat before;
    if (stat(path, &before) != 0 || !S_ISREG(before.st_mode) || before.st_size <= 0 ||
        before.st_size > MAX_XML_BYTES) return NULL;
    char resolved[PATH_MAX];
    const char * key = realpath(path, resolved) ? resolved : NULL;
    for (size_t i = 0; key && i < XML_CACHE_ENTRIES; ++i) {
        if (xml_stat_matches(&xml_cache[i], key, &before)) {
            char * copy = malloc(xml_cache[i].size + 1);
            if (!copy) return NULL;
            memcpy(copy, xml_cache[i].data, xml_cache[i].size + 1);
            xml_cache[i].last_use = ++xml_cache_clock;
            return copy;
        }
    }
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
#ifdef PLAYER_LAYOUTS_CACHE_TEST
    xml_cache_test_disk_reads++;
#endif
    char * buffer = NULL;
    size_t size = (size_t) before.st_size;
    buffer = malloc(size + 1);
    if (buffer && fread(buffer, 1, size, f) == size) {
        buffer[size] = '\0';
    } else {
        free(buffer);
        buffer = NULL;
    }
    fclose(f);
    struct stat after;
    if (!buffer || stat(path, &after) != 0 || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size ||
        before.st_mtime != after.st_mtime || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec) {
        free(buffer);
        return NULL;
    }

    /* Keep at most two raw files (each bounded by MAX_XML_BYTES). The returned
     * allocation remains caller-owned; LVGL component state is never cached. */
    char * cached = key ? malloc(size + 1) : NULL;
    if (cached) {
        memcpy(cached, buffer, size + 1);
        size_t victim = 0;
        for (size_t i = 1; i < XML_CACHE_ENTRIES; ++i)
            if (!xml_cache[i].data || (xml_cache[victim].data &&
                                      xml_cache[i].last_use < xml_cache[victim].last_use)) victim = i;
        free(xml_cache[victim].data);
        memset(&xml_cache[victim], 0, sizeof(xml_cache[victim]));
        memcpy(xml_cache[victim].path, key, strlen(key) + 1);
        xml_cache[victim].data = cached;
        xml_cache[victim].size = size;
        xml_cache[victim].device = before.st_dev;
        xml_cache[victim].inode = before.st_ino;
        xml_cache[victim].mtime_sec = before.st_mtime;
        xml_cache[victim].mtime_nsec = before.st_mtim.tv_nsec;
        xml_cache[victim].last_use = ++xml_cache_clock;
    }
    return buffer;
}

#ifdef PLAYER_LAYOUTS_CACHE_TEST
char * player_layouts_cache_test_read(const char * path) {
    return read_text_file(path);
}

unsigned int player_layouts_cache_test_disk_reads(void) {
    return xml_cache_test_disk_reads;
}

size_t player_layouts_cache_test_entry_count(void) {
    size_t count = 0;
    for (size_t i = 0; i < XML_CACHE_ENTRIES; ++i)
        if (xml_cache[i].data) count++;
    return count;
}

void player_layouts_cache_test_clear(void) {
    xml_cache_clear();
}
#endif

/* `foo.xml` -> `foo@<W>x<H>.xml` if that file exists. */
static void resolve_board_variant(const char * path, char * out, size_t size) {
    snprintf(out, size, "%s", path);
    const char * dot = strrchr(path, '.');
    if (!dot) return;
    char variant[MAX_LAYOUT_PATH];
    int n = snprintf(variant, sizeof(variant), "%.*s@%dx%d%s", (int) (dot - path), path,
                     BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT, dot);
    if (n <= 0 || n >= (int) sizeof(variant)) return;
    struct stat st;
    if (stat(variant, &st) == 0 && S_ISREG(st.st_mode)) snprintf(out, size, "%s", variant);
    else {
        n = snprintf(variant, sizeof(variant), "%.*s_%dx%d%s", (int) (dot - path), path,
                     BOARD_SCREEN_WIDTH, BOARD_SCREEN_HEIGHT, dot);
        if (n > 0 && n < (int) sizeof(variant) && stat(variant, &st) == 0 && S_ISREG(st.st_mode))
            snprintf(out, size, "%s", variant);
    }
}

/* A discovered file ID and a plugin's session ID can point to the same XML.
 * Resolve an owner only from an explicitly owned session entry for that exact
 * canonical file; conflicting owners leave the discovered alias unowned. */
static void resolve_plugin_owner(const layout_entry_t * entry, char * out, size_t size) {
    if (!out || !size) return;
    out[0] = '\0';
    if (!entry || entry->info.kind != PLAYER_LAYOUT_XML_FILE) return;
    if (entry->plugin_owner[0]) {
        snprintf(out, size, "%s", entry->plugin_owner);
        return;
    }

    char selected_path[MAX_LAYOUT_PATH];
    char selected_real[PATH_MAX];
    resolve_board_variant(entry->path, selected_path, sizeof(selected_path));
    if (!realpath(selected_path, selected_real)) return;

    const char * owner = NULL;
    for (int i = 0; i < entry_count; ++i) {
        const layout_entry_t * candidate = &entries[i];
        if (!candidate->session || candidate->info.kind != PLAYER_LAYOUT_XML_FILE ||
            !candidate->plugin_owner[0] || !candidate->path[0]) continue;
        char candidate_path[MAX_LAYOUT_PATH];
        char candidate_real[PATH_MAX];
        resolve_board_variant(candidate->path, candidate_path, sizeof(candidate_path));
        if (!realpath(candidate_path, candidate_real) || strcmp(selected_real, candidate_real) != 0) continue;
        if (owner && strcmp(owner, candidate->plugin_owner) != 0) return;
        owner = candidate->plugin_owner;
    }
    if (owner) snprintf(out, size, "%s", owner);
}

/* Fonts and constants are global in the XML engine and first registration
 * wins, so they are registered once. The font handles are the app's stable
 * lv_font_t objects, whose contents are replaced in place on a font change,
 * and the screen size is fixed for the life of the process. */
static void register_xml_globals_once(void) {
    if (xml_globals_ready) return;
    lv_xml_register_font(NULL, "player_title", &app_font_player_title);
    lv_xml_register_font(NULL, "player_meta", &app_font_player_meta);
    lv_xml_register_font(NULL, "font_16", &app_font_16);
    lv_xml_register_font(NULL, "font_20", &app_font_20);
    lv_xml_register_font(NULL, "font_22", &app_font_22);
    lv_xml_register_font(NULL, "font_28", &app_font_28);
    char value[16];
    snprintf(value, sizeof(value), "%d", BOARD_SCREEN_WIDTH);
    lv_xml_register_const(NULL, "screen_w", value);
    snprintf(value, sizeof(value), "%d", BOARD_SCREEN_HEIGHT);
    lv_xml_register_const(NULL, "screen_h", value);
    lv_xml_register_const(NULL, "compas_lyrics_enabled", "true");
    lv_xml_register_const(NULL, "compas_artist_action", "library");
    lv_xml_register_const(NULL, "compas_album_action", "library");
    lv_xml_register_event_cb(NULL, "compas_action", layout_action_event_cb);
    xml_globals_ready = true;
}

void player_layouts_release(void) {
    invalidate_active_layout();
    prepared_xml_timeline_t * prepared = prepared_xml_timelines;
    while (prepared) {
        prepared_xml_timeline_t * next = prepared->next;
        if (prepared->owned_style_timeline && prepared->timeline)
            lv_anim_timeline_delete(prepared->timeline);
        free(prepared);
        prepared = next;
    }
    prepared_xml_timelines = NULL;
    memset(timeline_suppressions, 0, sizeof(timeline_suppressions));
    active_xml_layout = false;
    if (xml_component_registered) {
        lv_xml_component_unregister(XML_COMPONENT_NAME);
        xml_component_registered = false;
    }
}

static int name_cmp(const void * a, const void * b) {
    return strcmp((const char *) a, (const char *) b);
}

/* `<dir>/<stem>.xml` may ship artwork in `<dir>/<stem>/`: each `*.png` there
 * becomes an image named by its file stem (letters, digits, `_`, `-`), up to
 * LAYOUT_FOLDER_IMAGES_MAX in name order. Registered before the built-in
 * names, so a file here replaces a built-in of the same name; first
 * registration wins, so names the XML declares itself still take precedence. */
static void register_layout_folder_images(lv_xml_component_scope_t * scope, const char * entry_path) {
    const char * dot = strrchr(entry_path, '.');
    if (!scope || !dot) return;
    char folder[MAX_LAYOUT_PATH];
    if (snprintf(folder, sizeof(folder), "%.*s", (int) (dot - entry_path), entry_path) >= (int) sizeof(folder)) return;
    DIR * d = opendir(folder);
    if (!d) return;

    /* The LAYOUT_FOLDER_IMAGES_MAX smallest names, whatever the directory
     * order: a full table replaces its largest name with a smaller one. */
    char names[LAYOUT_FOLDER_IMAGES_MAX][LAYOUT_FOLDER_IMAGE_NAME_MAX];
    int count = 0;
    struct dirent * ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 5 || len - 4 >= LAYOUT_FOLDER_IMAGE_NAME_MAX || strcmp(ent->d_name + len - 4, ".png") != 0) continue;
        bool ok = true;
        for (size_t i = 0; i < len - 4 && ok; ++i)
            ok = isalnum((unsigned char) ent->d_name[i]) || ent->d_name[i] == '_' || ent->d_name[i] == '-';
        if (!ok) continue;
        char path[MAX_LAYOUT_PATH];
        struct stat st;
        if (snprintf(path, sizeof(path), "%s/%s", folder, ent->d_name) >= (int) sizeof(path)) continue;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        char name[LAYOUT_FOLDER_IMAGE_NAME_MAX];
        memcpy(name, ent->d_name, len - 4);
        name[len - 4] = '\0';
        int slot = count;
        if (count == LAYOUT_FOLDER_IMAGES_MAX) {
            slot = 0;
            for (int i = 1; i < count; ++i)
                if (strcmp(names[i], names[slot]) > 0) slot = i;
            if (strcmp(name, names[slot]) >= 0) continue;
        } else {
            ++count;
        }
        memcpy(names[slot], name, len - 3);
    }
    closedir(d);
    qsort(names, (size_t) count, sizeof(names[0]), name_cmp);
    for (int i = 0; i < count; ++i) {
        /* The XML engine keeps image paths in LV_XML_MAX_PATH_LENGTH bytes and
         * would cut a longer one; such a file is skipped instead. */
        char src[LV_XML_MAX_PATH_LENGTH];
        if (snprintf(src, sizeof(src), "S:%s/%s.png", folder, names[i]) >= (int) sizeof(src)) continue;
        lv_xml_register_image(scope, names[i], src);
    }
}

static void delete_children_from(lv_obj_t * parent, uint32_t first) {
    while (lv_obj_get_child_count(parent) > first) lv_obj_delete(lv_obj_get_child(parent, (int32_t) first));
}

static prepared_xml_timeline_t * prepared_xml_timeline_for_anim(const lv_anim_t * anim) {
    for (prepared_xml_timeline_t * p = prepared_xml_timelines; p; p = p->next) {
        for (uint32_t i = 0; i < p->timeline->anim_dsc_cnt; ++i) {
            if (&p->timeline->anim_dsc[i].anim == anim) return p;
        }
    }
    return NULL;
}

static void prepared_xml_timeline_notify(prepared_xml_timeline_t * prepared, bool force) {
    if (!prepared || prepared->notifying_observers) return;
    uint32_t act_time = prepared->timeline->act_time;
    prepared->notifying_observers = true;
    uint32_t registration_ceiling = prepared->observer_registration_id;
    for (uint32_t i = 0; i < 4; ++i) {
        /* Read each slot immediately before invoking it. A callback may detach
         * another observer; a detach/reattach gets a newer id and waits until
         * the next timeline sample. */
        player_layouts_timeline_observer_cb_t callback = prepared->observers[i].callback;
        void * user_data = prepared->observers[i].user_data;
        uint32_t registration_id = prepared->observers[i].registration_id;
        if (!callback || registration_id > registration_ceiling) continue;
        if (!force && prepared->observers[i].has_act_time &&
            prepared->observers[i].act_time == act_time) continue;
        prepared->observers[i].act_time = act_time;
        prepared->observers[i].has_act_time = true;
        callback(prepared->timeline, act_time, user_data);
    }
    prepared->notifying_observers = false;
}

/* Prepared style animations encode selector and property in user_data. Keep
 * that word untouched: the write callback and transition query depend on it. */
static void style_timeline_exec_cb(lv_anim_t * anim, int32_t value) {
    /* LVGL updates timeline->act_time before it executes any child. Notify on
     * the first wrapped style animation for each new time, even when this
     * object's own style write is suppressed by a visual proxy. */
    prepared_xml_timeline_notify(prepared_xml_timeline_for_anim(anim), false);
    uint32_t packed = (uint32_t) (uintptr_t) lv_anim_get_user_data(anim);
    lv_style_prop_t prop = (lv_style_prop_t) (packed >> 24);
    lv_style_selector_t selector = (lv_style_selector_t) (packed & 0x00ffffffU);
    for (uint32_t i = 0; i < MAX_TIMELINE_SUPPRESSIONS; ++i) {
        timeline_suppression_t * s = &timeline_suppressions[i];
        if (s->timeline && s->obj == anim->var && s->prop == prop && s->selector == selector) return;
    }
    lv_style_value_t current;
    if (lv_obj_get_local_style_prop((lv_obj_t *) anim->var, prop, &current, selector) == LV_STYLE_RES_FOUND &&
        current.num == value) return;
    lv_style_value_t next;
    next.num = value;
    lv_obj_set_local_style_prop((lv_obj_t *) anim->var, prop, next, selector);
}

static bool xml_timeline_is_prepared(lv_anim_timeline_t * timeline) {
    for (prepared_xml_timeline_t * p = prepared_xml_timelines; p; p = p->next)
        if (p->timeline == timeline) return true;
    return false;
}

bool player_layouts_timeline_refresh_timing(lv_anim_timeline_t * timeline) {
    for (prepared_xml_timeline_t * p = prepared_xml_timelines; p; p = p->next) {
        if (p->timeline != timeline) continue;
        if (timeline->delay != (p->original_delay ? gui_anim_ms(p->original_delay) : 0))
            timeline->delay = p->original_delay ? gui_anim_ms(p->original_delay) : 0;
        if (timeline->repeat_delay != (p->original_repeat_delay ? gui_anim_ms(p->original_repeat_delay) : 0))
            timeline->repeat_delay = p->original_repeat_delay ? gui_anim_ms(p->original_repeat_delay) : 0;
        uint32_t limit = p->anim_count < timeline->anim_dsc_cnt ? p->anim_count : timeline->anim_dsc_cnt;
        for (uint32_t i = 0; i < limit; ++i) {
            uint32_t start = p->originals[i].start_time ? gui_anim_ms(p->originals[i].start_time) : 0;
            uint32_t duration = p->originals[i].duration ? gui_anim_ms(p->originals[i].duration) : 0;
            if (timeline->anim_dsc[i].start_time != start) timeline->anim_dsc[i].start_time = start;
            if (timeline->anim_dsc[i].anim.duration != duration) timeline->anim_dsc[i].anim.duration = duration;
        }
        return true;
    }
    return false;
}

bool player_layouts_timeline_observer_attach(lv_anim_timeline_t * timeline,
                                              player_layouts_timeline_observer_cb_t callback,
                                              void * user_data) {
    if (!timeline || !callback) return false;
    for (prepared_xml_timeline_t * p = prepared_xml_timelines; p; p = p->next) {
        if (p->timeline != timeline) continue;
        if (p->style_anim_count == 0) return false;
        for (uint32_t i = 0; i < 4; ++i) {
            if (p->observers[i].callback == callback && p->observers[i].user_data == user_data)
                return true;
        }
        for (uint32_t i = 0; i < 4; ++i) {
            if (p->observers[i].callback) continue;
            if (++p->observer_registration_id == 0) ++p->observer_registration_id;
            p->observers[i].callback = callback;
            p->observers[i].user_data = user_data;
            p->observers[i].registration_id = p->observer_registration_id;
            p->observers[i].has_act_time = false;
            return true;
        }
        return false;
    }
    return false;
}

bool player_layouts_timeline_observer_detach(lv_anim_timeline_t * timeline,
                                              player_layouts_timeline_observer_cb_t callback,
                                              void * user_data) {
    if (!timeline || !callback) return false;
    for (prepared_xml_timeline_t * p = prepared_xml_timelines; p; p = p->next) {
        if (p->timeline != timeline) continue;
        for (uint32_t i = 0; i < 4; ++i) {
            if (p->observers[i].callback != callback || p->observers[i].user_data != user_data) continue;
            memset(&p->observers[i], 0, sizeof(p->observers[i]));
            return true;
        }
        return false;
    }
    return false;
}

bool player_layouts_timeline_observer_sync(lv_anim_timeline_t * timeline) {
    if (!timeline) return false;
    for (prepared_xml_timeline_t * p = prepared_xml_timelines; p; p = p->next) {
        if (p->timeline != timeline) continue;
        bool attached = false;
        for (uint32_t i = 0; i < 4; ++i)
            if (p->observers[i].callback) attached = true;
        if (p->style_anim_count == 0 || !attached) return false;
        prepared_xml_timeline_notify(p, true);
        return true;
    }
    return false;
}

bool player_layouts_timeline_suppress_style(lv_anim_timeline_t * timeline, lv_obj_t * obj,
                                             lv_style_prop_t prop, lv_style_selector_t selector,
                                             bool suppress) {
    if (!timeline || !obj || !xml_timeline_is_prepared(timeline)) return false;
    timeline_suppression_t * free_slot = NULL;
    for (uint32_t i = 0; i < MAX_TIMELINE_SUPPRESSIONS; ++i) {
        timeline_suppression_t * s = &timeline_suppressions[i];
        if (!s->timeline && !free_slot) free_slot = s;
        if (s->timeline == timeline && s->obj == obj && s->prop == prop && s->selector == selector) {
            if (!suppress) memset(s, 0, sizeof(*s));
            return true;
        }
    }
    if (!suppress) return true;
    if (!free_slot) return false;
    free_slot->timeline = timeline;
    free_slot->obj = obj;
    free_slot->prop = prop;
    free_slot->selector = selector;
    return true;
}

/* Prepare only timelines actually requested by the UI. Allocate the marker
 * first so allocation failure leaves the timeline completely untouched. */
static void prepare_xml_timeline(lv_anim_timeline_t * timeline) {
    if (!timeline || xml_timeline_is_prepared(timeline)) return;
    if (timeline->anim_dsc_cnt > (SIZE_MAX - sizeof(prepared_xml_timeline_t)) /
                                 sizeof(((prepared_xml_timeline_t *) 0)->originals[0])) return;
    size_t bytes = sizeof(prepared_xml_timeline_t) +
                   (size_t) timeline->anim_dsc_cnt * sizeof(((prepared_xml_timeline_t *) 0)->originals[0]);
    prepared_xml_timeline_t * marker = calloc(1, bytes);
    if (!marker) return;
    marker->timeline = timeline;
    marker->original_delay = timeline->delay;
    marker->original_repeat_delay = timeline->repeat_delay;
    marker->anim_count = timeline->anim_dsc_cnt;
    for (uint32_t i = 0; i < timeline->anim_dsc_cnt; ++i) {
        lv_anim_timeline_dsc_t * dsc = &timeline->anim_dsc[i];
        marker->originals[i].start_time = dsc->start_time;
        marker->originals[i].duration = dsc->anim.duration;
        if (dsc->anim.custom_exec_cb) {
            marker->style_anim_count++;
            lv_anim_set_custom_exec_cb(&dsc->anim, style_timeline_exec_cb);
        }
    }
    /* Publish before refresh so timing is always derived from originals. */
    marker->next = prepared_xml_timelines;
    prepared_xml_timelines = marker;
    player_layouts_timeline_refresh_timing(timeline);
}

lv_anim_timeline_t * player_layouts_create_style_timeline(
    const player_layout_style_transition_t * transitions, uint32_t count) {
    if (!transitions || count == 0 ||
        (uint64_t)count * sizeof(lv_anim_timeline_dsc_t) > UINT32_MAX ||
        count > (SIZE_MAX - sizeof(prepared_xml_timeline_t)) /
                    sizeof(((prepared_xml_timeline_t *)0)->originals[0])) return NULL;

    for (uint32_t i = 0; i < count; ++i) {
        const player_layout_style_transition_t * transition = &transitions[i];
        uint32_t prop = (uint32_t)transition->prop;
        if (!transition->target || !lv_obj_is_valid(transition->target) ||
            prop == LV_STYLE_PROP_INV || prop >= LV_STYLE_PROP_ANY ||
            transition->selector > 0x00ffffffU || transition->start_time > INT32_MAX ||
            transition->duration > INT32_MAX) return NULL;
    }

    size_t marker_bytes = sizeof(prepared_xml_timeline_t) +
                          (size_t)count * sizeof(((prepared_xml_timeline_t *)0)->originals[0]);
    prepared_xml_timeline_t * marker = calloc(1, marker_bytes);
    if (!marker) return NULL;

    lv_anim_timeline_t * timeline = lv_anim_timeline_create();
    if (!timeline) {
        free(marker);
        return NULL;
    }
    uint32_t dsc_bytes = (uint32_t)((uint64_t)count * sizeof(lv_anim_timeline_dsc_t));
    lv_anim_timeline_dsc_t * descriptors = lv_malloc_zeroed(dsc_bytes);
    if (!descriptors) {
        lv_anim_timeline_delete(timeline);
        free(marker);
        return NULL;
    }

    /* lv_anim_timeline_add() increments the count then blindly assigns the
     * result of lv_realloc(). Allocate the complete descriptor array first so
     * failure is atomic and cannot leave a partially built timeline. */
    for (uint32_t i = 0; i < count; ++i) {
        const player_layout_style_transition_t * transition = &transitions[i];
        lv_anim_t * anim = &descriptors[i].anim;
        lv_anim_init(anim);
        lv_anim_set_var(anim, transition->target);
        lv_anim_set_values(anim, transition->start_value, transition->end_value);
        lv_anim_set_duration(anim, transition->duration ? gui_anim_ms(transition->duration) : 0);
        lv_anim_set_path_cb(anim, transition->path_cb ? transition->path_cb : lv_anim_path_linear);
        lv_anim_set_early_apply(anim, transition->animation.early_apply);
        uint32_t packed = ((uint32_t)transition->prop << 24) | transition->selector;
        lv_anim_set_user_data(anim, (void *)(uintptr_t)packed);
        lv_anim_set_custom_exec_cb(anim, style_timeline_exec_cb);
        descriptors[i].start_time = transition->start_time ? gui_anim_ms(transition->start_time) : 0;
        marker->originals[i].start_time = transition->start_time;
        marker->originals[i].duration = transition->duration;
    }

    timeline->anim_dsc = descriptors;
    timeline->anim_dsc_cnt = count;
    marker->timeline = timeline;
    marker->anim_count = count;
    marker->style_anim_count = count;
    marker->owned_style_timeline = true;
    marker->next = prepared_xml_timelines;
    prepared_xml_timelines = marker;
    return timeline;
}

void player_layouts_destroy_style_timeline(lv_anim_timeline_t * timeline) {
    if (!timeline) return;
    prepared_xml_timeline_t ** link = &prepared_xml_timelines;
    while (*link && ((*link)->timeline != timeline || !(*link)->owned_style_timeline))
        link = &(*link)->next;
    if (!*link) return;

    prepared_xml_timeline_t * prepared = *link;
    *link = prepared->next;
    for (uint32_t i = 0; i < MAX_TIMELINE_SUPPRESSIONS; ++i) {
        if (timeline_suppressions[i].timeline == timeline)
            memset(&timeline_suppressions[i], 0, sizeof(timeline_suppressions[i]));
    }
    lv_anim_timeline_pause(timeline);
    lv_anim_timeline_delete(timeline);
    free(prepared);
}

/* The engine registers its widget parsers and a global scope on start, so it
 * is started by the first XML layout rather than for everyone. It lives for
 * the rest of the process: soft UI reloads keep LVGL, and with it the engine,
 * alive, and each layout's own component is released separately. */
static void start_xml_engine_once(void) {
    if (xml_engine_ready) return;
    lv_xml_init();
    xml_engine_ready = true;
}

static lv_obj_t * create_xml_layout(const char * entry_path, lv_obj_t * parent) {
    char path[MAX_LAYOUT_PATH];
    resolve_board_variant(entry_path, path, sizeof(path));
    char * xml = read_text_file(path);
    if (!xml) {
        fprintf(stderr, "Warning: player layout: cannot read %s\n", path);
        return NULL;
    }

    start_xml_engine_once();
    register_xml_globals_once();
    player_layouts_release();
    lv_result_t res = lv_xml_register_component_from_data(XML_COMPONENT_NAME, xml);
    free(xml);
    if (res != LV_RESULT_OK) {
        fprintf(stderr, "Warning: player layout: %s is not a valid LVGL XML component\n", path);
        return NULL;
    }
    xml_component_registered = true;

    /* Images go into the component's own scope, after its metadata pass: the
     * paths depend on theme overrides, which may differ between builds, and
     * the scope is released together with the layout. A name the XML declares
     * itself keeps precedence, then the layout's own image folder, then the
     * built-in names. */
    lv_xml_component_scope_t * scope = lv_xml_component_get_scope(XML_COMPONENT_NAME);
    reset_interactions();
    const char * lyrics = scope ? lv_xml_get_const(scope, "compas_lyrics_enabled") : NULL;
    const char * artist = scope ? lv_xml_get_const(scope, "compas_artist_action") : NULL;
    const char * album = scope ? lv_xml_get_const(scope, "compas_album_action") : NULL;
    if (lyrics) {
        if (strcmp(lyrics, "true") == 0) active_interactions.lyrics_enabled = true;
        else if (strcmp(lyrics, "false") == 0) active_interactions.lyrics_enabled = false;
        else goto invalid_interactions;
    }
    if ((artist && !action_name_is_valid(artist, true)) || (album && !action_name_is_valid(album, true)))
        goto invalid_interactions;
    if (artist) snprintf(active_interactions.artist_action, sizeof(active_interactions.artist_action), "%s", artist);
    if (album) snprintf(active_interactions.album_action, sizeof(active_interactions.album_action), "%s", album);
    register_layout_folder_images(scope, entry_path);
    for (size_t i = 0; scope && i < sizeof(layout_images) / sizeof(layout_images[0]); ++i) {
        const char * image = asset_path(layout_images[i].relative_path);
#if defined(BOARD_TEMPOTEC_V1)
        if (strcmp(layout_images[i].name, "btn_back") == 0) {
            image = asset_path("navigation/back.png");
            const char * chevron = titlebar_breeze_icon(image);
            if (chevron) image = chevron;
        }
#endif
        lv_xml_register_image(scope, layout_images[i].name, image);
    }

    uint32_t children_before = lv_obj_get_child_count(parent);
    lv_obj_t * root = lv_xml_create(parent, XML_COMPONENT_NAME, NULL);
    if (!root) {
        fprintf(stderr, "Warning: player layout: %s failed to build\n", path);
        delete_children_from(parent, children_before);
        player_layouts_release();
        return NULL;
    }
    return root;

invalid_interactions:
    fprintf(stderr, "Warning: player layout: %s has invalid interaction constants\n", path);
    player_layouts_release();
    return NULL;
}

lv_obj_t * player_layouts_create(const char * id, lv_obj_t * parent, player_layout_kind_t * kind_out) {
    invalidate_active_layout();
    ensure_builtin_entry();
    int index = player_layouts_find(id);
    if (index < 0 || !parent) return NULL;
    const layout_entry_t * e = &entries[index];
    if (kind_out) *kind_out = e->info.kind;
    active_timeline_fn = NULL;
    active_xml_layout = false;

    lv_obj_t * root = NULL;
    if (e->info.kind == PLAYER_LAYOUT_XML_FILE) {
        root = create_xml_layout(e->path, parent);
        active_xml_layout = root != NULL;
    } else if (e->create) {
        uint32_t children_before = lv_obj_get_child_count(parent);
        root = e->create(parent);
        if (!root) delete_children_from(parent, children_before);
        else {
            active_timeline_fn = e->timeline;
            reset_interactions();
        }
    }
    if (root) {
        active_root = root;
        if (e->info.kind == PLAYER_LAYOUT_XML_FILE)
            resolve_plugin_owner(e, active_owner, sizeof(active_owner));
        lv_obj_add_event_cb(root, active_root_delete_cb, LV_EVENT_DELETE, NULL);
    }
    return root;
}

lv_anim_timeline_t * player_layouts_find_timeline(lv_obj_t * root, const char * name) {
    if (!root || !name) return NULL;
    if (active_timeline_fn) return active_timeline_fn(root, name);
    if (!xml_engine_ready) return NULL;

    lv_anim_timeline_t ** timelines = NULL;
    lv_obj_send_event(root, (lv_event_code_t) lv_xml_compat_globals.lv_event_xml_store_timeline, &timelines);
    if (!timelines) return NULL;
    for (uint32_t i = 0; timelines[i]; ++i) {
        const char * timeline_name = lv_anim_timeline_get_user_data(timelines[i]);
        if (timeline_name && strcmp(timeline_name, name) == 0) {
            if (active_xml_layout) prepare_xml_timeline(timelines[i]);
            return timelines[i];
        }
    }
    return NULL;
}

static void sample_style_transition(lv_anim_timeline_t * timeline, lv_anim_timeline_dsc_t * dsc,
                                    player_layout_style_transition_t * transition) {
    lv_anim_t * anim = &dsc->anim;
    transition->start_value = anim->start_value;
    transition->end_value = anim->end_value;
    transition->start_time = dsc->start_time;
    transition->duration = anim->duration;
    transition->current_value = anim->current_value;
    transition->path_cb = anim->path_cb;
    transition->animation = *anim;
    transition->has_current_value = false;
    if (timeline->act_time < (int32_t) dsc->start_time) {
        transition->has_current_value = anim->early_apply;
        transition->animation.act_time = 0;
        if (anim->early_apply) transition->current_value = anim->start_value;
    } else {
        transition->has_current_value = true;
        transition->animation.act_time = (int32_t) (timeline->act_time - dsc->start_time);
        if (transition->animation.act_time > transition->animation.duration)
            transition->animation.act_time = transition->animation.duration;
        transition->current_value = transition->animation.duration == 0 ? anim->end_value :
            (transition->animation.path_cb ? transition->animation.path_cb(&transition->animation) : anim->end_value);
    }
}

bool player_layouts_timeline_style_transition(lv_anim_timeline_t * timeline, lv_obj_t * obj,
                                               lv_style_prop_t prop, lv_style_selector_t selector,
                                               player_layout_style_transition_t * transition) {
    if (!timeline || !obj || !transition || !xml_timeline_is_prepared(timeline)) return false;
    for (uint32_t i = 0; i < timeline->anim_dsc_cnt; ++i) {
        lv_anim_timeline_dsc_t * dsc = &timeline->anim_dsc[i];
        lv_anim_t * anim = &dsc->anim;
        if (anim->custom_exec_cb != style_timeline_exec_cb || anim->var != obj) continue;
        uint32_t packed = (uint32_t) (uintptr_t) lv_anim_get_user_data(anim);
        if ((lv_style_prop_t) (packed >> 24) != prop ||
            (lv_style_selector_t) (packed & 0x00ffffffU) != selector) continue;
        sample_style_transition(timeline, dsc, transition);
        transition->target = obj;
        transition->prop = prop;
        transition->selector = selector;
        return true;
    }
    return false;
}

uint32_t player_layouts_timeline_style_transition_count(lv_anim_timeline_t * timeline, lv_obj_t * obj,
                                                         lv_style_prop_t prop, lv_style_selector_t selector) {
    if (!timeline || !obj || !xml_timeline_is_prepared(timeline)) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < timeline->anim_dsc_cnt; ++i) {
        lv_anim_t * anim = &timeline->anim_dsc[i].anim;
        if (anim->custom_exec_cb != style_timeline_exec_cb || anim->var != obj) continue;
        uint32_t packed = (uint32_t) (uintptr_t) lv_anim_get_user_data(anim);
        if ((lv_style_prop_t) (packed >> 24) == prop &&
            (lv_style_selector_t) (packed & 0x00ffffffU) == selector) ++count;
    }
    return count;
}

uint32_t player_layouts_timeline_style_animation_count(lv_anim_timeline_t * timeline) {
    if (!timeline || !xml_timeline_is_prepared(timeline)) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < timeline->anim_dsc_cnt; ++i)
        if (timeline->anim_dsc[i].anim.custom_exec_cb == style_timeline_exec_cb) ++count;
    return count;
}

bool player_layouts_timeline_get_style_animation(lv_anim_timeline_t * timeline, uint32_t index,
                                                  player_layout_style_transition_t * transition) {
    if (!timeline || !transition || !xml_timeline_is_prepared(timeline)) return false;
    for (uint32_t i = 0; i < timeline->anim_dsc_cnt; ++i) {
        lv_anim_timeline_dsc_t * dsc = &timeline->anim_dsc[i];
        lv_anim_t * anim = &dsc->anim;
        if (anim->custom_exec_cb != style_timeline_exec_cb) continue;
        if (index-- != 0) continue;
        uint32_t packed = (uint32_t) (uintptr_t) lv_anim_get_user_data(anim);
        transition->target = (lv_obj_t *) anim->var;
        transition->prop = (lv_style_prop_t) (packed >> 24);
        transition->selector = (lv_style_selector_t) (packed & 0x00ffffffU);
        sample_style_transition(timeline, dsc, transition);
        return true;
    }
    return false;
}

bool player_layouts_timeline_has_animation_for_obj(lv_anim_timeline_t * timeline, lv_obj_t * obj) {
    if (!timeline || !obj) return false;
    for (uint32_t i = 0; i < timeline->anim_dsc_cnt; ++i)
        if (timeline->anim_dsc[i].anim.var == obj) return true;
    return false;
}

uint32_t player_layouts_timeline_animation_count_for_obj(lv_anim_timeline_t * timeline, lv_obj_t * obj) {
    if (!timeline || !obj) return 0;
    uint32_t count = 0;
    for (uint32_t i = 0; i < timeline->anim_dsc_cnt; ++i)
        if (timeline->anim_dsc[i].anim.var == obj) ++count;
    return count;
}
