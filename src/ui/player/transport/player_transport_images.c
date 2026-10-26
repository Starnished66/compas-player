#include "player_transport_images.h"
#include "../../assets/assets.h"
#include "../../appearance/gui_theme.h"
#include "../screen/gui_player.h"

/* Decoded copies of btn_play.png / btn_pause.png with the baked-in cyan
 * glyph rewritten to the current accent. Kept across widget teardown so
 * the quick-drawer button (destroyed later in gui_shell_teardown) never
 * holds a freed src pointer during reload. */
static asset_decoded_image_t play_btn_play_img;
static asset_decoded_image_t play_btn_pause_img;
static bool play_btn_glyph_on_light;

/* play_btn_glyph_on_light: the backdrop is too light for the stock white
 * glyph, which then takes the backdrop's text color. */
static void recolor_play_btn_accent(lv_draw_buf_t * buf, lv_color_t accent)
{
    lv_color_t glyph = lv_color_hex(GUI_BREEZE_TEXT_ON_LIGHT);
    if (play_btn_glyph_on_light) {
        lv_style_value_t value;
        if (lv_style_get_prop(gui_player_backdrop_text_style(false), LV_STYLE_TEXT_COLOR, &value) == LV_STYLE_RES_FOUND)
            glyph = value.color;
    }
    if (!buf || !buf->data || !buf->header.w || !buf->header.h ||
        buf->header.cf != LV_COLOR_FORMAT_ARGB8888) return;
    uint32_t w = buf->header.w;
    uint32_t h = buf->header.h;
    uint32_t stride = buf->header.stride;
    uint8_t ar = accent.red;
    uint8_t ag = accent.green;
    uint8_t ab = accent.blue;
    for (uint32_t y = 0; y < h; y++) {
        lv_color32_t * row = (lv_color32_t *) (buf->data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            if (row[x].alpha == 0) continue;
            /* Normalize to the image ellipse. The outer quarter of its
             * radius contains the ring, outside either center glyph.
             * Preserve alpha so antialiased edges remain smooth. */
            int32_t dx = ((int32_t) (2 * x + 1) - (int32_t) w) * 256 / (int32_t) w;
            int32_t dy = ((int32_t) (2 * y + 1) - (int32_t) h) * 256 / (int32_t) h;
            if (dx * dx + dy * dy >= 192 * 192) {
                row[x].red = ar;
                row[x].green = ag;
                row[x].blue = ab;
                continue;
            }
            uint8_t minc = row[x].red;
            if (row[x].green < minc) minc = row[x].green;
            if (row[x].blue < minc) minc = row[x].blue;
            uint8_t t = (uint8_t) (255 - minc);
            /* Disc interior is white / near-white; the glyph is the stock
             * cyan. Skip the disc so only the mark is retinted. The ring-only
             * art has a white glyph here instead, which a light backdrop
             * would swallow. */
            if (t < 16) {
                if (play_btn_glyph_on_light) {
                    row[x].red = glyph.red;
                    row[x].green = glyph.green;
                    row[x].blue = glyph.blue;
                }
                continue;
            }
            row[x].red   = (uint8_t) (255 - ((uint16_t) (255 - ar) * t) / 255);
            row[x].green = (uint8_t) (255 - ((uint16_t) (255 - ag) * t) / 255);
            row[x].blue  = (uint8_t) (255 - ((uint16_t) (255 - ab) * t) / 255);
        }
    }
}

void gui_player_prepare_play_btn_images(void)
{
    lv_color_t accent = accent_lv_color();
    if (asset_decoded_image_prepare_retint(&play_btn_play_img, "playing_plane/btn_play.png"))
        recolor_play_btn_accent((lv_draw_buf_t *) play_btn_play_img.decoder.decoded, accent);
    if (asset_decoded_image_prepare_retint(&play_btn_pause_img, "playing_plane/btn_pause.png"))
        recolor_play_btn_accent((lv_draw_buf_t *) play_btn_pause_img.decoder.decoded, accent);
}

const void * gui_player_play_btn_image_src(bool is_playing)
{
    const void * src = asset_decoded_image_source(is_playing ? &play_btn_pause_img : &play_btn_play_img);
    if (src) return src;
    return asset_path(is_playing ? "playing_plane/btn_pause.png" : "playing_plane/btn_play.png");
}

bool player_transport_images_set_glyph_on_light(bool value)
{
    if (value != play_btn_glyph_on_light) {
        play_btn_glyph_on_light = value;
        return true;
    }
    return false;
}

void player_transport_images_reset(void)
{
    asset_decoded_image_close(&play_btn_play_img);
    asset_decoded_image_close(&play_btn_pause_img);
}
