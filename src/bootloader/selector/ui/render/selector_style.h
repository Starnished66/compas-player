#ifndef BOOTLOADER_SELECTOR_STYLE_H
#define BOOTLOADER_SELECTOR_STYLE_H

/* Plasma 6 Breeze Dark tokens from /usr/share/color-schemes/BreezeDark.colors:
 *   Window BackgroundNormal      32,35,38   (#202326)
 *   Window BackgroundAlternate   41,44,48   (#292c30)
 *   Window ForegroundNormal     252,252,252 (#fcfcfc)
 *   Window ForegroundInactive   161,169,177 (#a1a9b1)
 *   Selection BackgroundNormal   61,174,233 (#3daee9)
 *
 * Card border and inactive switch track are not named ColorGroup roles.
 * They mix Window BackgroundNormal toward ForegroundNormal at 12% and 22%.
 * Breeze selection colour is used on purpose; the player accent setting is
 * ignored. */

#define BREEZE_WINDOW_NORMAL      0x202326u
#define BREEZE_WINDOW_ALTERNATE   0x292C30u
#define BREEZE_WINDOW_FOREGROUND  0xFCFCFCu
#define BREEZE_WINDOW_INACTIVE    0xA1A9B1u
#define BREEZE_SELECTION_NORMAL   0x3DAEE9u
#define BREEZE_DECORATION_FOCUS   0x3DAEE9u
#define BREEZE_CARD_BORDER        0x3A3D40u /* 12% mix window → foreground */
#define BREEZE_BUTTON_INACTIVE    0x505355u /* 22% mix window → foreground */

#define BREEZE_CORNER_RADIUS 5
#define BREEZE_FOCUS_WIDTH   2
#define BREEZE_LARGE_SPACING 8
#define BREEZE_LIST_NOTICE_BAND 40

static inline int selector_is_compact(int width) { return width < 400; }
static inline int selector_margin(int width) { return width >= 400 ? 24 : 16; }
static inline int selector_header_title_y(int width) { return width >= 400 ? 38 : 24; }
static inline int selector_header_title_px(int width) { return width >= 400 ? 28 : 24; }
static inline int selector_header_sub_y(int width) { return width >= 400 ? 80 : 64; }
static inline int selector_list_top(int width) { return width >= 400 ? 142 : 120; }
static inline int selector_card_h(int width) { return width >= 400 ? 112 : 90; }
static inline int selector_gap(int width) { return width >= 400 ? 14 : 10; }
static inline int selector_logo_inset(int width) { return width >= 400 ? 22 : 16; }
static inline int selector_row_icon(int width) { return width >= 400 ? 40 : 32; }
static inline int selector_row_title_px(int width) { return width >= 400 ? 22 : 20; }
static inline int selector_row_sub_px(int width) { return width >= 400 ? 16 : 14; }
static inline int selector_tool_list_top(int width) { return width >= 400 ? 128 : 68; }
static inline int selector_tool_row_h(int width) { return width >= 400 ? 76 : 52; }
static inline int selector_tool_gap(int width) { return width >= 400 ? 10 : 6; }
static inline int selector_back_icon(int width) { (void)width; return 32; }
static inline int selector_next_icon(void) { return 22; }
static inline int selector_switch_w(int width) { return width >= 400 ? 52 : 46; }
static inline int selector_switch_h(int width) { return width >= 400 ? 32 : 28; }
static inline int selector_footer_y(int height) { return height - 34; }

#endif /* BOOTLOADER_SELECTOR_STYLE_H */
