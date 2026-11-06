#include "gui_subsonic_menu.h"
#include "../../assets/assets.h"
#include "../../home/launcher_layout.h"
#include "../../shell/gui_navigation.h"
#include "../../text/i18n/i18n.h"
#include "../../widgets/builders/screen_builders.h"

static lv_obj_t * menu_screen;
static gui_subsonic_menu_actions_t menu_actions;

static lv_obj_t * build_menu(void) {
    /* Same icons as the Music page; add rows here as the menu grows. */
    const icon_grid_item_t items[] = {
        { row_icon_or("submenu/cover_flow.png", "submenu/albums.png"), NULL, TR("Cover Flow"),
          menu_actions.cover_flow, NULL },
        { "submenu/artists.png", NULL, TR("Artists"), menu_actions.artists, NULL },
        { "submenu/albums.png", NULL, TR("Albums"), menu_actions.albums, NULL },
        { "submenu/genres.png", NULL, TR("Genres"), menu_actions.genres, NULL },
        { "submenu/all_songs.png", NULL, TR("Random Songs"), menu_actions.random_songs, NULL },
        { "submenu/playlists.png", NULL, TR("Playlists"), menu_actions.playlists, NULL },
        { "submenu/favorites.png", NULL, TR("Favorites"), menu_actions.favorites, NULL },
    };
    lv_obj_t * scr = build_category_menu_screen(TR("Subsonic"), generic_back_cb, items,
                                                (int) (sizeof(items) / sizeof(items[0])),
                                                &launcher_layout_config.music);
    if (!scr) return NULL;
    build_top_right_icon_button(scr, asset_path("settings/playback.png"), menu_actions.settings);
    finalize_screen_navigation(scr);
    return scr;
}

void gui_subsonic_menu_init(const gui_subsonic_menu_actions_t * actions) {
    menu_actions = *actions;
    menu_screen = build_menu();
}

lv_obj_t * gui_subsonic_menu_screen(void) { return menu_screen; }

void gui_subsonic_menu_refresh(void) {
    if (!menu_screen) return;
    lv_obj_t * old = menu_screen;
    lv_obj_t * fresh = build_menu();
    if (!fresh) return;
    menu_screen = fresh;
    gui_navigation_replace_screen(old, fresh);
    lv_obj_delete(old);
}

void gui_subsonic_menu_teardown(void) {
    if (menu_screen) lv_obj_delete(menu_screen);
    menu_screen = NULL;
}
