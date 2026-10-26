# Player release audit — 2026-10-25

This is an evidence ledger, not a release approval. A passing host test does
not establish correct behavior on the R1 or with an external accessory.

## Current outcome

The R1 now runs non-trace build **`2f8bf7ea749c`** after a verified SD-backed
firmware update; see the firmware follow-up at the end. At the end of the
preceding non-peripheral QA phase, build **`d3f33c59c7e2`** ran from persistent storage. At that checkpoint, Wi-Fi
and Remote Control were enabled; `/status` responds; both original queue files
are byte-identical. No emulator child or test loop mount remains, owned SD
fixtures/logs/binaries were removed, and the player stayed alive throughout
post-install checks with zero crash-log markers.

Release fixes cover upload timeout/retry, live catalog revision, remembered
Bluetooth rates, theme exclusion lifetime, VFAT playlist publication, settings
write failure atomicity, stale game-input replay and plugin pagination. The
new C changes received actual **Opus 5.5** review. Player/refactor
changes are included in the owner-requested main integration checkpoint; plugin commits are pushed, most recently
**`1bdf970`**, with no release tags or publication.

**No overall release sign-off:** PrBoom has confirmed recoverable audio
underruns; USB DAC/DSD/DoP and Bluetooth accessory tests await the final
hardware phase. Physical updater recovery/power loss, live-card removal,
bright-panel flicker and remaining positive provider/hardware effects are not
cleared by mocks or screenshots. Real Jellyfin/Subsonic authenticated browsing
passed; their streaming/queue/listening scenarios remain distinct. Tidal and
Qobuz real-service checks are regionally unavailable. Detailed limits and
issue deferrals appear below.

## Baseline and constraints

- Android 0.4.0 is approved separately at `8051ad0`.
- Player workspace starts at `a0d603e4d`, with the existing uncommitted
  modularization and feature work preserved. GitHub main is `705c81071`.
- R1 serial: `ingenic_2233`; installed baseline: `96dba079c57e`.
- Plugin API remains 17. Device fixtures and installs use persistent SD
  storage. Preserve the queue and leave Wi-Fi and Remote Control enabled.
- Plugin changes may be committed and pushed, but not published or tagged.
- Raw evidence is kept outside Git under
  `~/.cache/compas-theme-review/release-qa-20261025/`. Credentials and tokens
  must not appear in reports or test output.

## Stage 1 — every open issue and PR

| Item | Release decision / current evidence | Outstanding verification |
| --- | --- | --- |
| PR #172: replacing plugin lists loses scroll | Merged as `fcbe31f18` after actual Opus 5.5 approval | Production-function baseline/new real-LVGL tests pass for lists, grids, shorter replacements, selection, covered-screen liveness and fresh positioning. Device check remains in stage 2 |
| #188: native DSD / DoP USB output | Implemented in the pending workspace; output, route and segmented-decoder host tests pass | Real compatible DAC, fallback and reconnect tests remain pending; not release-cleared |
| #183: AirPods reconnect | Investigate; confirmed app policy arms reconnect only on adapter power-up | Reproduce real disconnect and distinguish manual-disconnect suppression from unexpected loss; finite retry/cancellation tests before changing policy |
| #176: high-resolution LDAC clipping | Existing mitigation is marked Testing Fix; no new clipping cause established by source review | Sustained 48/96/192 kHz playback, ABR vs forced HQ, transport rates, CPU/underruns and listening with affected hardware |
| #133: firmware update hangs | Safe host image/scan/staging regressions pass against production functions | Physical updater/reboot still pending. Covered valid image, chunk-hash corruption, truncation, missing SD/file, symlink/directory/wrong root/filename, and changed-image snapshot rejection; no NAND or reboot invoked |
| #194: remote Plugin Store, management and layouts | Newly reviewed; defer this API expansion to a separately scoped change | Requires PIN-protected catalog-id commands, confirmed local-file replacement, UI-thread/lifetime coordination, asynchronous progress and playback-continuity tests; no release-ready implementation or PR |
| #192: batch cover/lyrics fetching | Defer feature expansion while validating release stability | Needs bounded scan, progress/cancel, incremental persistence, missing-result policy and plugin-specific tests |
| #174: Spotify Connect | Retain owner-approved deferral | No supported implementation ready for this release |
| #89: AVRCP shuffle/repeat and extras | Retain owner-approved deferral until Bluetooth reliability is established | Accessory interoperability, advertised capabilities and metadata support |

## Stage 2 — player coverage matrix

Status terms: **pending**, **passed host**, **passed device**, **failed**, or
**blocked**. Each recorded result must name the exact command/build or replay.

| Area | Required scenarios | Status |
| --- | --- | --- |
| Remote uploads | Interrupted/half-open sockets, fast retry, slow progress, cancellation, disconnect, full/read-only/removed SD, partial-file cleanup | Passed host and native candidate checks: 12-second precise idle deadline, slow local I/O, cancellation, EOF/ENOSPC cleanup and retry. Native SD-backed FAT loop full and read-only checks returned HTTP 400, cleaned partials, preserved prior data, then retried with HTTP 201. Physical removal of the live card was not performed. Baseline R1 socket stall reproduced at 60.07 s; candidate closed at 12.035 s and a following upload returned 201 |
| Remote status/catalog | Current revision, generation/order changes, unavailable/rebuilding library, stale pages, thread safety and API compatibility | Passed host: additive nullable revision, escaped output, lock ordering, empty/unavailable catalog and revision updates. Native `/status.catalog_revision` matched `/catalog.revision` on the live library; live add/remove scans passed: 2,421 → 2,422 → 2,421 songs with matching status/catalog revisions and unchanged queue files |
| Library/files/playlists/queue | Empty/large libraries, tags/Unicode, sorting/paging, edit/delete/reorder, long queues, persistence and cancellation | Passed host characterization/fault suites; passed native catalog/import pagination and 2,421-item queue read. Full physical UI scenario sweep pending |
| Playback and DSP | Supported codecs and sources, gapless/manual transitions, seeking, speed, repeat/shuffle, volume, EQ/MSEB/crossfeed and route changes | Passed host codec/container/seek/loop/route suites. Listening and physical routes pending |
| Settings | Enumerate all persisted settings and UI controls; defaults, allowed values and on/off behavior, save/reload | Passed host schema/defaults/round-trip: 45 persisted booleans both ways plus dynamic TLS, enums and boundaries. Actual hardware effects remain pending |
| Navigation/display/input | Scroll/drag, back/home/drawer, lock/wake, button mappings, orientations and every supported resolution | Passed targeted host UI suites and safe native Home/Music/Settings/Sound/Display replay. Lock/wake/orientation exhaustive physical sweep pending |
| Themes/layouts/languages | Every shipped package and supported language; fallbacks, missing assets, reload/teardown and font sizing | Targeted host RTL/font/theme/layout suites and corrected multi-board render matrix pass. Physical flicker remains unresolved |
| Connectivity | Wi-Fi/BT drops and reconnects, remote control, DAC/USB modes, network service failures, authentication and cancellation | Native Wi-Fi disconnect/reassociate restored Remote Control in 7.649 s; a retry succeeded with HTTP 201 after 4.638 s more, with the partial removed. Native UI Wi-Fi off/on recovery took 5.181 s. Queue files stayed byte-identical. Bluetooth accessories and USB routes remain untested and are reserved for the final phase |
| Storage/power/update | Interrupted publication and reboot, full/read-only/replaced/missing SD, invalid firmware, low memory and safe recovery | Passed injected host storage/publication/fault suites, native VFAT fallback and supervised restart, plus SD-backed FAT loop full/read-only upload cases. Physical live-card removal, real power cuts, updater recovery and global OOM are not cleared |
| Long sessions | Playback/queue transitions, resource growth, audio continuity and responsiveness under load | Passed muted native endurance: 1,210.78 s, 40 samples, four repeat-one restarts, same player PID and no unexpected stop. Audible continuity and sustained physical-output behavior remain untested |

Real power cuts during a NAND write and removal of the user's live SD card
are not simulated by a successful host fixture. Dangerous failure cases use
isolated filesystems and injected interruptions first; physical coverage and
its limits are recorded separately.

## Stage 3 — plugins

Inventory every available plugin and its features, UI/settings, empty/error
states, assets and API requirement. Run existing native/Lua tests and add
meaningful missing regressions. Record authenticated services and required
hardware separately; a mocked response is not a real-service playback test.

Status: 66-plugin inventory and mocked API-17 sandbox run complete: 132 scenarios and 5,103 bounded UI actions passed. Device-provider and emulator QA remains ongoing, not complete. Tidal and Qobuz are unavailable in this region; Jellyfin and Subsonic are the current real-service candidates. Sandbox results do not prove real-service behavior.

## Fixes, test results and remaining release gates

The entries below record reviewed changes and current evidence. No overall
release approval has been given.

### GitHub gate results

The initial seven open issues and the one open PR were reviewed, including comments
and current source/history. PR #172 is merged; no open PR remains as of this
gate. #183 has a real policy gap candidate, but globally relaxing the deliberate
power-cycle-only reconnect rule risks reconnecting after manual Disconnect or
codec renegotiation. It waits for the finite-retry/suppression tests and an
affected-accessory reproduction rather than receiving an unverified change.
Stage 2 has now started.

### Reviewed release fixes and current evidence

Actual Opus 5.5 review approved the upload deadline, status revision, remembered
Bluetooth-device sample-rate parsing, theme exclusion lifetime, VFAT playlist
publication fallback and settings failure-atomic publication. Test-only review
follow-ups remain tracked separately. API 17 is unchanged.

- Upload idle time is 12 seconds, measured precisely; successful local writes
  and syncs do not consume the network idle budget. Slow progressing transfers
  retain the six-hour total limit. Real baseline TCP stall: 60.07 seconds;
  immediate retry: 201 in 0.12 seconds, no partial file.
- Settings now retain the previous file when writing, flushing, syncing,
  closing or renaming fails. Host injected failures and process termination
  recover complete old-or-new settings/queue snapshots. SIGKILL does not test
  power-loss durability of the card or kernel buffers.
- Saved Bluetooth device rates previously failed parsing because validation
  received the MAC plus the comma and rate. Valid/invalid/capacity cases and
  persisted settings matrices pass; no accessory interoperability claim.
- Clearing legacy theme-copy exclusions now survives failed persistence and
  repeated initialization within the process. Persistence across restart on a
  read-only card is not claimed.
- Playlist Save As falls back on filesystems without RENAME_NOREPLACE support,
  reserves names exclusively, preserves collisions, and takes the file-operation
  and USB storage gates in its worker. Host fault/cancellation tests pass with
  ASan/UBSan. The fallback has the same external-writer reservation race limit
  as the existing import path; it cannot provide kernel no-replace semantics.

Normal host and R1 target builds pass. Candidate SHA-256:
`4a2e245852163f990391a655e5ea6c68fd0c3371d4197bb5c72eee8904751e77`.
It was staged on verified SD VFAT, installed persistently and started successfully;
both stored queue files remained byte-identical immediately after startup.

Host coverage includes audio/decoder/container/HTTP seeking, catalog sorting and
concurrency, file-operation cancellation/faults, firmware validation/staging,
settings serialization, real-LVGL navigation/layout interactions, RTL and asset
retint/theme behavior. Detailed command/result manifests are in the evidence
folder. These are not substitutes for every physical setting effect, authenticated
streaming service, accessory, panel flicker observation or long-session test.

Outstanding: dynamic catalog revision during a live scan, broader physical UI
replay, audible long-session validation, physical Bluetooth and USB DAC checks,
updater recovery, and device-provider/emulator QA. The corrected host
theme/layout/language matrix is complete; physical panel flicker remains
unresolved. The legacy full player-layout test contains stale expectations for
colors no longer declared by shipped XML; targeted official interaction/
ownership modes pass.

### Native candidate results

The R1 is running the candidate above from persistent storage. The Wi-Fi TCP
stall probe now closes after **12.035 s**, versus **60.074 s** before the fix.
A following upload returns 201 in **0.034 s**; the partial file is absent.
`/status.catalog_revision` matches `/catalog.revision` on the live 2,421-song
library. Dynamic revision changes during a native scan are not yet tested.
Wi-Fi and Remote Control are enabled. Both stored queue files remained unchanged.

A safe native navigation replay covered Home, Music scrolling in both directions,
Settings, Sound, Display and Back/Home return paths. Ten framebuffer captures
were visually reviewed: screens rendered correctly, with no observed overlap or
missing row controls. This does not establish panel flicker or audio continuity.
The queue-model host suite includes allocation-failure and lazy/nonmutating
failure scenarios; it does not simulate global kernel OOM on the live player.

Native VFAT backend probe passed create/content, collision preservation and
provider-failure cleanup. Device `strace` confirms the real fallback was used:
`renameat2(RENAME_NOREPLACE)` returned EINVAL, followed by successful `renameat`;
a pre-existing destination returned EEXIST and remained unchanged. The probe was
statically linked and run only from the owned SD fixture. Its first dynamic build
could not run because the R1 has no musl loader; no production defect was inferred.

Additional native read-only API checks: all 25 catalog pages (2,421 unique IDs)
and 10 sorted album-group pages (240 rows) were consistent; artists, album artists,
genres, albums and queue reads succeeded. Missing authentication returned 401;
invalid catalog paging returned 400. Six owned import entries paged across three
pages sorted folders first and names case-insensitively. Traversal, absolute,
protected-dot, negative-offset and NUL paths returned 400. Import lifecycle was
restored to stopped; Remote Control remains enabled. No real queue mutation.

Opus 5.5 also approved the final test follow-ups and independently reran the
plain and ASan/UBSan queue-storage suites. Historical failed cases and corrected
runs remain in the raw evidence instead of being counted as new distinct tests.

Three native power-button sleep/wake cycles passed; the same player process
remained alive and the final Home framebuffer rendered correctly. This checks
liveness, not measured wake latency, physical flash/flicker or lock-screen modes.
An earlier inventory recheck found no open PRs and the same seven issues. A subsequent recheck found new issue #194 (remote store/plugin/layout APIs), reviewed and deferred as a substantial feature expansion. There are now eight open issues and no open PRs.

### Follow-up candidate and plugin corrections

A further injected playlist-close failure found that a reserved empty name could
remain after `close()` reported an error. Ownership is now retained independently
of close success: publication is refused and inode-checked cleanup still runs.
Opus 5.5 approved this fix and its deterministic test. Normal and sanitizer queue
storage tests pass. The final normal host and target builds pass; the R1 now runs:
`17a21e80f70fb0850ac511f0ba2b3f8b882eabcedc1b7a60e8223021150cadac`.
It was staged on SD and installed persistently. Wi-Fi/Remote Control are reachable,
revision is present, and both stored queue files match the immediate pre-install
backups. The upload path is unchanged from the native 12.035-second result.

Fifteen repeated Music/Settings/Display navigation cycles passed without restart.
RSS was 9,452 KiB initially and 9,456 KiB finally; swap remained 17,572 KiB and
threads remained 15. This is a bounded short navigation stress check, not proof
of no leak or sustained audio performance.

Earlier plugin commit `85abb8d` was pushed to main: Qobuz 1.3.8 and Tidal 1.3.7 remove
the obsolete eager 12-track compatibility path, consistently using API 17
on-demand links for up to 500 tracks. Queue/resolver tests and Lua syntax pass;
Opus 5.5 approved the changed hunks. No release tags or publishing were used.
Real authenticated streaming remains untested. Earlier pre-release API-17 builds
without the additive resolver event are outside this owner-chosen contract.

The initial standalone XML screenshots were not accepted as visual QA: they
bypassed production theme/binding setup and used a different artwork source
representation. Corrected visual fixtures use production build/bind, owned RGB565
artwork descriptors and a complete refresh before capture. Construction/bounds
results remain distinct from visual approval; corrected multi-board review is
complete as host evidence; physical panel behavior remains unverified.

### Executed verification commands

Commands below ran against the preserved working tree. Build commands use the
host cache for temporary files. The raw manifests retain additional audio/UI
commands and corrected failures; rows here summarize representative final checks.

| Command | Result |
| --- | --- |
| `TMPDIR=$HOME/.cache/compas-theme-review make host -j6 BOARD=r1 UI_PERF_TRACE= UI_PERF_TRACE_DEFINE=` | Pass |
| `TMPDIR=$HOME/.cache/compas-theme-review make target -j6 UI_PERF_TRACE= UI_PERF_TRACE_DEFINE=` | Pass |
| `bash scripts/tests/network/remote/run_remote_import_upload_timeout_test.sh` | Pass, includes precise deadline and slow local-I/O regressions |
| `bash scripts/tests/network/remote/run_remote_control_status_spec.sh` | Pass |
| `bash scripts/tests/library/metadata/run_metadata_catalog_test.sh` | Pass |
| `bash scripts/tests/core/settings/run_settings_roundtrip_spec.sh` | Pass |
| `bash scripts/tests/core/settings/run_atomic_persistence_powerloss_spec.sh` | Pass, host injected failures/process termination |
| `bash scripts/tests/ui/playlists/run_player_queue_storage_spec.sh` | Pass |
| `SANITIZE=1 bash scripts/tests/ui/playlists/run_player_queue_storage_spec.sh` | Pass |
| `bash scripts/tests/library/browser/test_file_ops.sh` | Pass, host sanitizers and faults |
| `bash scripts/tests/library/artwork/test_image_thumb.sh` | Pass |
| `python3 scripts/test_firmware_update_host.py` | Pass using cached valid fixture; no NAND operation |
| Plugin repo: `python3 tools/test_api_requirements.py` | Pass |
| Plugin repo: all nine `lua`/Python feature runners listed in `plugins-existing-tests.json` | Pass; ten existing checks total including API requirements |
| Plugin repo: `python3 tools/test_stage3_plugin_sandbox.py --self-test` | Pass |
| Plugin repo: `GO_PLUGINS=1 python3 tools/test_stage3_plugin_sandbox.py --run` | 66 plugins, 132 mocked scenarios, 5,103 bounded UI actions; all final scenarios pass |

The sandbox checks syntax/load, registrations, settings traversal and explicit
SDK mocks in empty/offline/no-track scenarios. It supplied no real credentials,
made no provider requests and virtualized filesystem/storage. Earlier LastFM and
Discover reports were rejected as mock defects and withdrawn, not changed in
production. Track Inspector's missing optional getter has a working state-table
fallback, which was verified. Authenticated streaming, games with real ROMs,
physical recording/radios and every positive plugin feature remain separate gaps.

Plugin preview bundle commit `db97305` and offline sandbox commit `2278fd4` were
also pushed. Preview images were reviewed and focused Audiobooks/Podcasts/Reading
Room tests pass. No plugin release tags or publishing occurred.

Owned SD test folders were removed after native verification; the installed
player and user files remain. Persistent crash logs contain no new crash marker
(0 before and after). Wi-Fi/Remote Control remain enabled and saved queues intact.

A source-and-observed-UI checklist was completed for all 66 plugins, separately
from functional claims. Two concrete clarity improvements were implemented:
Podcasts gives an actionable no-episode state without misleading host/show rows,
and ABX identifies the active accepted formats in an empty audio or PEQ folder.
Focused actual-function Lua tests cover these states, metadata fallback and file
selection indices. Four affected sandbox scenarios pass after the changes.
Plugin commit `65b002f` contains these updates (Podcasts 1.7.3, ABX 1.1.3).
No real-service success is implied by offline mocks or this wording review.

The latest plugin repository commit is `a141a1c` (`Keep QA capability discovery
independent of source layout`).

### Corrected visual matrix — accepted host evidence

Opus 5.5 approved the corrected C test harnesses after rereview. The shared PPM
writer verifies format/stride/size and converts BGR memory to RGB. Matrices now
use production base-XML variant resolution (including `@` variants), builder and
binder, the actual owned RGB565 cover state and derived artwork, and complete
captures. All temporary layout fixtures are cleaned on every exit path.

- Three host resolutions: R1 480×800, R3 Pro II 480×720, R3 II 2025 320×480.
- Ten selected plugin layouts plus native default per board; real cover,
  fixture metadata and controls rendered and final contact sheets reviewed.
- All 42 theme files across 21 packages parsed/applied per board. Each board
  rendered 463 representative theme samples: 42 × 11 supported languages plus
  an unknown-Arabic fallback. Each asserted 420 non-English translations of
  `Music`. Arabic fallback is not an Arabic translation or RTL theme test.
- Native-default icons include a test stub, so these captures do not establish
  native asset completeness. The native navigation replay separately shows
  actual Back and Settings icons.
- These theme samples are representative style objects, not every real screen
  in every language/theme combination. Physical panel flicker is not tested.

The focused theme runner also passed from outside the repository with a relative
scratch directory after its path was normalized. No production UI behavior was
changed to make the renderer pass.

### Remaining release checks — not cleared

| Check | Why it remains / needed evidence |
| --- | --- |
| AirPods 4 reconnection (#183) and high-rate LDAC clipping (#176) | Final Bluetooth peripheral phase: affected accessories and real drop/reconnect/listening trials; preserve intentional Disconnect/Forget suppression |
| Native DSD/DoP (#188) | Final USB DAC peripheral phase: compatible external DAC and listening/output negotiation, disconnect and fallback checks; host route/output tests alone are insufficient |
| Firmware updater/recovery (#133) | Recoverable physical updater test, cold boots and interrupted staging; no NAND write or destructive power-loss test was performed |
| SD removal/full/read-only | SD-backed FAT loop full/read-only upload checks passed with cleanup, preservation and retries. Physically removing or filling the user's live card was not performed |
| White/high-contrast cover/theme flicker | Physical panel observation/camera with affected bright artwork; framebuffer snapshots cannot establish absence of flicker |
| Every setting's real hardware effect | Persistence and representative UI tests pass, not all DSP/audio/charging/clock/battery/network effects or combinations |
| Every plugin's positive features | Offline/no-track/settings sandbox and targeted fixtures pass; authenticated providers, real games, recording, radios and peripherals need their own accounts/media/hardware trials |
| Whole-app localization and all theme/layout combinations | Representative host matrices and targeted RTL/font tests pass; not every screen or Cartesian combination |
| Sustained audible playback | Muted native endurance completed; no listening or physical output continuity claim |
| PrBoom audio | Published core logged 23 underruns in a 26.6-second smoke. A separate 90-second interval run logged 30, including active-play intervals; 8 MiB remains unchanged and no audio pass is claimed |

No overall release sign-off is given. Player fixes and the existing refactor
remain in the preserved uncommitted player tree; plugin changes were committed
and pushed separately as requested. No plugin publication occurred.

### Native muted endurance and layout changes

The muted native playback run completed **1,210.78 seconds** with 40 samples,
four repeat-one restarts, the same player PID throughout and no unexpected stop.
Maximum sampled status latency was **0.1444 seconds**. All samples had 16 threads.
The run included opening the live layout store and ten background switches
  between Default and Audiobooks Reading Room, then restoring Default. Decoder,
layout and preview caches changed the working set, so RSS/swap changes are not
interpreted as a leak measurement. No audible continuity claim is made.

Original mode, volume and position were restored, playback is paused, and both
stored queue files are **byte-identical** to pre-run backups (231 bytes internal,
223,068 bytes SD). User preference keys match the pre-audit settings apart from
the normal playback checkpoint. The device stayed on the final installed build.

The stock daemon accepts `adb tcpip` but its supervisor restarts it in USB mode;
Wi-Fi port 5555 remained closed. No wireless-ADB success is claimed. An SD-only
USB/ALSA sampler was prepared and foreground-tested; the owner's instruction is
to leave USB DAC and Bluetooth headphone tests until the non-peripheral work is
  finished. They remain the final phase, not waived tests.

### Published emulator package native smoke

All seven published v0.3.0 packages launched and exited across ten supported
system/fixture combinations. The player PID remained 1269, queue/settings
hashes and preexisting GBA/PCE save hashes matched, and owned fixtures were
removed with the original session log restored. Detailed evidence is in
`release-qa-20261025/emulator-native-smoke/native-smoke-summary.json`.

This is not an audio-performance pass: Game Boy and three Gearsystem runs
logged one recoverable underrun each; Freedoom logged **23 ALSA EPIPE
underruns in 26.6 seconds**. All reported zero unrecovered write errors.
Freedoom's discrepancy from the earlier approved listening run is under
investigation. No listening or sustained-FPS claim is made.

The initial PCE exit and PrBoom-card failures were resolved by correcting the
test driver's type-A protocol and wake sequence. A separate isolated host
characterization using the real LVGL evdev driver reproduced a stale queued
Back click after a synchronous callback returns. It establishes a mechanism,
not yet proof of the exact native first-Back observation. Actual Opus 5.5
review confirmed the queued-input mechanism and identified missing EVENT-mode,
held-contact and fresh-tap coverage. A focused fix and expanded regressions
are in progress; it is not installed or cleared yet.

### Authenticated service browsing

The R1 connected using its saved Subsonic account and loaded Artists, an
artist's Albums, Playlists and all Albums. Jellyfin restored a saved account
and loaded Music Libraries plus artist, album and song lists (server totals
339 artists, 1,015 albums and 11,137 songs; first 25-row pages checked). Both queue files remained byte-identical and player PID stayed
1269. Saved connection details were handled privately; no credentials or
tokens were included in output. Tidal/Qobuz real-service tests are regionally
unavailable, as confirmed by the owner. These checks do not establish remote
queue advancement, seeking or audible playback.

Jellyfin's next-page action also loaded songs 26–50 from the real server
without changing the queue. The provider keeps server-side sorting; these
checks concern browse/pagination behavior, not local catalog collation.

### Input-handoff fix — reviewed, device verification pending

Actual Opus 5.5 approved the parser-owned evdev queue discard and GUI handoff
fix after missing host stubs and EVENT-mode held-contact coverage were fixed.
Full host and target builds passed; candidate SHA-256 is
`823184a018754fd49513c78c43339e27643e28e45185aa08af92f3d3d75ea2b6`.
This intermediate candidate was superseded by the combined SD-backed
installation recorded below.

Real-driver regressions pass TIMER/EVENT × single/type-A × live-key/fallback ×
held/released cases, with bounded EVENT readiness draining. Opus mutation
checks confirmed that removing queue discard or held-contact suppression fails
the test. Follow-finger, remote-stream, SACD, shell, CUE runtime, USB DSD segment,
audio startup and button-lock lifecycle checks passed. The HTTP range/seek
runner initially hit a shared make configuration temporary-file race from
concurrent test builds; its serial rerun passed real HTTP range/seek scenarios.
This runner race is separate from product results.

### Plugin pagination UX — fix in progress

Live Jellyfin Next page correctly fetched songs 26–50 but inherited the prior
page's bottom offset. An explicit `reset_scroll` option is being added for
logical page/view changes while preserving the merged same-page refresh
behavior. Jellyfin and Cover Fetcher's in-place history paginator will opt in;
API 17 and existing replacement defaults remain unchanged.

PrBoom follow-up corrected an evidence-provenance error: the older QEMU
purge-8 crashes predated the published binary and did not retain a command or
executable hash. A fresh isolated pair on the actual published SHA completed
600 frames at both 8 and 16 MiB without a segfault. Both save-state round trips
failed in QEMU; native audio timing and save-state behavior are separate checks.
The 23 native underruns remain real and unexplained; no cache-budget change or
emulator source change has been made. See `prboom-audio-followup.md` and
`prboom-purge-repro-20261025/results.json` in the private QA evidence directory.

### Reviewed combined installation and live pagination

The combined non-trace player is running on the R1 from persistent storage:
SHA-256 `d3f33c59c7e26df02a402eeb20889c10c51ab8cc33c7b7aa5df3c638923d3e7d`,
PID 1271 after a successful managed reboot. SD staging and the running
`/proc/1271/exe` hash were verified. Wi-Fi and Remote Control were re-enabled
through the native UI; authenticated `/status` returns HTTP 200 with the
catalog revision. Both queue files remain byte-identical.

Actual Opus 5.5 approved both C changes and the final plugin follow-ups. Live
Jellyfin pagination now opens songs 26–50 at the top (Back, Previous page, first
song), rather than inheriting the former bottom offset. Immutable request keys
and latest-request-wins generations also have deferred-response regressions in
both completion orders. Cover Fetcher's history has real-plugin mocked
Older/Newer/receipt regressions.

Plugin changes were committed and pushed as `1bdf970` without publication or
release tags. The final offline rerun passed **132 mocked scenarios across 66
plugins, 5,103 bounded UI actions**, and all **11** `tools/test_*.lua` scripts.
These counts do not turn mocked providers into real streaming/listening tests.

The published PrBoom diagnostic used its existing `--stats-interval 2` option,
with the unchanged 8 MiB limit. Of 45 two-second active intervals, the first
four seconds had zero underruns, the next 20 seconds had 26, the following
36 seconds had four, and the final 30 seconds had none. CPU was 91–96% in the
clustered intervals. This excludes an exclusively initialization-time pattern
but does not prove the cause. Interval logging adds SD I/O. The temporary
wrapper, manifest and session log were restored, with queue/settings hashes
unchanged. Evidence: `prboom-stats-device/result.json` in the QA cache.

### Final handoff verification and peripheral preparation

Two instrumented PCE exits passed, including a stale header touch injected
while the child owned input: the parent stayed on its game list, and the first
fresh Back returned to Emulators once. Host command durations were 1.010 and
0.991 seconds; device capture/input brackets were 0.49 and 0.46 seconds. These
are not isolated transition-latency measurements. A third scripted launch was
inconclusive; a controlled root repeat successfully launched the actual test
fixture, exited and accepted the first fresh Back. An additional existing-game
launch also succeeded. No extra save was created for it. Queue/settings hashes,
existing test SRAM and the restored original session log all match baseline;
the main player remains PID 1271.

A bounded ALSA sampler was verified to complete 12 SD-backed samples after its
ADB launching shell closed. `nohup` plus `setsid` and a start-file handshake
were required; an unacknowledged background launch raced shell termination.
The validation artifacts were removed. An actual USB disconnect is not yet
tested; Wi-Fi Remote Control is the planned control path while a DAC occupies
the USB port. Bluetooth headphones and USB DAC remain the final hardware phase,
as the owner requested; availability/model information has been requested.

Final cleanup verified `final-native-state.json`: Wi-Fi associated, Remote
Control enabled, HTTP 200, running executable hash matches the installed
candidate, queue files byte-identical, updated plugin sources match pushed
commit, no test loop devices, no remaining owned SD test folders and zero
crash markers. The device is stopped on Home, ready for the owner's next
checks. Hardware model/availability information is still required before
Bluetooth and USB DAC trials.

The remaining media session should run real streaming/seek/queue tests first,
then Bluetooth, then USB DAC. Keeping it in one session permits one exact queue
restoration instead of repeated restarts. Before replacing any test queue,
back up both original queue files and settings with verified hashes. The
confirmed supervisor is `compas_loader` (parent of the player, not PID 1).
For exact restoration, hold that supervisor, stop the player, restore original
files atomically while no player worker can overwrite them, verify hashes,
then resume the supervisor for a managed reboot and re-enable Wi-Fi/Remote
Control. Credentials stay in private host/persistent-flash backups. This is a
planned procedure, not a completed streaming or hardware test. The current
original queue remains untouched while accessory availability is pending.

### Deferred UI defect — streaming pagination consistency

**Open; owner-confirmed; fix later.** Jellyfin and Subsonic browsing should
follow the file explorer's standard Previous/Next pagination design. The
current mixture of Back and Previous page rows is confusing: Back navigates
to the parent view, while Previous page changes pages within the current view.
Jellyfin additionally exposes Back as a list row despite the header Back arrow.

Acceptance criteria:
- Use the file explorer's pagination placement, controls and appearance in
  both Jellyfin and Subsonic wherever paging is needed.
- Keep parent navigation in the standard header Back control; remove redundant
  Back list rows.
- Preserve page boundaries, scroll behavior, asynchronous request cancellation
  and the current playback queue.

This is a presentation defect separate from the corrected page-offset reset.
No code or device changes were made for this deferred item.


## Firmware installation follow-up

The owner authorized firmware installation and four-board image preparation.
The R1 completed a stock-recovery update from the verified SD-backed package
`43f55224db696391cdc650b77d276176a533505d88f98946d83f6eb5fa1bd8e9`.
After reboot both the packaged and persistent player match
`2f8bf7ea749cf115bcecf73cc240ab99fdf6b3482dd4f1716d1cc955f1c039e1`;
`/usr/bin/bluealsad` matches the static-SSC runtime
`d683e3389fbfb3e35351d3b44f9dafbbc4007136c1f2ee510702280415cc11c4`.
The temporary daemon bind mount is absent after reboot. Kernel and rootfs
fit the observed R1 partitions; recovery helper diagnostics are persisted on SD.
The original SD-root firmware and pre-flash queue were backed up.

The startup-volume fix forces one hardware-volume capability retry immediately
before source preparation, avoiding the earlier five-second negative cache when
AVRCP becomes available between PCM discovery and playback. Existing running-PCM
protection remains. Opus 5.5 approved the change and its characterization tests.
The owner confirmed normal volume after the earlier pause/reprobe; cold-start
listening and post-flash volume synchronization remain hardware checks.

Both R3 target builds and firmware packages passed host checks, including exact
input hashes, unchanged base kernels, OTA chunk chains and package size gates.
They have not been installed on R3 hardware. The V1 image requires a separate
port and packaging pass; the official V1 1.2 base and vendor checksum have now
been verified. Its in-progress work is isolated outside the main source tree.
These artifacts are QA candidates, not a declaration that every release-audit
scenario or peripheral has passed. Wi-Fi/Remote Control restoration and final
queue verification after the firmware reboot are still in progress.


## Main integration checkpoint review

Opus 5.5 approved the staged integration checkpoint with no demonstrable
blocking findings. Its final review covered boot/splash code, init ordering,
packaging, build inputs and USB DSD integration, checked tracked source/header
closure, and reran 13 focused host specs successfully. Earlier Opus reviews
cover the individual QA fixes. The large extraction was not reread line by
line during this final pass. The R1 firmware installed above intentionally
used the clean packaging path and **did not include** the new early-splash
helper/init pipeline; that pipeline has host tests and three target builds,
not device boot verification. Native USB DSD/DoP listening remains pending.
