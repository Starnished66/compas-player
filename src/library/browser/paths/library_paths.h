#pragma once

#ifdef HOST_BUILD
#define MUSIC_ROOT_DIR "./music"
#else
#define MUSIC_ROOT_DIR "/data/mnt/sd_0"
#endif

#define SUBSONIC_STREAM_CACHE_DIR MUSIC_ROOT_DIR "/.subsonic_cache"

#define AUDIOBOOKS_LIBRARY_DIR_NAME "Audiobooks"
