#ifndef GUI_PLAY_THROUGH_H
#define GUI_PLAY_THROUGH_H

void gui_play_through_track_started(const char * artist, const char * album);
void gui_play_through_stopped(void);
void gui_play_through_resumed(void);
void gui_play_through_queue_exhausted(int direction);
void gui_play_through_deinit(void);

#endif
