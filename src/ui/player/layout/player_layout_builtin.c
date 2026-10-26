#include "../../application/gui.h"
#include "../screen/gui_player.h"
#include "../../assets/assets.h"
#include "../../text/i18n/i18n.h"
#include "../../text/rendering/fallback_font.h"
#include "../../../audio/engine/audio.h"
#include "../../../core/persistence/settings.h"
#include "player_layout_internal.h"
#include "player_layout_builtin.h"

static lv_obj_t * create_cover_card(lv_obj_t * scr) {
    /* Cover card (clipped, crisp, unblurred) */
    lv_obj_t * card = lv_obj_create(scr);
    lv_obj_set_name_static(card, "cover_card");
    lv_obj_remove_style_all(card);
    /* Centered directly from the resolved card size rather than a fixed
     * offset literal -- that would only center a player_s(350) card when
     * player_x and player_s agree, which is true on R1 but not on the other
     * boards. 350x350 matches COVER_ART_WIDTH/HEIGHT (gui.h) exactly, so the
     * reference board needs no runtime scale at all in fit_cover_img_to_card(). */
    lv_obj_set_pos(card, (BOARD_SCREEN_WIDTH - player_s(350)) / 2, player_y(152));
    lv_obj_set_size(card, player_s(350), player_s(350));
    lv_obj_set_style_radius(card, player_s(10), 0);
    lv_obj_set_style_clip_corner(card, true, 0);
    lv_obj_set_style_bg_opa(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * cover = lv_image_create(card);
    lv_obj_set_name_static(cover, "cover_img");
    lv_image_set_src(cover, asset_path("playing_plane/default_cover_565.png"));

    lv_obj_t * fav_circle = lv_obj_create(card);
    lv_obj_set_name_static(fav_circle, "favorite_circle");
    lv_obj_remove_style_all(fav_circle);
    lv_obj_set_pos(fav_circle, player_s(296), player_s(296)); /* 352-44-12=296 from each edge */
    lv_obj_set_size(fav_circle, player_s(44), player_s(44));
    lv_obj_set_style_radius(fav_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(fav_circle, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(fav_circle, LV_OPA_40, 0);
    lv_obj_add_style(fav_circle, gui_theme_accent_outline_style(), 0);
    lv_obj_remove_flag(fav_circle, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * fav_icon = lv_image_create(fav_circle);
    lv_obj_set_name_static(fav_icon, "favorite_icon");
    lv_image_set_src(fav_icon, asset_path("playing_plane/collect_out.png"));
    player_fit_small_screen_icon(fav_icon, 40);
    lv_obj_add_style(fav_icon, &icon_press_style, LV_STATE_PRESSED);
    lv_obj_center(fav_icon);

    return card;
}

static void create_metadata_labels(lv_obj_t * scr, const lv_font_t * player_title_font, const lv_font_t * player_meta_font) {
    lv_obj_t * title = lv_label_create(scr);
    lv_obj_set_name_static(title, "title");
    lv_label_set_text(title, TR("No track loaded"));
    lv_obj_add_style(title, gui_player_backdrop_text_style(false), 0);
    lv_obj_set_style_text_font(title, player_title_font, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(title, player_x(22));
    lv_obj_set_width(title, BOARD_SCREEN_WIDTH - 2 * player_x(22));

    lv_obj_t * album = lv_label_create(scr);
    lv_obj_set_name_static(album, "album");
    lv_label_set_text(album, "");
    lv_obj_add_style(album, gui_player_backdrop_text_style(true), 0);
    lv_obj_set_style_text_font(album, player_meta_font, 0);
    lv_obj_set_style_text_align(album, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(album, player_x(22));
    /* Content height so circular marquee does not fall back to vertical scrolling. */
    lv_obj_set_width(album, BOARD_SCREEN_WIDTH - 2 * player_x(22));
    lv_obj_set_height(album, LV_SIZE_CONTENT);

    lv_obj_t * artist = lv_label_create(scr);
    lv_obj_set_name_static(artist, "artist");
    lv_label_set_text(artist, "");
    lv_obj_add_style(artist, gui_player_backdrop_text_style(true), 0);
    lv_obj_set_style_text_font(artist, player_meta_font, 0);
    lv_obj_set_style_text_align(artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_x(artist, player_x(22));
    /* Circular marquee falls back to vertical scrolling when text exceeds
     * a fixed height. Fit the actual font metrics (including loaded fallback
     * fonts) while retaining the fixed width for horizontal overflow. */
    lv_obj_set_width(artist, BOARD_SCREEN_WIDTH - 2 * player_x(22));
    lv_obj_set_height(artist, LV_SIZE_CONTENT);
}

static void create_dismiss_button(lv_obj_t * scr) {
    /* Back/dismiss button. build_header_back_button()'s shared 64x64 default
     * (TITLE_ROW_HEIGHT, used by every other screen's header) would reach
     * further into the album label's row than the symmetric 44x44 "more"
     * target on the opposite corner -- shrink to match it instead of
     * needlessly widening this corner's overlap with the metadata block. */
    lv_obj_t * dismiss = build_header_back_button(scr, NULL);
    lv_obj_set_name_static(dismiss, "dismiss_btn");
    lv_obj_set_size(dismiss, player_s(44), player_s(44));
    lv_obj_set_pos(dismiss, player_x(22), player_y(62));
    /* The reference keeps the status bar unobstructed and has no visible
     * back arrow on the Player. Retain the transparent hit target so the
     * established tap-to-dismiss behavior remains available. */
    lv_obj_t * dismiss_arrow = lv_obj_get_child(dismiss, 0);
    if (dismiss_arrow) {
        /* The Now Playing dismiss target intentionally retains its original
         * hidden stock back asset; the visible navigation back glyph is for
         * standard screen headers only. */
        lv_image_set_src(dismiss_arrow, asset_path("sub_back/btn_back.png"));
        lv_obj_set_style_image_recolor_opa(dismiss_arrow, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(dismiss_arrow, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_quality_pill(lv_obj_t * scr) {
    /* Quality pill */
    lv_obj_t * pill = lv_obj_create(scr);
    lv_obj_set_name_static(pill, "quality_pill");
    lv_obj_remove_style_all(pill);
    /* This label is intentionally not marquee-animated: the pill grows to
     * the complete, short format string and therefore never clips codec,
     * bit-depth, or sample-rate information. */
    lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(pill, player_s(36), 0);
    lv_obj_align(pill, LV_ALIGN_TOP_MID, 0, player_y(518));
    lv_obj_set_style_pad_hor(pill, player_s(14), 0);
    lv_obj_set_style_pad_ver(pill, player_s(6), 0);
    lv_obj_set_style_pad_column(pill, player_s(10), 0);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_20, 0);
    lv_obj_set_style_bg_color(pill, lv_color_hex(0x000000), 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * quality_waveform = lv_image_create(pill);
    lv_image_set_src(quality_waveform, asset_path("playing_plane/quality_waveform.png"));
    lv_image_set_scale(quality_waveform, (player_s(20) * LV_SCALE_NONE) / 20);
    lv_obj_set_size(quality_waveform, player_s(20), player_s(20));
    lv_obj_add_style(quality_waveform, gui_theme_accent_style(), 0);
    lv_obj_set_style_bg_opa(quality_waveform, LV_OPA_TRANSP, 0);
    lv_obj_set_style_image_recolor_opa(quality_waveform, LV_OPA_COVER, 0);

    lv_obj_t * format_badge = lv_label_create(pill);
    lv_obj_set_name_static(format_badge, "format_badge");
    lv_label_set_text(format_badge, "");
    lv_obj_add_style(format_badge, gui_player_backdrop_text_style(true), 0);
    lv_obj_set_style_text_font(format_badge, &app_font_16, 0);
    lv_obj_set_width(format_badge, LV_SIZE_CONTENT);
}

static void create_progress_and_time(lv_obj_t * scr, const lv_font_t * player_meta_font) {
    /* Progress bar (native rail, not the old fixed PNG sprites) */
    lv_obj_t * slider = lv_slider_create(scr);
    lv_obj_set_name_static(slider, "progress_slider");
    lv_obj_set_pos(slider, player_x(30), player_y(575));
    lv_obj_set_size(slider, player_x(420), SLIDER_TRACK_HEIGHT);
    configure_native_slider_rail(slider);
    lv_obj_add_style(slider, gui_theme_accent_style(), LV_PART_INDICATOR);
    lv_obj_add_style(slider, gui_theme_accent_knob_style(), LV_PART_KNOB);
    lv_obj_set_style_width(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);
    lv_obj_set_style_height(slider, SLIDER_KNOB_SIZE, LV_PART_KNOB);

    /* Time row */
    lv_obj_t * time_row = lv_obj_create(scr);
    lv_obj_set_pos(time_row, player_x(30), player_y(610));
    lv_obj_set_size(time_row, player_x(420), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_row, 0, 0);
    lv_obj_set_style_border_width(time_row, 0, 0);
    lv_obj_set_style_pad_all(time_row, 0, 0);
    lv_obj_remove_flag(time_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(time_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(time_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * pos = lv_label_create(time_row);
    lv_obj_set_name_static(pos, "pos_label");
    lv_label_set_text(pos, "0:00");
    lv_obj_add_style(pos, gui_player_backdrop_text_style(true), 0);
    lv_obj_set_style_text_font(pos, player_meta_font, 0);

    lv_obj_t * dur = lv_label_create(time_row);
    lv_obj_set_name_static(dur, "dur_label");
    lv_label_set_text(dur, "--:--");
    lv_obj_add_style(dur, gui_player_backdrop_text_style(true), 0);
    lv_obj_set_style_text_font(dur, player_meta_font, 0);
}

static void create_song_count(lv_obj_t * card) {
    /* Current position in the active queue, overlaid on the cover. */
    lv_obj_t * count = lv_label_create(card);
    lv_obj_set_name_static(count, "song_count");
    lv_label_set_text(count, "--/--");
    lv_obj_add_style(count, gui_player_backdrop_text_style(false), 0);
    lv_obj_set_style_text_font(count, &app_font_16, 0);
    lv_obj_set_style_bg_color(count, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(count, LV_OPA_50, 0);
    lv_obj_set_style_radius(count, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(count, player_s(9), 0);
    lv_obj_set_style_pad_ver(count, player_s(5), 0);
    lv_obj_set_pos(count, player_s(10), player_s(10));
    lv_obj_remove_flag(count, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(count, LV_OBJ_FLAG_HIDDEN);
}

static void create_transport_row(lv_obj_t * scr) {
    /* Transport row: order, previous, play/pause, next, more. Cover taps are
     * the sole lyrics actuator, leaving the final slot for song actions. */
    lv_obj_t * controls_row = lv_obj_create(scr);
    lv_obj_remove_style_all(controls_row);
    const int32_t transport_top = player_y(645);
    lv_obj_set_pos(controls_row, 0, transport_top);
    lv_obj_set_size(controls_row, BOARD_SCREEN_WIDTH, player_y(94));
    lv_obj_remove_flag(controls_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t * order = lv_image_create(controls_row);
    lv_obj_set_name_static(order, "order_btn");
    lv_image_set_src(order, asset_path(play_mode_icon_asset((play_mode_t) current_settings.play_mode)));
    player_fit_small_screen_icon(order, 40);
    lv_obj_add_style(order, gui_player_backdrop_icon_style(), 0);
    lv_obj_add_style(order, &icon_press_style, LV_STATE_PRESSED);

    lv_obj_t * prev = lv_image_create(controls_row);
    lv_obj_set_name_static(prev, "prev_btn");
    lv_image_set_src(prev, asset_path("playing_plane/btn_prev.png"));
    player_fit_small_screen_icon(prev, 40);
    lv_obj_add_style(prev, gui_player_backdrop_icon_style(), 0);

    /* btn_play.png/btn_pause.png already contain their own filled accent
     * circle -- no separate outline ring needed here, unlike favorite_circle/
     * quality_pill which wrap plain glyphs/text with no ring of their own. */
    lv_obj_t * play = lv_image_create(controls_row);
    lv_obj_set_name_static(play, "play_btn");
    gui_player_prepare_play_btn_images();
    lv_image_set_src(play, gui_player_play_btn_image_src(audio_is_playing()));
    player_fit_small_screen_icon(play, 84);
    lv_obj_add_style(play, &icon_press_style, LV_STATE_PRESSED);

    lv_obj_t * more = lv_image_create(controls_row);
    lv_obj_set_name_static(more, "more_btn");
    lv_image_set_src(more, asset_path("playing_plane/ic_more.png"));
    player_fit_small_screen_icon(more, 40);
    lv_obj_add_style(more, gui_player_backdrop_icon_style(), 0);
    lv_obj_add_style(more, &icon_press_style, LV_STATE_PRESSED);

    lv_obj_t * next = lv_image_create(controls_row);
    lv_obj_set_name_static(next, "next_btn");
    lv_image_set_src(next, asset_path("playing_plane/btn_next.png"));
    player_fit_small_screen_icon(next, 40);
    lv_obj_add_style(next, gui_player_backdrop_icon_style(), 0);

    lv_obj_update_layout(controls_row);
    int32_t row_h = lv_obj_get_height(controls_row);
    lv_obj_set_pos(play, player_x(240) - lv_obj_get_width(play) / 2, (row_h - lv_obj_get_height(play)) / 2);
    lv_obj_set_pos(order, player_x(57) - lv_obj_get_width(order) / 2, (row_h - lv_obj_get_height(order)) / 2);
    lv_obj_set_pos(prev, player_x(142) - lv_obj_get_width(prev) / 2, (row_h - lv_obj_get_height(prev)) / 2);
    lv_obj_set_pos(more, player_x(423) - lv_obj_get_width(more) / 2, (row_h - lv_obj_get_height(more)) / 2);
    lv_obj_set_pos(next, player_x(338) - lv_obj_get_width(next) / 2, (row_h - lv_obj_get_height(next)) / 2);
}

lv_obj_t * player_layout_create_builtin(lv_obj_t * scr) {
    /* Full-screen fallback surface and background reflection image */
    lv_obj_t * overlay_panel = lv_obj_create(scr);
    lv_obj_set_name_static(overlay_panel, "overlay_panel");
    player_init_overlay_panel(overlay_panel);
    player_create_hidden_background_img(overlay_panel);

    lv_obj_t * card = create_cover_card(scr);

    /* The supplied reference is the R1 480x800 composition. These compact
     * fonts keep the three metadata lines clear of the cover on shorter panels. */
    const lv_font_t * player_title_font = &app_font_player_title;
    const lv_font_t * player_meta_font = &app_font_player_meta;

    create_metadata_labels(scr, player_title_font, player_meta_font);
    create_dismiss_button(scr);
    create_quality_pill(scr);
    create_progress_and_time(scr, player_meta_font);
    create_song_count(card);
    create_transport_row(scr);

    lv_obj_t * volume = lv_slider_create(scr);
    lv_obj_set_name_static(volume, "volume_slider");
    lv_obj_add_flag(volume, LV_OBJ_FLAG_HIDDEN);

    return scr;
}
