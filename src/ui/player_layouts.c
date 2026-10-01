/* Player layout registry and the XML loader behind it. See player_layouts.h
 * for the model and docs/PLAYER_LAYOUTS.md for the widget-name contract. */
#include "player_layouts.h"
#include "board_config.h"
#include "fallback_font.h"
#include "assets.h"
#include "settings.h"

#include "../../third_party/lv_xml/src/others/xml/lv_xml.h"
#include "../../third_party/lv_xml/src/others/xml/lv_xml_compat.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef HOST_BUILD
  #define PLUGIN_LAYOUT_DIR "./music/.plugins/player_layouts"
#else
  #define PLUGIN_LAYOUT_DIR "/data/mnt/sd_0/.plugins/player_layouts"
#endif

#define MAX_LAYOUTS 24
#define MAX_LAYOUT_PATH 512
#define MAX_XML_BYTES (256 * 1024)
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
} layout_entry_t;

static layout_entry_t entries[MAX_LAYOUTS];
static int entry_count;
static char session_selected[PLAYER_LAYOUT_ID_MAX];
static bool xml_engine_ready;
static bool xml_globals_ready;
static bool xml_component_registered;
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
};

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

int player_layouts_find(const char * id) {
    ensure_builtin_entry();
    if (!id) return -1;
    for (int i = 0; i < entry_count; ++i) {
        if (strcmp(entries[i].info.id, id) == 0) return i;
    }
    return -1;
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
    if (existing >= 0 && !entries[existing].session) return false;
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

/* ---- XML file discovery ---- */

typedef struct {
    char id[PLAYER_LAYOUT_ID_MAX];
    char path[MAX_LAYOUT_PATH];
} found_layout_t;

static int found_cmp(const void * a, const void * b) {
    return strcmp(((const found_layout_t *) a)->id, ((const found_layout_t *) b)->id);
}

/* `foo@480x800.xml` is a board variant of `foo.xml`, never an entry itself. */
static bool file_is_board_variant(const char * name) {
    return strchr(name, '@') != NULL;
}

static void scan_directory(const char * dir, found_layout_t * found, int * found_count, int max_found) {
    if (!dir || !dir[0]) return;
    DIR * d = opendir(dir);
    if (!d) return;
    struct dirent * ent;
    while ((ent = readdir(d)) != NULL) {
        size_t len = strlen(ent->d_name);
        if (len < 5 || strcasecmp(ent->d_name + len - 4, ".xml") != 0) continue;
        if (ent->d_name[0] == '.' || file_is_board_variant(ent->d_name)) continue;
        char id[PLAYER_LAYOUT_ID_MAX];
        if (len - 4 >= sizeof(id)) continue;
        memcpy(id, ent->d_name, len - 4);
        id[len - 4] = '\0';
        if (!id_is_valid(id)) continue;
        char path[MAX_LAYOUT_PATH];
        if (snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name) >= (int) sizeof(path)) continue;
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        /* A later directory replaces an earlier file of the same id. */
        int slot = -1;
        for (int i = 0; i < *found_count; ++i) {
            if (strcmp(found[i].id, id) == 0) { slot = i; break; }
        }
        if (slot < 0) {
            if (*found_count >= max_found) continue;
            slot = (*found_count)++;
        }
        snprintf(found[slot].id, sizeof(found[slot].id), "%s", id);
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
    scan_directory(dir, found, &found_count, MAX_LAYOUTS);
    if (asset_override_root()) {
        join_layout_dir(dir, sizeof(dir), asset_override_root());
        scan_directory(dir, found, &found_count, MAX_LAYOUTS);
    }
    scan_directory(PLUGIN_LAYOUT_DIR, found, &found_count, MAX_LAYOUTS);
    qsort(found, (size_t) found_count, sizeof(found[0]), found_cmp);

    for (int i = 0; i < found_count; ++i) {
        /* Never shadow a C layout or a plugin's session layout of the same id. */
        if (player_layouts_find(found[i].id) >= 0) continue;
        layout_entry_t * e = upsert_entry(found[i].id, found[i].id, PLAYER_LAYOUT_XML_FILE);
        if (!e) break;
        snprintf(e->path, sizeof(e->path), "%s", found[i].path);
    }
}

void player_layouts_rescan(void) {
    ensure_builtin_entry();
    rescan_xml_layouts();
}

void player_layouts_init(player_layout_create_fn builtin_create) {
    ensure_builtin_entry();
    entries[0].create = builtin_create;
    rescan_xml_layouts();
}

/* ---- XML creation ---- */

static char * read_text_file(const char * path) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    char * buffer = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > 0 && size <= MAX_XML_BYTES && fseek(f, 0, SEEK_SET) == 0) {
            buffer = malloc((size_t) size + 1);
            if (buffer && fread(buffer, 1, (size_t) size, f) == (size_t) size) {
                buffer[size] = '\0';
            } else {
                free(buffer);
                buffer = NULL;
            }
        }
    }
    fclose(f);
    return buffer;
}

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
    xml_globals_ready = true;
}

void player_layouts_release(void) {
    if (!xml_component_registered) return;
    lv_xml_component_unregister(XML_COMPONENT_NAME);
    xml_component_registered = false;
}

static void delete_children_from(lv_obj_t * parent, uint32_t first) {
    while (lv_obj_get_child_count(parent) > first) lv_obj_delete(lv_obj_get_child(parent, (int32_t) first));
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
     * itself keeps precedence. */
    lv_xml_component_scope_t * scope = lv_xml_component_get_scope(XML_COMPONENT_NAME);
    for (size_t i = 0; scope && i < sizeof(layout_images) / sizeof(layout_images[0]); ++i)
        lv_xml_register_image(scope, layout_images[i].name, asset_path(layout_images[i].relative_path));

    uint32_t children_before = lv_obj_get_child_count(parent);
    lv_obj_t * root = lv_xml_create(parent, XML_COMPONENT_NAME, NULL);
    if (!root) {
        fprintf(stderr, "Warning: player layout: %s failed to build\n", path);
        delete_children_from(parent, children_before);
        player_layouts_release();
        return NULL;
    }
    return root;
}

lv_obj_t * player_layouts_create(const char * id, lv_obj_t * parent, player_layout_kind_t * kind_out) {
    ensure_builtin_entry();
    int index = player_layouts_find(id);
    if (index < 0 || !parent) return NULL;
    const layout_entry_t * e = &entries[index];
    if (kind_out) *kind_out = e->info.kind;
    active_timeline_fn = NULL;

    lv_obj_t * root = NULL;
    if (e->info.kind == PLAYER_LAYOUT_XML_FILE) {
        root = create_xml_layout(e->path, parent);
    } else if (e->create) {
        uint32_t children_before = lv_obj_get_child_count(parent);
        root = e->create(parent);
        if (!root) delete_children_from(parent, children_before);
        else active_timeline_fn = e->timeline;
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
        if (timeline_name && strcmp(timeline_name, name) == 0) return timelines[i];
    }
    return NULL;
}
