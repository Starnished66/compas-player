/* SD card hotplug detection, mount-failure popups and Format SD Card. */
#include "../../text/i18n/i18n.h"
#include "gui_library_internal.h"
#include "gui_folder_actions.h"
#include "../../../library/metadata/durations/duration_cache.h"
#include "../library_thumbnails.h"
#include "../../../network/bluetooth/control/bluetooth_control.h"
#include "../../application/gui.h"
#include "../../../core/storage/sd_fsck_run.h"
#include "../../../library/browser/files/file_browser.h"
#include "../../../library/browser/files/file_ops.h"
#include "../../storage/sd_card_identity.h"
#include "../../../core/process/subprocess.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>


static bool sd_format_active;

static gui_busy_handle_t sd_format_token = 0;

/* SD mount-failure detection + Format SD Card -- see poll_sd_card_hotplug()
 * below for the detection/debounce logic and sd_format_card_worker() further
 * down for the actual format sequence. These three are declared here
 * (rather than alongside their real definitions further down) so that both
 * halves of the feature -- the gated detection logic below, and the
 * ungated popups/background thread that come after it in this file -- can
 * see them regardless of which one is defined first. */
static bool sd_mount_fail_notified = false;
#ifndef HOST_BUILD
static void show_sd_mount_failed_popup(void); /* defined below, alongside its popup */
static bool sd_handoff_pending = false; /* only the target's hotplug logic latches it */
#endif

bool file_folder_job_storage_invalidated(void) {
#ifdef HOST_BUILD
    return false;
#else
    return sd_handoff_pending || sd_repair_in_progress() || !sd_card_root_is_mounted();
#endif
}

#ifndef HOST_BUILD

/* Periodic polling for SD card mount state changes.
 *
 * When an unmounted card is detected, retries mounting. On transition to
 * mounted, reloads the database cache from the card. On transition to
 * unmounted, closes the database, clears library screens, and resets
 * the file browser to root. */
#define SD_CARD_MOUNT_POLL_SECONDS 5

/* Consecutive failed poll cycles (SD_CARD_MOUNT_POLL_SECONDS apart) before
 * the mount-failure popup shows -- 6 * 5s = 30s of genuinely stuck retries,
 * matching this file's own LIBRARY_SCAN_WALK_STALL_TIMEOUT_MS in spirit
 * (long enough that the kernel's own brief card-enumeration window right
 * after physical insertion can't false-positive, short enough the user
 * isn't left staring at an empty library wondering what's wrong). */
#define SD_MOUNT_FAIL_STREAK_THRESHOLD 6

static dev_t last_mounted_device;
static bool have_last_mounted_device;
static char last_card_cid[64];
static bool have_last_card_identity;
static bool prompt_card_change_seen;

/* Assume already mounted initially since mount_sd_card_if_needed() ran
 * at boot. */
static bool was_mounted = true;
static int mount_fail_streak = 0;
/* Require unmounted state across consecutive polls
 * (SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD) before tearing down library
 * state to debounce brief transient unmounts. */
static int unmount_confirm_streak = 0;
#define SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD 2

static bool sd_card_mount_attached(dev_t * mounted_device) {
    struct stat parent_st, root_st;
    if (stat("/data/mnt", &parent_st) != 0) return false;
    if (stat(MUSIC_ROOT_DIR, &root_st) != 0) return false;
    if (parent_st.st_dev == root_st.st_dev) return false;
    if (mounted_device) *mounted_device = root_st.st_dev;
    return true;
}

static sd_probe_result_t sd_kernel_device_probe(dev_t mounted_device, const char * node) {
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/block/%s/dev", node);
    errno = 0;
    FILE * file = fopen(path, "r");
    if (!file) return errno == ENOENT ? SD_PROBE_ABSENT : SD_PROBE_UNKNOWN;
    unsigned major_number, minor_number;
    int parsed = fscanf(file, "%u:%u", &major_number, &minor_number);
    fclose(file);
    if (parsed != 2) return SD_PROBE_UNKNOWN;
    return mounted_device == makedev(major_number, minor_number) ? SD_PROBE_MATCH : SD_PROBE_DIFFERENT;
}

static bool sd_card_root_is_mounted_observed(bool * attached_observed) {
    dev_t mounted_device;
    bool attached = sd_card_mount_attached(&mounted_device);
    if (attached_observed) *attached_observed = attached;
    if (!attached) return false;
    /* ntfs-3g uses a FUSE filesystem device rather than the underlying MMC
     * device. Its identity is checked by the card CID in the hotplug poll;
     * presence in sysfs still catches a removed card. */
    if (major(mounted_device) != 179) {
        struct stat st;
        if (stat("/sys/class/block/mmcblk0", &st) == 0) return true;
        /* A failed stat that is not "node missing" is not a pulled card. */
        return errno != ENOENT;
    }
    /* The kernel can leave an exFAT mount attached after the card is pulled.
     * /dev/mmcblk0p1 can also be stale if mdev missed the replacement event.
     * Compare the mount's actual device with sysfs, which follows the newly
     * inserted card even when its /dev node has not been recreated. A sysfs
     * read error is not that comparison and must not look like removal. */
    return !sd_card_probes_confirm_removed(
        sd_kernel_device_probe(mounted_device, "mmcblk0p1"),
        sd_kernel_device_probe(mounted_device, "mmcblk0"));
}

bool sd_card_root_is_mounted(void) {
    return sd_card_root_is_mounted_observed(NULL);
}

static int sd_card_sysfs_node_stat(const char * node, int * err) {
    char path[96];
    struct stat st;
    snprintf(path, sizeof(path), "/sys/class/block/%s", node);
    if (stat(path, &st) == 0) {
        *err = 0;
        return 0;
    }
    *err = errno;
    return -1;
}

static bool sd_card_device_node_present(void) {
    int ignored = 0;
    /* Query sysfs rather than /dev: mdev may have left stale nodes behind.
     * A partition-less card has only the whole-disk entry. */
    return sd_card_sysfs_node_stat("mmcblk0p1", &ignored) == 0 ||
           sd_card_sysfs_node_stat("mmcblk0", &ignored) == 0;
}

/* Both nodes missing. A read error on either one is not confirmation. */
static bool sd_card_device_nodes_confirmed_absent(void) {
    int partition_errno = 0, disk_errno = 0;
    int partition_rc = sd_card_sysfs_node_stat("mmcblk0p1", &partition_errno);
    int disk_rc = sd_card_sysfs_node_stat("mmcblk0", &disk_errno);
    return sd_card_sysfs_stats_confirm_absent(partition_rc, partition_errno, disk_rc, disk_errno);
}

/* Whole-disk sysfs entry -- present whenever a card is physically inserted
 * regardless of whether it has a partition table at all, so this is what
 * distinguishes "no card" from "card inserted but can't be mounted" for the
 * mount-failure/Format SD Card flow below (mount_sd_card_if_needed() itself
 * -- see main.c -- already tries every filesystem type this platform
 * actually supports, so a card that's physically present but still never
 * mounts genuinely needs a repartition/reformat, not just a different -t). */
static bool sd_card_base_device_present(void) {
    struct stat st;
    return stat("/sys/class/block/mmcblk0", &st) == 0;
}

static void sd_card_read_cid(char out[64]) {
    out[0] = '\0';
    FILE * file = fopen("/sys/class/block/mmcblk0/device/cid", "r");
    if (!file) return;
    if (fgets(out, 64, file)) out[strcspn(out, "\r\n")] = '\0';
    fclose(file);
}

library_prompt_card_status_t library_prompt_card_status(void) {
    if (!sd_card_root_is_mounted()) return LIBRARY_PROMPT_CARD_UNKNOWN;
    bool have_current_device = false;
    dev_t current_device = 0;
    have_current_device = sd_card_mount_attached(&current_device);
    if (!have_current_device) return LIBRARY_PROMPT_CARD_UNKNOWN;
    char current_cid[64];
    sd_card_read_cid(current_cid);
    if (sd_card_identity_replaced(true, true, have_last_card_identity,
                                  have_last_mounted_device, last_mounted_device,
                                  have_current_device, current_device,
                                  last_card_cid, current_cid))
        return LIBRARY_PROMPT_CARD_CHANGED;
    if (have_last_card_identity && !current_cid[0]) return LIBRARY_PROMPT_CARD_UNKNOWN;
    return LIBRARY_PROMPT_CARD_CURRENT;
}

void library_prompt_prepare_cache_load_card(void) {
    if (!sd_card_root_is_mounted()) return;
    dev_t current_device = 0;
    bool have_current_device = sd_card_mount_attached(&current_device);
    char current_cid[64];
    sd_card_read_cid(current_cid);
    bool replaced = sd_card_identity_replaced(true, true, have_last_card_identity,
                                              have_last_mounted_device, last_mounted_device,
                                              have_current_device, current_device,
                                              last_card_cid, current_cid);
    if (replaced) library_prompt_card_changed();
}

static void clear_removed_sd_library(void) {
    /* Signal the worker without joining on the hotplug/repair path. Its UI
     * completion joins later and rejects even an already-finished result. */
    gui_folder_actions_invalidate_storage_targets();
    gui_folder_actions_cancel();
    file_op_cancel(); /* a File Manager copy must not continue on a new card */
    quiesce_thumbnails();
    /* The remembered durations belong to the card that left: drop them and
     * ignore any load or write still running for it (never blocks). */
    duration_cache_new_storage_epoch();
    gui_player_handle_sd_unmount();
    metadata_db_close();
    refresh_library_screens_after_reload(true);
    file_browser_reset_to_root();
}

static void show_sd_repair_note(void) {
    const char * message = NULL;
    switch (sd_repair_take_note()) {
    case SD_REPAIR_NOTE_STARTED: message = TR("Checking the SD card. This may take a while"); break;
    case SD_REPAIR_NOTE_REPAIRED: message = TR("SD card repaired"); break;
    case SD_REPAIR_NOTE_STILL_READONLY: message = TR("SD card is still read-only"); break;
    case SD_REPAIR_NOTE_FAILED: message = TR("Could not repair the SD card"); break;
    case SD_REPAIR_NOTE_NEEDS_COMPUTER: message = TR("SD card may have errors. Check it on a computer"); break;
    case SD_REPAIR_NOTE_READONLY_NEEDS_COMPUTER: message = TR("SD card is read-only. Check it on a computer"); break;
    case SD_REPAIR_NOTE_NONE: break;
    }
    if (message) show_error_toast(message);
}

/* Drop file handles so umount can succeed after the kernel has remounted
 * a damaged card read-only underneath an open library. */
static void release_sd_handles_for_repair(void) {
    bt_control_debug_capture_release(false);
    if (library_rescan_active) gui_library_cancel_scan();
    library_thumbnails_cancel_warmup();
    gui_player_notify_sd_unmounted_immediate();
    clear_removed_sd_library();
}

static void sd_note_card_changed(void) {
    if (!prompt_card_change_seen) library_prompt_card_changed();
    prompt_card_change_seen = true;
}

static void sd_forget_card_identity(void) {
    have_last_card_identity = false;
    have_last_mounted_device = false;
    last_card_cid[0] = '\0';
}

static void sd_mount_fail_reset(void) {
    mount_fail_streak = 0;
    sd_mount_fail_notified = false;
}

static void sd_hotplug_unmounted(bool stale_mount, bool forced_removal, bool replaced_between_polls) {
    gui_folder_actions_invalidate_storage_targets();
    gui_folder_actions_cancel();
    if (group_probe_job && group_probe_job_generation == group_probe_generation)
        cancel_group_song_probes();
    gui_player_notify_sd_unmounted_immediate();
    bt_control_debug_capture_release(false);
    bool removal_confirmed = sd_card_absence_confirmed(
        stale_mount || forced_removal, 0, SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD);
    if (was_mounted) {
        /* Stop post-scan artwork reads/writes as soon as removal is
         * observed. Joining is deliberately deferred to the next
         * generator start/reap poll so this hotplug callback never
         * blocks the UI on an in-progress image decode. */
        library_thumbnails_invalidate_storage();
        unmount_confirm_streak++;
        removal_confirmed = sd_card_absence_confirmed(
            stale_mount || forced_removal, unmount_confirm_streak,
            SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD);
        if (removal_confirmed) sd_note_card_changed();
        if (removal_confirmed && !library_rescan_active) {
            /* Close old handles before loading the next card. */
            clear_removed_sd_library();
            was_mounted = false;
            unmount_confirm_streak = 0;
            sd_forget_card_identity();
        }
        /* Else: not yet confirmed (or a rescan from something else,
         * e.g. Settings > Update Music Database, is already running)
         * -- was_mounted deliberately stays true, so a flicker that
         * remounts before reaching the threshold is not a handoff.
         * The latch below follows removal_confirmed, which was sampled
         * before the streak reset, so a confirmed removal stays pending
         * while the scan still holds the database. */
    }
    sd_handoff_pending = sd_card_handoff_after_sample(
        sd_handoff_pending, removal_confirmed, replaced_between_polls);

    /* Tear down state tied to the old card before mounting a replacement.
     * Otherwise a rapid swap can hide the unmounted edge from the next
     * poll and leave the previous card's library and queue in memory. */
    mount_sd_card_if_needed();
    if (sd_card_root_is_mounted()) {
        sd_mount_fail_reset();
        return;
    }
    /* A read-only card is unmounted while fsck runs. That is not a
     * missing or unmountable card, so it must not offer Format. */
    if (sd_repair_in_progress()) {
        sd_mount_fail_reset();
        return;
    }

    /* Card is physically there (the whole-disk node exists) but still
     * won't mount after repeated retries. mount_sd_card_if_needed()
     * (main.c) already tries every supported filesystem type against
     * both the first-partition node and, as a fallback, the whole-disk
     * node itself (covering a "superfloppy" card with no partition
     * table/MBR at all) -- so reaching here means either the partition
     * exists but holds something none of those attempts could mount
     * (corruption, or a filesystem this platform genuinely doesn't
     * support), or the raw disk itself does too. Both need the same fix
     * -- reformat -- so this doesn't try to tell them apart. Debounced
     * (SD_MOUNT_FAIL_STREAK_THRESHOLD
     * consecutive failed polls, SD_CARD_MOUNT_POLL_SECONDS apart) so
     * the kernel's own brief card-enumeration window right after
     * physical insertion -- where the node legitimately isn't there
     * *yet* -- doesn't fire a false alarm; not re-shown on every poll
     * once shown once (sd_mount_fail_notified), so the user isn't
     * nagged again until the card actually changes (removed, or a
     * format attempt runs -- both reset it below/in poll_sd_format()). */
    if (sd_card_base_device_present()) {
        mount_fail_streak++;
        if (mount_fail_streak >= SD_MOUNT_FAIL_STREAK_THRESHOLD && !sd_mount_fail_notified && !sd_format_active) {
            sd_mount_fail_notified = true;
            show_sd_mount_failed_popup();
        }
    } else {
        sd_mount_fail_reset();
    }
}

static void sd_hotplug_mounted(bool replaced_between_polls, bool have_current_device,
                               dev_t current_mounted_device, const char * current_card_cid) {
    /* See its own comment further down, where it's checked -- separate
     * one-shot guard for the boot-time-mount-race case was_mounted's own
     * confirmed-transition logic can silently miss. */
    static bool boot_library_recheck_done = false;

    sd_mount_fail_reset();
    if (!sd_repair_in_progress()) bt_control_debug_capture_resume(false); /* a healthy card is mounted */
    unmount_confirm_streak = 0; /* seeing "mounted" again cancels any not-yet-confirmed removal */
    /* Same card again does not drop a latch. Only confirmation or a real
     * replacement sets one; passing a cleared streak here leaves a real
     * pending handoff untouched. */
    sd_handoff_pending = sd_card_handoff_after_sample(
        sd_handoff_pending,
        sd_card_absence_confirmed(false, 0, SD_UNMOUNT_CONFIRM_STREAK_THRESHOLD),
        replaced_between_polls);
    if (have_last_card_identity && !current_card_cid[0]) {
        /* A transient CID read failure is an uncertain identity, never a
         * license to restore the old card's queue or cache. */
        gui_player_notify_sd_unmounted_immediate();
        return;
    }
    if (library_rescan_active && (sd_handoff_pending || !was_mounted || replaced_between_polls)) {
        /* Keep the old identity until the scan worker releases its database
         * handles; the next poll must still perform the card handoff. */
        gui_player_notify_sd_unmounted_immediate();
        return;
    }
    /* Every poll that observes the card mounted un-sticks the checkpoint
    * target, even for a transient blip that never reached the confirmed-
     * removal streak above. */
    gui_player_notify_sd_mounted();
    /* Reset file browser and reload fallback fonts before triggering the
     * rescan screen switch. */
    if (sd_handoff_pending && !library_rescan_active) {
        if (was_mounted) clear_removed_sd_library();
        was_mounted = false;
        gui_folder_actions_invalidate_storage_targets();
        file_browser_reset_to_root();
        fallback_font_on_sd_mounted();
        reload_library_on_sd_reinsert(true);
        gui_player_restore_sd_queue(false);
        sd_handoff_pending = false;
    } else if (!boot_library_recheck_done) {
        boot_library_recheck_done = true;
        fallback_font_on_sd_mounted();
        if (metadata_db_get_load_outcome() == METADATA_DB_LOAD_UNMOUNTED && !library_rescan_active) {
            gui_folder_actions_invalidate_storage_targets();
            file_browser_reset_to_root();
            reload_library_on_sd_reinsert(false);
        }
        /* Covers the boot-time mount race this whole branch exists for: the
         * card genuinely was NOT YET mounted when gui_init() ran its own
         * SD-restore attempt (both use the same sd_card_root_is_mounted()
         * check), but became mounted by this first poll. This is the only
         * other place a "the card actually showed up right after boot"
         * restore can happen --
         * boot_library_recheck_done's one-shot guard means this fires at
         * most once per run. */
        gui_player_restore_sd_queue(true);
    }
    /* Handoff above may have cleared was_mounted. A missed reread must not
     * be stored as device 0, or the next poll looks like a different card. */
    have_last_mounted_device = sd_card_remember_mounted_device(
        have_current_device, current_mounted_device, replaced_between_polls, was_mounted,
        &last_mounted_device, have_last_mounted_device);
    was_mounted = true;
    if (current_card_cid[0]) snprintf(last_card_cid, sizeof(last_card_cid), "%s", current_card_cid);
    /* An empty CID after a handoff must not resurrect the previous card's
     * CID. A known device number remains a valid identity baseline, so a
     * later device change is still detected while CID reads recover. */
    have_last_card_identity = sd_card_identity_is_known(have_last_mounted_device, current_card_cid);
    prompt_card_change_seen = false;
}

void poll_sd_card_hotplug(void) {
    static time_t last_check = 0;

    time_t now = time(NULL);
    if (last_check != 0 && now - last_check < SD_CARD_MOUNT_POLL_SECONDS) return;
    last_check = now;
    show_sd_repair_note();

    bool mount_observed = false;
    bool mounted = sd_card_root_is_mounted_observed(&mount_observed);
    sd_repair_kick_result_t repair = { false, false };
    bool repair_kick_attempted = false;
    if (mounted && !sd_repair_in_progress()) {
        /* Only a card the kernel remounted read-only is checked; see sd_fsck.h. */
        repair_kick_attempted = true;
        repair = sd_readonly_repair_kick(release_sd_handles_for_repair);
    }
    if (repair_kick_attempted) {
        if (repair.started || !sd_card_root_is_mounted()) {
            mounted = false;
            mount_observed = false;
            was_mounted = false;
            sd_handoff_pending = true;
            sd_forget_card_identity();
        } else if (repair.released_handles) {
            /* The check could not unmount. Reload the library that was
             * closed to make the unmount attempt. */
            was_mounted = false;
            sd_handoff_pending = true;
        }
    }
    show_sd_repair_note();
    if (sd_handoff_pending || sd_repair_in_progress()) {
        gui_folder_actions_invalidate_storage_targets();
        gui_folder_actions_cancel();
    }
    /* Only an attached mount that the kernel probes proved stale may bypass
     * the normal two-poll absence debounce. If the initial mount stat was
     * inconclusive, a successful retry is not evidence of a stale mount. */
    bool stale_mount = sd_card_stale_mount_confirmed(mount_observed, mounted);
    dev_t current_mounted_device = 0;
    /* The mount check above already stat'd this card. This reread can fail
     * on its own; a leftover 0 is not a different device number. */
    bool have_current_device = mounted && sd_card_mount_attached(&current_mounted_device);
    char current_card_cid[64];
    sd_card_read_cid(current_card_cid);
    bool replaced_between_polls = sd_card_identity_replaced(
        mounted, was_mounted, have_last_card_identity, have_last_mounted_device, last_mounted_device,
        have_current_device, current_mounted_device, last_card_cid, current_card_cid);
    if (replaced_between_polls) {
        sd_note_card_changed();
    } else if (!mounted) {
        library_prompt_requeue_visible();
    }
    /* Unconfirmed absence still cancels a scan and, below, blocks writes.
     * It does not latch sd_handoff_pending; that waits for confirmation. */
    if (sd_card_sample_cancels_scan(mounted, replaced_between_polls, library_rescan_active))
        gui_library_cancel_scan();
    if (replaced_between_polls && !library_rescan_active && current_card_cid[0]) {
        /* An old card may have been unmounted and a new one mounted wholly
         * between polls. The mount is valid, but the in-memory library and
         * queue still belong to the previous card. */
        library_thumbnails_cancel_warmup();
        gui_player_notify_sd_unmounted_immediate();
        bt_control_debug_capture_release(false);
        clear_removed_sd_library();
        was_mounted = false;
    }
    /* Node-missing is decided before umount changes `mounted`. A sysfs
     * read error is not node-missing; only ENOENT on both nodes is. */
    bool forced_removal = mounted && sd_card_device_nodes_confirmed_absent();
    if (stale_mount || forced_removal) {
        /* -l (lazy): the node's already gone, so there's no real device
         * left to flush to, and a plain umount would fail with EBUSY if
         * this app (or anything else) still has an fd open on a file under
         * MUSIC_ROOT_DIR from right before the card was pulled -- the same
         * reasoning mass_storage_removing.sh already applies to external
         * USB media on this firmware (see its own "umount -l" call). */
        char * argv[] = { (char *) "umount", (char *) "-l", (char *) MUSIC_ROOT_DIR, NULL };
        subprocess_run(argv, NULL, 0);
        mounted = false;
        if (library_rescan_active) gui_library_cancel_scan();
        sd_note_card_changed();
    }

    group_probe_storage_available = mounted;
    if (!mounted) {
        sd_hotplug_unmounted(stale_mount, forced_removal, replaced_between_polls);
        return;
    }
    sd_hotplug_mounted(replaced_between_polls, have_current_device,
                       current_mounted_device, current_card_cid);
}
#else
bool sd_card_root_is_mounted(void) {
    struct stat st;
    return stat(MUSIC_ROOT_DIR, &st) == 0 && S_ISDIR(st.st_mode);
}

library_prompt_card_status_t library_prompt_card_status(void) {
    return sd_card_root_is_mounted() ? LIBRARY_PROMPT_CARD_CURRENT : LIBRARY_PROMPT_CARD_UNKNOWN;
}

void library_prompt_prepare_cache_load_card(void) {
}

void poll_sd_card_hotplug(void) {
}
#endif

/* Actual "Format SD Card" sequence -- runs on a background thread (see
 * start_sd_format()/poll_sd_format() below), since fdisk/mkdosfs can take
 * real time on a slow card and this must never block the UI thread.
 *
 * UNVERIFIED ON REAL HARDWARE. Every command here was chosen from static
 * analysis of the real firmware's own busybox binary (`strings` confirmed
 * mkdosfs/fdisk/dd/umount/partprobe are all compiled-in applets, including
 * fdisk's own interactive prompt text, proving a standard util-linux-style
 * command menu), not from an actual run -- this is a MIPS binary and can't
 * be executed on the dev host, and no live device was available while this
 * was written. Test with a spare/non-critical card before trusting it with
 * a card that matters.
 *
 * Sequence: best-effort unmount, zero the first sector (so fdisk always
 * sees a blank disk regardless of whatever partition table, or lack of
 * one, the card came in with -- avoids needing to conditionally script
 * fdisk's own delete-partition flow for "already has a stale/foreign
 * table"), script fdisk to create one primary partition spanning the whole
 * disk typed W95 FAT32 (LBA), partprobe to force the kernel to notice the
 * new partition, wait for its device node to appear, mkdosfs it, then hand
 * off to the normal mount path and confirm it actually mounted. */
#ifndef HOST_BUILD
static bool sd_format_card_worker(void) {
    char * umount_argv[] = { (char *) "umount", (char *) "-l", (char *) MUSIC_ROOT_DIR, NULL };
    subprocess_run(umount_argv, NULL, 0);

    if (!sd_card_base_device_present()) return false; /* card pulled before/during format */

    char * dd_argv[] = { (char *) "dd", (char *) "if=/dev/zero", (char *) "of=/dev/mmcblk0",
                          (char *) "bs=512", (char *) "count=1", NULL };
    if (!subprocess_run_timeout(dd_argv, NULL, 0, 15000)) return false;

    pid_t fdisk_pid;
    int fdisk_write_fd;
    char * fdisk_argv[] = { (char *) "fdisk", (char *) "/dev/mmcblk0", NULL };
    if (!subprocess_popen_stdin(fdisk_argv, &fdisk_pid, &fdisk_write_fd)) return false;
    /* n(ew) -> p(rimary) -> partition 1 -> default first sector [Enter] ->
     * default last sector [Enter] -> t(ype) -> c (W95 FAT32, LBA) -> w(rite)+exit. */
    const char * fdisk_cmds = "n\np\n1\n\n\nt\nc\nw\n";
    ssize_t ignored = write(fdisk_write_fd, fdisk_cmds, strlen(fdisk_cmds));
    (void) ignored;
    close(fdisk_write_fd);
    subprocess_terminate(fdisk_pid); /* reaps it -- `w` alone isn't guaranteed to have taken effect yet */

    char * partprobe_argv[] = { (char *) "partprobe", (char *) "/dev/mmcblk0", NULL };
    subprocess_run_timeout(partprobe_argv, NULL, 0, 10000);

    bool got_partition = false;
    for (int i = 0; i < 20 && !got_partition; i++) {
        if (sd_card_device_node_present()) { got_partition = true; break; }
        usleep(250000);
    }
    if (!got_partition) return false;

    char * mkdosfs_argv[] = { (char *) "mkdosfs", (char *) "-F", (char *) "32", (char *) "-n",
                               (char *) "MUSIC", (char *) "/dev/mmcblk0p1", NULL };
    if (!subprocess_run_timeout(mkdosfs_argv, NULL, 0, 120000)) return false;

    mount_sd_card_if_needed();
    return sd_card_root_is_mounted();
}
#else
static bool sd_format_card_worker(void) {
    return false;
}
#endif

static pthread_t sd_format_thread;
static atomic_bool sd_format_done_flag = false;
static volatile bool sd_format_succeeded = false;

static void * sd_format_thread_func(void * arg) {
    (void) arg;
    sd_format_succeeded = sd_format_card_worker();
    atomic_store_explicit(&sd_format_done_flag, true, memory_order_release); /* written last -- poll_sd_format() only checks this flag */
    return NULL;
}

static void start_sd_format(void) {
    if (sd_repair_in_progress()) {
        show_error_toast(TR("SD card repair is still running"));
        return;
    }
    if (file_op_running()) {
        show_error_toast(TR("Another file operation is still running"));
        return;
    }
    gui_library_invalidate_boot_prompt();
    boot_warmup_pending = false;
    atomic_store_explicit(&sd_format_done_flag, false, memory_order_relaxed);
    sd_format_active = true;
    sd_format_token = gui_busy_show(TR("Formatting\nSD Card..."), "");
        if (pthread_create(&sd_format_thread, NULL, sd_format_thread_func, NULL) != 0) {
        sd_format_active = false;
        gui_busy_hide(sd_format_token);
        show_error_toast(TR("Thread launch failed"));
    }
}

void poll_sd_format(void) {
    if (!sd_format_active || !atomic_load_explicit(&sd_format_done_flag, memory_order_acquire)) return;

    sd_format_active = false;
    pthread_join(sd_format_thread, NULL);
    gui_busy_hide(sd_format_token);

    if (sd_format_succeeded) {
        show_error_toast(TR("SD card formatted"));
        sd_mount_fail_notified = false; /* give the freshly-formatted card a clean slate */
        if (!library_rescan_active) {
            start_library_auto_rescan();
            gui_folder_actions_invalidate_storage_targets();
            file_browser_reset_to_root();
        }
    } else {
        show_error_toast(TR("SD card format failed"));
    }
}

gui_popup_t sd_mount_failed_popup;
gui_popup_t sd_format_confirm_popup;

static void hide_sd_mount_failed_popup(void) {
    gui_popup_hide(&sd_mount_failed_popup);
}

static void hide_sd_format_confirm_popup(void) {
    gui_popup_hide(&sd_format_confirm_popup);
}

static void sd_mount_failed_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
}

static void sd_mount_failed_dismiss_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
}

static void sd_format_confirm_popup_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
}

static void sd_format_cancel_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
}

static void sd_mount_failed_format_btn_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_mount_failed_popup();
    gui_popup_show(&sd_format_confirm_popup);
}

static void sd_format_confirm_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_sd_format_confirm_popup();
    start_sd_format();
}

/* Displayed when persistent SD card mount failure is detected. */
#ifndef HOST_BUILD
static void show_sd_mount_failed_popup(void) {
    gui_popup_show(&sd_mount_failed_popup);
}
#endif

void build_sd_mount_failed_popup(void) {
    sd_mount_failed_popup.popup = build_confirm_popup(
        TR("SD card couldn't be read"), LV_LABEL_LONG_WRAP, NULL,
        TR("It may have no partition table or a file system this player can't use. "
        "Formatting will erase it and set it up for this player."),
        TR("Format SD Card"), lv_color_hex(GUI_BREEZE_NEGATIVE), sd_mount_failed_format_btn_cb, NULL, TR("Dismiss"),
        accent_lv_color(), sd_mount_failed_dismiss_cb, NULL, sd_mount_failed_popup_backdrop_cb,
        &sd_mount_failed_popup.backdrop);
}

void build_sd_format_confirm_popup(void) {
    sd_format_confirm_popup.popup = build_confirm_popup(
        TR("Erase and format SD card?"), LV_LABEL_LONG_WRAP, NULL,
        TR("This permanently deletes everything on the card. This cannot be undone."), TR("Format"),
        lv_color_hex(GUI_BREEZE_NEGATIVE), sd_format_confirm_cb, NULL, TR("Cancel"), accent_lv_color(), sd_format_cancel_cb,
        NULL, sd_format_confirm_popup_backdrop_cb, &sd_format_confirm_popup.backdrop);
}
bool gui_library_sd_format_is_active(void) {
    return sd_format_active;
}

void gui_library_sd_cancel_format(void) {
    if (sd_format_active) {
        pthread_join(sd_format_thread, NULL);
        sd_format_active = false;
        gui_busy_hide(sd_format_token);
    }
}
