# UI wake and artwork reuse prototypes

These experiments preserve the normal firmware behavior by default. Enable them
independently in the player's environment when running a persistent SD-card test
build:

- `COMPAS_EVENT_UI=1`: poll the LVGL-owned touch descriptor and a nonblocking
  worker notification pipe instead of sleeping through input. LVGL's event mode
  keeps its read timer active during a press for long presses and scrolling.
  Hardware button processing runs on the UI thread; its 16 ms timer runs only
  while a configured double-tap remains pending. The 25 ms wake timer runs only
  during screen wake. Normal status, plugin, animation and power deadlines stay
  active. Cover completion also wakes the UI. Pipe creation failure falls back
  to periodic input handling.
- `COMPAS_ARTWORK_REUSE=1`: retain at most two decoded generated player covers
  in a mutex-protected LRU, capped at 1 MiB. Returning to a recently viewed album
  copies RGB565 pixels instead of reading and decoding the BMP again. The key
  includes file identity, size, modification/change timestamps and target size.
  A file changed during decode is not retained. Below the existing 8 MiB optional-artwork reserve plus the 1 MiB cache
  budget, lookup and UI maintenance clear the cache and uses the existing decode path. Widgets never
  borrow cache storage, and stream artwork is excluded.

Run `scripts/test_ui_experiments.sh` for notification latency/flood handling,
input ownership, raster ownership, invalidation, eviction and budget checks.
Build with `make target BOARD=r1`. Device tests must use persistent SD storage,
back up settings/queues, and restore the installed player after each comparison.
Compare the same build with flags off/on, measuring the main thread's CPU ticks,
context switches and RSS with the screen off; verify wake, taps, swipe, holds,
double taps, cover changes and return-to-album reuse. A short idle test does not
establish a battery-life gain. Do not make either prototype the default until
representative playback and memory measurements justify it.
