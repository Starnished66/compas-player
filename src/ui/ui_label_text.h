#ifndef COMPAS_UI_LABEL_TEXT_H
#define COMPAS_UI_LABEL_TEXT_H

#include "lvgl/lvgl.h"
#include "lvgl/src/misc/lv_text_ap.h"
#include <string.h>

/* LVGL stores presentation forms in display labels. Compare against raw
 * metadata without resetting an unchanged label's marquee or snapshot. */
static inline bool ui_label_text_matches(const lv_obj_t * label, const char * text) {
    if (!label) return false;
    if (!text) text = "";
    const char * current = lv_label_get_text(label);
    if (!current) return false;
    if (strcmp(current, text) == 0) return true;
#if LV_USE_ARABIC_PERSIAN_CHARS
    const unsigned char * p = (const unsigned char *) text;
    while (*p && *p < 0x80) ++p;
    if (!*p) return false;
    uint32_t size = lv_text_ap_calc_bytes_count(text);
    if (!size) return false;
    char local[256];
    char * shaped = size <= sizeof(local) ? local : lv_malloc(size);
    if (!shaped) return false;
    shaped[0] = '\0';
    lv_text_ap_proc(text, shaped);
    bool matches = shaped[0] != '\0' && strcmp(current, shaped) == 0;
    if (shaped != local) lv_free(shaped);
    return matches;
#else
    return false;
#endif
}

#endif
