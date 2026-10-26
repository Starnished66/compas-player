#ifndef PLAYER_INSTALL_H
#define PLAYER_INSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>

#ifdef HOST_BUILD
#define PLAYER_INSTALL_DATA_ROOT "./build/player-update-host"
#define PLAYER_INSTALL_SD_ROOT "./music"
#else
#define PLAYER_INSTALL_DATA_ROOT "/usr/data"
#define PLAYER_INSTALL_SD_ROOT "/data/mnt/sd_0"
#endif

#define PLAYER_INSTALL_DESTINATION PLAYER_INSTALL_DATA_ROOT "/compas_player"
#define PLAYER_INSTALL_SD_SOURCE PLAYER_INSTALL_SD_ROOT "/.compas/compas_player"
#define PLAYER_INSTALL_MAX_BYTES (32u * 1024u * 1024u)

const char *player_install_board_asset(void);

/* Preflight a pinned source before splash or internal-storage writes. */
bool player_install_preflight_fd(int fd, bool allow_missing_board_marker, char *error,
                                 size_t error_size);

/* Install a verified MIPS32LE/O32 player by same-filesystem atomic rename.
 * Online updates pass a mandatory trusted SHA-256. SD updates may pass NULL
 * to have the module hash the pinned source itself. SD updates require this build's embedded
 * board marker; verified board-specific online assets may lack one for
 * compatibility, but a marker for another board is always rejected.
 * Set preflighted only after player_install_preflight_fd succeeded on this
 * same descriptor and the caller retained the matching SD path snapshot.
 * The installer still rechecks inode, metadata and ELF before copying.
 * When sd_source_path is supplied, its pinned snapshot is archived before
 * commit so the legacy loader cannot reinstall it on the next boot. */
bool player_install_fd(int source_fd, uint64_t size, const char *expected_sha256,
                       bool allow_missing_board_marker, bool preflighted,
                       const char *sd_source_path, const struct stat *sd_snapshot, char *error,
                       size_t error_size);

/* Hash a pinned descriptor, bounded to PLAYER_INSTALL_MAX_BYTES. */
bool player_install_sha256_fd(int fd, uint64_t max_size, uint64_t *size_out, char hex[65]);

#endif
