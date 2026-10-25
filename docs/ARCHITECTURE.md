# Source architecture and modularization

## Goal and current status

Keep each module focused on one coherent responsibility, its state, and its
resource lifetime. File length is a signal to inspect a boundary, not a mandatory
limit. Do not hide complexity in include fragments, shared mutable globals, or
large callback tables that merely reproduce a coordinator in another file.

The project-wide migration is **in progress**. The original size-based inventory
recorded 96 maintained files and 33 vendored or generated/data files above its
500-line reporting threshold. Those historical numbers include tests and scripts,
not just application C. The large player, library, audio and plugin coordinators
still need staged refactoring; file moves alone do not make them maintainable.

Generated catalogs, embedded data and upstream libraries are reported separately.
Their source generators or upstream update processes should remain authoritative;
changing their physical representation requires a separate decision from splitting
maintained application logic.

## Dependency and ownership rules

- A dedicated header describes a module's operations, data contracts and lifetime.
  Include only the types needed by that interface. Keep implementation dependencies
  in the `.c` file.
- Keep state private to the module that creates, mutates and destroys it. Do not
  move a monolith's globals into a common header to make extraction compile.
- A feature coordinator assembles its components. Components must not depend on
  that coordinator's private state. Pass explicit values, immutable snapshots or
  bounded callbacks when crossing an ownership boundary.
- Separate data processing from LVGL widgets, filesystem transactions and worker
  orchestration. Worker code must not retain transient widget pointers.
- Preserve the existing lock, cancellation, playback-generation and buffer-lifetime
  contracts while moving code. Redesign behavior in a separately reviewed change.
- Production modules compile independently. Do not split them with `.c` includes
  or `.inc` fragments. Avoid cyclic headers and generic `utils` collections.
- A compatibility header may re-export a focused header while existing callers
  migrate. It must not duplicate declarations or introduce a dependency cycle.
- Shared definitions belong at the lowest layer that owns their meaning. Do not
  create a general-purpose framework for a single caller.

These are the target boundaries. Existing code that violates them remains visible
in the migration inventory; it is not automatically compliant after a file move.

## Finding the owner of a change

Start with the feature owner below rather than adding another helper to a large
coordinator. Headers describe the operations; state stays in the implementation.
These entries describe completed extractions, not a claim that the coordinators
have no remaining unrelated responsibilities.

| Concern | Owner | Contract |
| --- | --- | --- |
| Plugin asynchronous HTTP | `plugins/bindings/plugin_http_bindings` | Owns request/download workers, callback references and cancellation; manager supplies guarded callbacks and clears jobs before closing Lua. |
| Shared Lua main-state lookup | `plugins/plugin_lua_runtime` | Canonical main-state and Lua error conversion helpers; no plugin instance registry. |
| Player artwork jobs | `ui/player/player_artwork_jobs` | Owns worker mailboxes and generation/cancel state; GUI owns visible buffers and applies results. |
| Reflection artwork cache | `ui/player/player_artwork_cache` | Owns cache mutex/epoch; borrowed input is not retained and stale jobs cannot publish. |
| Library scan execution | `library/library_scan` | Owns scan targets, scope, cancellation/progress and outcomes; UI owns thread orchestration, busy presentation and notifications. |
| Queue persistence jobs | `ui/player/player_queue_storage` | Owns immutable checkpoint/save inputs, pinned descriptors and worker completion/cancellation; the player decides scheduling, queue contents and publication policy. |
| Live player queue | `ui/player/player_queue_model` | UI-thread owner of paths, lazy lookup references, the pinned SD directory, shuffle bags, pending tracks and revisions. The player coordinator retains audio dispatch, metadata, source identity and notifications. Play-mode values come from `core/play_mode.h`. |
| Plugin text reader callbacks | `plugins/bindings/plugin_text_view_bindings` | Owns callback refs and delayed-close slots; the manager supplies guarded calls and frozen-owner checks before Lua access. |
| Folder actions | `ui/gui_folder_actions` | Owns menu, index worker and completion transaction; workers receive copied paths and tokens, never live picker state. |
| Saved playlists and picker | `ui/gui_playlists` | Owns playlist widgets, dialogs and CRUD; group/queue drill-down stays in the library coordinator through synchronous borrowed inputs. |
| Add-to-playlist intent | `ui/gui_playlist_target` | One UI-thread owner for target paths and generation; callers keep validity decisions at the original point before UI effects. |
| Native wide list rows | `ui/screen_builders` | Shared row geometry for Playlists and CUE; callers specify whether navigation needs a chevron. |
| System Power Hub | `ui/gui_power_hub` | UI-thread owner of the modal, motion snapshot, timer and blur pixels. Build before show; teardown cancels motion before releasing buffers. |
| Seek gestures and held buttons | `ui/player/player_seek_input` | Owns seek targets, generation guards, long-press suppression and slider debounce. Audio remains the playback owner; the legacy `user_seeking` symbol is retained for compatibility. |
| Plugin equalizer operations | `plugins/bindings/plugin_equalizer_bindings` | Validates and marshals Lua EQ state; the plugin manager retains its single API guard and path policy. |
| Inline lyrics and cover morphs | `ui/player/player_inline_lyrics` | Borrows the current screen's widgets; owns transition resources and releases them before the view is replaced. Artwork decoding remains separate. |
| Selecting visible library metadata | `ui/library_selection_snapshot` | Copies row metadata into one bounded, locked cache; a busy or stale lookup clears the pending player seed without blocking. |
| Player-size album artwork decoding | `library/library_album_art_decode` | Owns decoding/cache-file operations; returns caller-owned pixels rather than retaining UI widgets. |
| Artist artwork resolution | `library/library_artist_art` | Resolves source/alias/negative-cache paths; no screen state. |
| Library list thumbnails | `ui/library/library_thumbnails` | Owns decoded pixels, artist aliases, decode workers, result handoffs and borrowed list bindings. The library coordinator retains row geometry, scan policy and busy-screen presentation; workers never access live widgets. |
| Library search | `ui/library/library_search` | Owns filtering, result labels, request coalescing, debounce and search/T9 view lifetimes. The library coordinator supplies copied registration inputs and UI callbacks, and retains sorting, playback and editing policy. |
| A-Z library index | `ui/library/library_az_index` | Owns the four-screen strip/popup registry, scroll visibility and raw-touch drag timers; the library coordinator supplies pointer lookup and list-specific availability policy, and retains sort policy. |
| Artwork tag identity | `library/albumart_info` | One shared conversion from track/database metadata; no duplicate Player and Library implementations. |
| Read-only plugin library queries | `plugins/bindings/plugin_library_bindings` | Converts native query results to Lua values; plugin instances, guards and callback lifetimes remain in the manager. |
| Plugin information and capabilities | `plugins/bindings/plugin_info_bindings` | Stateless Lua bindings; one guarded registry in the manager. |
| Compact virtualized lists | `ui/compact_list` | Owns recycled rows, page requests and provider-job lifetime together. |
| Display timeout and dimming settings | `ui/gui_display_timing_settings` | Owns the settings view and its controls; runtime display policy stays in the hardware/shell services. |
| Clock and timezone settings | `ui/gui_time_settings` | Owns clock/timezone views and temporary selection state. |
| Button mapping settings | `ui/gui_button_mapping` | Owns binding controls; runtime input dispatch stays outside the view. |
| Volume and car-mode settings | `ui/gui_volume_settings` | Owns related settings views, not the audio output backend. |
| Home/menu shell customization | `ui/gui_shell_settings` | Owns order and visibility settings; uses the shared order model. |
| Isolated metadata reads | `library/metadata_isolation` | Owns subprocess creation, bounded result transfer, timeout and reaping as one lifecycle. |

For a new feature, first decide which existing owner should expose the operation.
Do not reach into its static state from a coordinator, or create a second registry,
mutex or cache to avoid defining the interface.

## First migration group

| Module | Owns | Interface and dependencies |
| --- | --- | --- |
| `theme_file` | File parsing, candidate state, final validation and error cleanup | Public `theme_file.h`; private lexical interface |
| `theme_file_values` | Scalar conversion and lexical/path/identifier validation | Private `theme_file_values.h`; no widgets or parser state |
| `http_url` | Splitting the existing HTTP URL format into caller-owned buffers | `http_url.h`; no sockets, TLS or cancellation state |
| `http_conn` | DNS/connect/TLS lifecycle and connection reads/writes | Existing connection API; re-exports URL API during migration |
| `artwork_memory` | Memory availability cache, decode estimates and admission reserves | `artwork_memory.h` and priority enum |
| `artwork_failure_cache` | Failure/backoff entries and their mutex | `artwork_failure_cache.h`; no scheduling or UI dependency |
| `artwork_coordinator` | Exclusive decode slot and priority waiters | Scheduling API; consults memory admission |

The artwork modules share only a private monotonic-clock helper and public value
types. Each mutex and the state it protects stay in the same implementation file.
The public coordinator header temporarily re-exports the focused memory and
failure headers so existing consumers continue to compile.

First-group measurements from the working tree:

| Original file | Before | After | Extracted implementations |
| --- | ---: | ---: | --- |
| `theme_file.c` | 628 | 472 | `theme_file_values.c`: 161 |
| `http_conn.c` | 506 | 460 | `http_url.c`: 50 |
| `artwork_coordinator.c` | 510 | 126 | `artwork_memory.c`: 281; `artwork_failure_cache.c`: 103 |

At the first-group checkpoint, 93 maintained files remained above the reporting
threshold. The first group passed the R1
cross-build and targeted parser, HTTP range/TLS, artwork failure-cache, prefetch
and thumbnail checks. The broader theme-service fixture still fails its existing
`pull_down/wifi.png` legacy-exclusion assertion on both baseline and refactored
code. Linking the real UI font descriptors lets that fixture reach the assertion;
its expected theme behavior is a separate issue.

The player already has separate built-in layout, volume popup, transport image,
pixel processing, seekbar, cover fade, lyrics backdrop and timeline-label modules.
They are starting points, not substitutes for isolating the remaining large
player responsibilities.

## Remaining migration sequence

Each step needs a symbol/dependency audit before implementation. The names below
describe ownership boundaries, not permission to split by arbitrary line ranges.
Large domains will need multiple focused files under the same feature directory.

1. **Player model and persistence:** separate queue operations, ordering, checkpoint
   writing and restore policy. Queue UI remains a consumer of the model. Isolate
   metadata/favorite requests and generation checks from widget installation.
2. **Player presentation:** isolate layout binding, lyrics-panel transitions and
   artwork publication behind explicit bind/unbind operations. The screen
   coordinator controls teardown order. Borrowed pixels and snapshot buffers must
   have documented owners before code moves.
3. **Library:** separate scan workers/publication, SD lifecycle, search, collection
   browsing, thumbnail work and prompts. UI polling receives worker results through
   immutable results rather than sharing widget pointers with workers.
4. **Settings:** separate screen families for audio, appearance, library, system,
   and customization ordering. Share existing row/reorder primitives rather than
   exposing screen globals or copying builders.
5. **Plugin runtime:** separate lifecycle/event dispatch, Lua API families, resource
   ownership and service bridges. Keep API registration in one composition root;
   API implementations should not access a shared dump of runtime internals.
6. **Audio:** separate playback command/state coordination, decoder selection,
   output setup and next-track preparation. Keep each lock/state-machine owner
   intact until targeted cancellation and transition tests cover its new interface.
7. **Networking and stores:** separate catalog parsing, transport, installation
   transactions and request lifecycle. Path validation and filesystem operations
   need their existing traversal/symlink guarantees preserved.
8. **Hardware and boot:** separate protocol/input parsing from device lifecycle and
   persistent transactions. Bootloader and firmware source manifests must include
   every new translation unit; application builds alone are insufficient.
9. **Remaining core parsers and utilities:** split HTML entities/tokenization,
   archive directory/entry operations, settings serialization and async persistence
   according to their data ownership. Avoid exported generic helper collections.
10. **Tests and scripts:** group fixtures by behavior and separate reusable test
    support from cases. Split long scripts by a real command boundary. Preserve
    the same assertions and test discovery rather than reducing coverage.

## Verification and progress

Run the inventory from the repository root:

```sh
python3 scripts/check_module_sizes.py --json
python3 scripts/check_module_sizes.py --check
python3 scripts/check_module_sizes.py --check --include-external
python3 -m unittest scripts.tests.test_check_module_sizes -v
```

The report includes all classifications. Its legacy `--check` mode flags any
maintained source above 500 lines; use that as an inspection list rather than
an architectural acceptance criterion. `--include-external` also flags generated
and vendor files, which remain outside this refactoring scope. Do not split
cohesive modules or conceal remaining debt just to make the size check pass.

For each extraction:

1. Preserve the current working-tree baseline, including unrelated uncommitted work.
2. Review exact function groups, call sites, resource ownership and header graph.
3. Compare moved function bodies and call order with that baseline. Compile each
   new module independently and update standalone test source lists.
4. Run the targeted behavioral tests and the target cross-build. Use trace builds
   when a moved boundary contains trace-sensitive code; build flags are not
   reliably tracked by the current Makefile.
5. Obtain the independent C/header review required by `AGENTS.md`, resolve findings
   and review the fixes. Keep device verification distinct from host verification.
6. Record the resulting owners, interfaces, remaining coupling and file sizes.
   Review large files again when they still combine independent responsibilities.

A file move is not a reduction in total production lines. Report central-file
size, resulting module sizes and any actual net deletion separately. Test code
is not counted as production-code reduction.
