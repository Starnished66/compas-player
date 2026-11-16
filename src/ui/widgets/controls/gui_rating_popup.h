#ifndef GUI_RATING_POPUP_H
#define GUI_RATING_POPUP_H

typedef void (*gui_rating_popup_cb_t)(int stars, void * user_data);
void gui_rating_popup_open(const char * title, int stars, gui_rating_popup_cb_t cb, void * user_data);
void gui_rating_popup_load_stars(const char *key);
void gui_rating_popup_teardown(void);
#endif
