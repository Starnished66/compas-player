# Remote Control API v1 — Android integration

The player keeps decoding and playing audio locally. A client sends control requests and reads metadata; no audio stream is sent to the phone. Remote Control must be enabled on the player for either transport to accept requests. The setting is session-only and is off again after a player restart.

## Connections and discovery

| Transport | Discovery and connection | Framing |
| --- | --- | --- |
| Wi-Fi | The app discovers `_compas-remote._tcp.local` with DNS-SD and reads the service address and port. The player also shows `http://<player-ip>:8899` and a QR code on **Wireless → Remote Control**. The phone and player must share a reachable network. | Ordinary HTTP/1.1. The service TXT record contains `api=1`. |
| Bluetooth Classic | Pair with the player, then discover the SDP service named **Compas Remote Control** with UUID `9d2f8a7e-6b4c-4d51-8e43-38f7a2c0b719`. On Android use `BluetoothDevice.createRfcommSocketToServiceRecord(UUID)` on a worker thread. | One HTTP/1.1 request and response per RFCOMM socket. Close the socket after reading the response. |

The Bluetooth connection carries **the same method, path, query string, status code, headers, and body** as Wi-Fi HTTP. It is a byte stream, so callers must not assume one read returns a whole response. Parse headers until `\r\n\r\n`, then read exactly `Content-Length` bytes. Send parameters in the URL query string even for `POST`. The only request body in v1 is the playlist file uploaded to `POST /playlists/import`, sent with `Content-Length` on either transport. The server ignores a body on any other route. For example:

```http
GET /api/v1/status HTTP/1.1
Host: compas
X-Compas-PIN: 123456
Connection: close

```

The RFCOMM UUID is the service identifier for discovery. The player publishes RFCOMM channel 21 for this UUID in its SDP record; clients must discover the channel from SDP and must not hardcode it. Do not use the generic Serial Port Profile UUID. The service is advertised only while Remote Control is enabled, Bluetooth is available, and the player's Remote Control screen reports the Bluetooth service as ready. Bluetooth pairing is required by the service. A paired phone may still need to reconnect after the player toggles Bluetooth or restarts.

The Wi-Fi DNS-SD advertisement is present only while Remote Control is enabled and `wlan0` is up with an IPv4 address. Its service type is `_compas-remote._tcp.local`, TCP port is `8899`, and TXT data is `api=1`. A client should resolve the returned SRV target and A address instead of relying on a fixed hostname. The advertisement is best-effort: networks that block multicast DNS can still use the displayed IP address.

## Common rules

- The Wi-Fi base URL is `http://<player-ip>:8899`. Prefix all paths below with `/api/v1`.
- Every HTTP response has `Content-Length` and `Connection: close`. JSON responses have `Content-Type: application/json`; cover art uses `image/jpeg`, `image/png`, or `image/bmp`. Control requests currently return `text/plain` (`OK` on success). `POST /screenshot` returns `OK` once the UI has been asked to start capture; the client does not receive the saved path or a later completion event.
- Every request except `GET /` and `GET /api/v1/capabilities` must include `X-Compas-PIN: <pin>`. This applies to Wi-Fi and HTTP carried over Bluetooth RFCOMM. The player generates a random 6-digit PIN the first time Remote Control needs one and keeps reusing it across boots and upgrades. The user sees it in **Wireless → Remote Control** and can replace it only with the refresh icon next to the PIN, which immediately invalidates the old PIN and clears any PIN lockout; clients must then ask for the new PIN. Players that still had the old shared placeholder PIN `0000` are moved to a random PIN once. Earlier user-chosen 4–12 digit PINs are kept, so clients must accept any 4–12 digit PIN. If the player cannot read its system random source it refuses to serve rather than fall back to a guessable PIN (`503`).
- Five distinct incorrect PIN guesses within a minute pause further PIN checks for one minute. Repeated requests using the same wrong PIN count once, so concurrent polls do not trigger a lockout. Missing PINs do not count as guesses. Incorrect PINs return `401`; while paused, requests return `429 Too Many Requests` with `Retry-After: 60` and `{"error":"rate_limited","code":"pin_rate_limited","retryAfterSeconds":60}`. Wait before asking for or retrying a PIN.
- `401 Unauthorized` returns JSON `{"error":"unauthorized","code":"pin_required"}`. Ask the user to enter the current player PIN, then retry with the header. Do not put the PIN in a URL, query string, or log.
- `GET /` serves the browser remote shell without authentication; its API and artwork requests use the same PIN header. The browser prompts for the PIN and stores it in that browser's local storage.
- JSON string fields can be empty. Song `index` values are persistent database IDs, not array positions. Treat them as signed 64-bit integers. Queue `offset` is a zero-based position in the current upcoming queue and may change between reads.
- Query values must be UTF-8 URL encoded. The server accepts `+` as a space and percent decoding for names and filters.
- Poll `/status` for playback changes. A one-second interval matches the web client. Requests are accepted asynchronously: `200 OK` means queued or applied, not that playback has finished changing; read `/status` again to observe the result.
- The server is a small, single-request-per-connection service. Bound concurrent polling and pagination; do not send request bodies or expect persistent connections, WebSockets, or push notifications.

## Discovery and playback state

| Method and path | Response |
| --- | --- |
| `GET /capabilities` | Public handshake: `{"version":1,"authRequired":true,"authHeader":"X-Compas-PIN","transports":["wifi","bluetooth_classic_rfcomm"],"features":["status","playback_controls","queue","library_browse","playlists","album_art","screenshots","catalog_sync_v1","folder_browse","recently_played","favorites_edit","queue_edit","playlist_edit","playlist_import"]}`. A transport is listed as supported, though its radio may currently be off. It exposes no playback or library data. Check a feature flag before calling the routes it covers; older players omit the newer flags. |
| `GET /status` | `playing` and `paused` booleans; `title`, `artist`, `album` strings; `position_seconds`, `duration_seconds` integers; `volume` number from 0 to 1; `play_mode` integer. Modes: `0` sequential, `1` repeat all, `2` repeat one, `3` shuffle. Also includes `codec` (for example `FLAC`), `source_bit_depth`, and `source_sample_rate` for a matching local track. `bluetooth_audio_output` is true when Bluetooth is the requested audio output route, and false for local or USB output. `favorite` is the player's heart state for the current track (false when nothing is playing). It is present on players that advertise `favorites_edit`. Unknown/unavailable source values are `""` and `0`. The source format fields describe the source file, not the player's output format. |
| `GET /art` | Artwork for the currently playing file, or `404` if unavailable. |
| `GET /art?index=<song-id>` | Artwork for a library song, or `404` if unavailable. Use the returned `Content-Type`; there is no fixed image size. |

The status producer includes the current track's album when metadata is available; `album` is empty when the file has no album tag. Use the title and artist for track display, and `/art` without an index to resolve the current file's artwork.

## Playback commands

All commands use `POST`. Successful commands return `200 OK` with text body `OK`.

| Path | Effect and parameters |
| --- | --- |
| `/playback/toggle` | Toggle play/pause. |
| `/playback/next` | Skip to the next song. |
| `/playback/prev` | Skip to the previous song. |
| `/screenshot` | Queue a screenshot of the visible display. The response confirms it was queued; the PNG is saved under `<SD card>/Screenshots/`. |
| `/playback/mode` | Cycle sequential → repeat all → repeat one → shuffle. Add `?mode=0`, `?mode=1`, `?mode=2`, or `?mode=3` to select sequential, repeat all, repeat one, or shuffle directly. |
| `/playback/seek?seconds=<n>` | Seek to a nonnegative whole-second position. |
| `/playback/volume?percent=<n>` | Set volume to integer 0–100. |
| `/playback/play?index=<song-id>` | Play the song. Catalog clients should add `catalog_revision=<revision>`; the player checks the revision and song ID before accepting the request and returns `409` if that catalog has gone stale. Optional context: `playlist=<key>`, or `artist=<name>&album=<name>`, or `album_artist=<name>&album=<name>`. Context controls the following playback queue. Without context, playback falls back to All Songs and rebuilds the queue; do not use this route to jump within the current upcoming queue. |
| `/playback/queue?index=<song-id>` | Add a library song to the upcoming queue. Catalog clients should also pass `catalog_revision=<revision>`; a stale catalog returns `409`. |

## Browse the library

| Method and path | Response |
| --- | --- |
| `GET /library?offset=0&limit=50&q=<text>` | `{"total":N,"songs":[{"index":ID,"title":"...","artist":"..."}]}`. `q` is optional. `limit` is capped at 100. Use `total` and the returned song count for pagination. |
| `GET /library?artist=<name>&album=<name>` | Same shape, filtered by artist and album. `album_artist=<name>` may replace `artist`. Filters may be combined with `offset` and `limit`. |
| `GET /library/artists` | `{"artists":[{"name":"...","count":N,"index":ID,"album_artist":"..."}]}`. |
| `GET /library/album_artists` | Same shape as `/library/artists`; the grouping is by album artist. |
| `GET /library/genres` | `{"genres":[{"name":"...","count":N,"index":ID,"album_artist":""}]}`. `index` is a representative song ID, suitable for requesting its artwork. |
| `GET /library/albums?artist=<name>` | `{"albums":[{"name":"...","count":N,"index":ID,"album_artist":"..."}]}`. `album_artist=<name>` is also accepted. |
| `GET /library?genre=<name>` | Same paged song shape, filtered by an exact case-insensitive genre name (up to 599 UTF-8 bytes). Combine with `q`, `artist`, `album_artist`, `album`, `offset`, and `limit`. Songs without a genre are not included in a genre group. |

The artist, album, and genre group lists are capped at 2,000 entries in v1. Genre groups are derived by a bounded streaming scan of song metadata and hold only distinct genre names and counts in memory. Use a library song ID to request its artwork or play it.

## Paged catalog and original artwork

Clients that advertise `catalog_sync_v1` can sync the complete song catalog and artwork associations into a local database. These endpoints are PIN protected. Page size defaults to 50 and is capped at 100. Every response carries a stable `library_id` UUID and an opaque `revision` captured with that page. Bind every later page to the first response's revision; a changed library returns `409` and the client should restart from offset 0. `total` is stable for all pages with the same revision. `next_offset` advances only by complete rows actually serialized.

| Method and path | Response or effect |
| --- | --- |
| `GET /catalog?offset=0&limit=50&revision=<optional>` | `{"library_id":"<uuid>","revision":"<uuid>:<generation>","total":N,"offset":0,"songs":[{"id":ID,"title":"...","artist":"...","artists":["..."],"album":"...","album_artist":"...","genre":"...","track_number":1,"disc_number":null,"album_key":"a2-<16 hex>"}],"next_offset":50}`. The first request may omit `revision`; the response always includes it. IDs are the persistent numeric song IDs accepted by `/playback/play`, `/playback/queue`, and legacy `/art?index=`. `artists` is the exact current artist-split membership list (empty when no artist is indexed). Missing/nonpositive track or disc numbers and songs without an album use JSON `null`. `album_artist` falls back to `artist` in this projection when no album-artist tag is stored. `album_key` is an opaque stable hash of the case-insensitive `(album, effective album_artist)` association, using tagcache's album grouping identity and is null when there is no album. |
| `GET /catalog/covers?offset=0&limit=50&revision=<revision>` | `{"library_id":"<uuid>","revision":"<uuid>:<generation>","total":M,"offset":0,"covers":[{"album_key":"a2-<16 hex>","representative_id":ID,"cover_key":"s-<16 hex>","available":true}],"next_offset":50}`. Cover rows use the same `a2-` album association key as song rows. The prefix identifies the shared grouping scheme; clients must treat the key as opaque. `available:true` means an original compressed sidecar was found; `available:null` means an embedded source has not been inspected. Keys are always non-empty; the server does not pre-decode embedded artwork during manifest paging. |
| `GET /catalog/art?revision=<revision>&representative_id=<song-id>&cover_key=<key>` | Serves the original encoded sidecar bytes first, otherwise extracts original embedded bytes in an isolated low-priority worker. `Content-Length` is fixed (no chunked transfer); `Content-Type` reports PNG, BMP, or JPEG when recognized. Original sidecars larger than 8 MiB and embedded artwork larger than the parser's safe 4 MiB extraction cap return `413`. `404` means the current source completed with no artwork. `503` means a temporary source, worker, invalid embedded image, or memory-admission failure. `409` means the revision, representative, or source fingerprint changed. `cover_key` is a source fingerprint derived from source path, modification time, and size; it does not include the catalog generation or representative ID. Pass the representative ID from the matching covers row. |

The revision is `<library_id>:<tagcache-generation>`. The UUID is persisted with the library and changes when the catalog is freshly created or recovered/rebuilt, so a reused numeric song ID cannot masquerade as the prior library. Cover keys are opaque and must not be parsed by clients. Keep artwork under the returned `library_id` namespace so Wi-Fi and Bluetooth connections to the same player share cached art.

## Queue and playlists

| Method and path | Response or effect |
| --- | --- |
| `GET /queue?offset=<n>&limit=<n>` | A page of upcoming songs in the player's displayed play order: `{"total":N,"offset":0,"revision":R,"songs":[{"offset":0,"index":ID,"title":"...","artist":"..."}]}`. Offset defaults to 0, limit defaults to 25 and is capped at 25. Song offsets are global across pages. `index` is the library song ID; `-1` means metadata could not be resolved and that row cannot be selected by song ID. The current track is excluded. |
| `POST /queue/remove?offset=<n>&revision=<R>` | Remove one upcoming song at the global zero-based offset. The revision must match the latest queue response or the request is ignored. |
| `POST /queue/clear?revision=<R>` | Clear the playback queue around the current track. The revision must match the latest queue response or the request is ignored. |
| `GET /playlists` | `{"playlists":[{"name":"Favorites","key":"@favorites","internal":true,"writable":false},...]}`. Built-ins are `@favorites`, `@most_played`, and, on players that advertise `recently_played`, `@recently_played`. Each playlist has a stable `key`; built-ins use reserved keys such as `@favorites`, and user playlists use the filename stem (for example, `name: "Gym.m3u"`, `key: "Gym"`). Keep displaying `name` and pass `key` to song browsing and playback context. User playlists also include `internal: false` and `writable: true`. Only direct children of the Playlists folder with a lowercase `.m3u` extension and a safe stem are listed, matching the files the API routes can address. |
| `GET /playlists/songs?name=<key>&offset=<n>` | `{"writable":true,"revision":"p-<16 hex>","songs":[{"index":ID,"title":"...","artist":"...","position":0}],"total":N,"next_offset":N}`. `offset` is optional (default 0). Built-in keys include `@favorites`, `@most_played`, and `@recently_played`; built-ins return `"writable":false,"revision":null`. Entries that no longer match a library song are skipped. `position` is the entry's offset in the playlist file, counting skipped entries, so positions can have gaps. A response holds as many songs as fit (about 64 KiB). `total` is the number of entries in the list and `next_offset` the entry position to continue from: while `next_offset < total`, request again with `offset=<next_offset>`. `writable`, `revision`, `position`, `total`, `next_offset`, and `offset` are additions; older players omit them. |
| `POST /playlists?name=<new-name>&index=<song-id>` | Create a writable playlist containing the song. A built-in key or alias as the name returns `403`. |
| `POST /playlists/add?name=<name>&index=<song-id>` | Add a song to an existing writable playlist. A built-in key returns `403` with text `Playlist is read-only`. |

Playlist names are a single path component: empty names, slash, backslash, and `..` are rejected. Playlist mutation may take longer than playback controls because it writes to the SD card. The playlist management routes below add stricter rules for new names.

## Extension routes

These routes follow the rules above: `/api/v1` prefix, PIN header, query-string parameters, and the same behavior over Wi-Fi and Bluetooth RFCOMM. Errors return JSON `{"error":"<family>_error","code":"<reason>"}`, where the family is `folder`, `recent`, `favorite`, `queue`, or `playlist`. Branch on `code`. Paged routes take `offset` (default 0) and `limit` (default 50, at most 100) as plain decimal digits. They return `next_offset`, which advances only past rows actually returned. Keep requesting while `next_offset < total`. Text values must be valid UTF-8 URL encoding; malformed `%` escapes and `%00` return `400`.

### Folder browsing (`folder_browse`)

The player has one storage root, the SD card, with id `sd`. Folder paths are relative to the root: `""` (or `/`) is the root, otherwise `/`-separated names. The path must not have an empty, `.`, `..`, or hidden (leading `.`) component, a trailing slash, a backslash, or control bytes. A child's path is the parent `path` + `/` + `name`, or just `name` at the root. `root` may be omitted and defaults to `sd`.

| Method and path | Response |
| --- | --- |
| `GET /folders/roots` | `{"roots":[{"id":"sd","name":"SD card","available":true}]}`. `available` is false while no card is mounted, using the same check the player's library uses. |
| `GET /folders?root=sd&path=<rel>&offset=0&limit=50` | `{"root":"sd","path":"Artist/Album","parent":"Artist","total":N,"folder_count":D,"offset":0,"entries":[{"type":"folder","name":"CD1"},{"type":"playlist","name":"mix.m3u"},{"type":"track","name":"01.flac","index":ID,"title":"...","artist":"..."}],"next_offset":3}`. Entries use the player's Files order: folders first, then files, each sorted by name ignoring case. Only folders, `.m3u`/`.m3u8` playlists, and playable audio files are listed; hidden entries and symlinks are left out. `parent` is `null` at the root and `""` for a top-level folder. `index` is the persistent library song ID used by `/playback/play`, `/playback/queue`, favorites, and `/art`. It is `-1` when the file is not in the library (not scanned yet, or in an ignored folder); then `title` is the file name and `artist` is empty. The listing is read from the card on each request. |
| `POST /folders/play?root=sd&path=<rel file>` | Plays the file with its folder as the queue, like tapping it in Files. Works for files without a song ID. Returns `OK` when accepted; the UI then builds the queue. If the folder can't be read at that point, the player shows an error and playback is unchanged. |

Errors: `400 invalid_path`, `400 invalid_paging`, `400 not_playable` (play target is not an audio file), `404 folder_not_found`, `404 track_not_found`, `503 storage_unavailable` (card not mounted), `503 folder_too_large` (a read-only folder over the Files view's 4,096-entry limit), `503 folder_unavailable`, `503 temporary_unavailable`.

### Recently played (`recently_played`)

| Method and path | Response |
| --- | --- |
| `GET /recent?offset=0&limit=50` | `{"total":N,"offset":0,"songs":[{"index":ID,"title":"...","artist":"...","last_played":1758844800}],"next_offset":50}`. Songs are ordered by last playback time, newest first; ties go to the lower song ID. `last_played` is the player clock's Unix time in seconds. |

The library stores only each song's latest play time, not a play log, so a song appears once however often it was played. Only library songs are listed; streams and other files outside the library are not. The list covers the newest 500 songs: `total` is at most 500, and offsets past it return no rows. If the player clock was wrong when a song played, its timestamp keeps that value. `GET /playlists/songs?name=@recently_played` returns the same order, and `/playback/play?index=<id>&playlist=@recently_played` plays with that list as the queue.

Errors: `400 invalid_paging`, `503 temporary_unavailable` (library not loaded).

### Favorites (`favorites_edit`)

Favorites are the library's per-song favorite flag, the same flag the player's heart sets. `@favorites` is built from that flag, so it updates with every change and needs no separate write.

| Method and path | Response or effect |
| --- | --- |
| `GET /favorites/state?ids=<id>,<id>,...` | `{"songs":[{"index":7,"found":true,"favorite":true},{"index":99,"found":false,"favorite":false}]}`. 1 to 100 comma-separated positive song IDs; rows follow request order. |
| `POST /favorites/add?index=<song-id>` | `{"index":ID,"favorite":true}`. Optional `catalog_revision=<revision>` checks the ID against a synced catalog, as `/playback/play` does. |
| `POST /favorites/remove?index=<song-id>` | `{"index":ID,"favorite":false}`. Same parameters as add. |
| `POST /favorites/add?current=1`, `POST /favorites/remove?current=1` | Changes the current track, which `/status` does not identify by ID. `index` is `-1` when the current file is not a library song; the flag still persists but `@favorites` lists only library songs. |

A `200` means the change is on the card: the response waits for the write to finish. Remote changes and the player's own heart taps go through one ordered writer. A remote change replaces a heart tap that has not been saved yet, and waits for one that is being saved, so the later action wins. If the player's heart is tapped again for the same song while a remote change is still waiting, that newer tap wins: the remote change is not written and still returns `200`. `/status.favorite` then shows the final state. After any favorite change the player refreshes its heart; `/status.favorite` shows it on the next poll, or immediately when the changed song is the current one.

Errors: `400 invalid_target` (send exactly one of `index` or `current=1`), `400 invalid_index`, `400 invalid_ids`, `404 song_not_found`, `404 nothing_playing`, `409 revision_mismatch` (stale `catalog_revision`), `503 write_failed` (the flag did not persist), `503 temporary_unavailable`.

### Queue editing (`queue_edit`)

Offsets are Up Next positions from `GET /queue` (current track excluded). `revision` is that response's `revision`.

| Method and path | Effect |
| --- | --- |
| `POST /queue/move?from=<offset>&to=<offset>&revision=<R>` | Moves one upcoming song so it ends up at offset `to`. `from == to` is accepted and changes nothing. |
| `POST /queue/play?offset=<offset>&revision=<R>` | Starts playing the upcoming song at `offset` without replacing the queue. Songs before it move into played history, as when skipping ahead. |

Both return `OK` when accepted and apply on the next UI tick. The revision changes after every queue change, so read `/queue` again before the next edit. The UI checks the revision again before applying; if the queue changed in between, the edit is dropped. Only one queue edit can be pending per revision.

Errors: `400 invalid_request` (missing or non-numeric parameter), `400 invalid_offset` (past the end of Up Next), `409 revision_mismatch`, `409 edit_pending` (an earlier edit has not been applied yet; reread `/queue`). The original `/queue/remove` and `/queue/clear` keep their text responses.

### Playlist management (`playlist_edit`, `playlist_import`)

User playlists are `<key>.m3u` files in the SD card's Playlists folder. Built-ins (`@favorites`, `@most_played`, `@recently_played`, and the aliases `Favorites` and `Most Played`) are read-only: every mutation returns `403 read_only`. A new or renamed name is a file stem without an extension: 1 to 127 bytes, no `/ \ : * ? " < > |` or control bytes, no leading `.`, no trailing space or `.`, no `..`, and not ending in `.m3u`/`.m3u8`. Names starting with `@` and the built-in aliases are refused with `reserved_name`. The card's file system ignores case, so a name that differs from an existing playlist only in case counts as existing.

Edits by position use the `revision` from `GET /playlists/songs`: an opaque hash of the playlist's entries. It changes on any entry edit, including a reorder, and a stale revision returns `409`. The revision is checked under the same lock as the write, so an edit on the device or over the other transport cannot slip in between. Successful mutations return `{"key":"Gym","name":"Gym.m3u","revision":"p-<16 hex>"}` so the next edit can go straight on; `revision` is `null` in the rare case the file could not be reread after the change, and the client should then reload the playlist. The original create-with-song and add-song routes are unchanged.

| Method and path | Effect |
| --- | --- |
| `POST /playlists/create?name=<name>` (`playlist_edit`) | Creates an empty playlist. |
| `POST /playlists/rename?name=<key>&new_name=<name>` (`playlist_edit`) | Renames the file; the response carries the new key. |
| `POST /playlists/delete?name=<key>` (`playlist_edit`) | Deletes the playlist: `{"key":"Gym","deleted":true}`. |
| `POST /playlists/remove?name=<key>&position=<n>&revision=<R>` (`playlist_edit`) | Removes the entry at that position. |
| `POST /playlists/move?name=<key>&from=<n>&to=<n>&revision=<R>` (`playlist_edit`) | Moves an entry so it ends up at position `to`. |
| `POST /playlists/import?name=<name>` with the playlist file as the body (`playlist_import`) | Stores an `.m3u` or `.m3u8` file from the phone as `<name>.m3u`. Send `Content-Length`; the body is at most 256 KiB and must not contain NUL bytes. The bytes are stored unchanged, including `#EXTINF` lines and the entry paths as written. Relative entries resolve from the Playlists folder, like any playlist there. Response: `{"key":"Road","name":"Road.m3u","revision":"...","entries":N,"matched":M}`. `entries` counts the entries and `matched` those that resolve to library songs on this player. Entries that point at phone storage will not match. The playlist is added to the player's Playlists list and appears in `GET /playlists`. |

Errors: `400 invalid_name`, `400 reserved_name`, `400 invalid_request` (missing or non-numeric position or revision), `400 invalid_position`, `400 invalid_playlist` (NUL bytes, or an entry longer than the player's path limit), `400 incomplete_body` (the body ended or stalled for 2 seconds before `Content-Length` bytes), `403 read_only`, `404 playlist_not_found`, `409 name_exists`, `409 revision_mismatch`, `411 length_required`, `413 playlist_too_large`, `500 read_failed` (a same-position move could not read the playlist), `500 write_failed`.

When an import is refused before its body has been read, the player sends the error, stops sending, and reads the rest of the body (up to 8 MiB or 10 seconds) before closing. This keeps the error from being lost to a connection reset. Clients should still read the response after sending the body.

## Errors and compatibility

`400` means a malformed or unsafe request, `401` means the PIN is missing or incorrect, `403` means a built-in playlist cannot be changed, `429` means PIN checks are temporarily rate-limited, `404` means a missing route or resource, `405` means an unsupported method, `409` means a queue, catalog, or playlist revision is stale, or the name is taken, `411` means an import has no `Content-Length`, `413` means sidecar artwork exceeds the 8 MiB sync limit, embedded artwork exceeds the 4 MiB extraction cap, or an imported playlist exceeds 256 KiB, `500` means an internal failure, and `503` means a temporary server/resource failure. Unauthorized and rate-limit responses use the JSON bodies documented above; catalog errors use `{"error":"catalog_error","code":"..."}` and the extension routes use the same shape with their own family. Artwork absence is a normal `404`; retry `503` and restart the catalog sync after `409`.

The original unversioned `/api/...` paths remain available for the existing web remote and require the same PIN. New clients should use `/api/v1/...`. The web page itself is served at `/` over Wi-Fi; the Android app should consume the API directly. Bluetooth uses the bonded link in addition to the PIN. The PIN is stored in the player's settings file, whose mode is restricted to owner read/write. This is a control interface, not an audio output profile.

Wi-Fi uses plain HTTP: the PIN and response data are not encrypted in transit. Use it on a trusted local network. Bluetooth adds the paired link's transport security.

## Validation status

The Wi-Fi and Bluetooth code paths compile and link into the R1 target build. Bluetooth pairing, SDP discovery, RFCOMM requests, coexistence with A2DP playback, and R3 hardware behavior still require validation on physical devices before treating this contract as field-proven. The extension routes are covered by host tests (`make remote-control-api-selftest`, `make playlist-selftest`, `make metadata-catalog-selftest`) and build for all three boards, but have not yet been exercised against the Compás Remote app on hardware.
