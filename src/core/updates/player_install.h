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
 * Online updates require a trusted SHA-256 and run preflight. Board-specific
 * online assets may lack a board marker for compatibility, but a marker for
 * another board is always rejected. The canonical SD update is archived if present. */
bool player_install_online_fd(int fd, uint64_t size, const char *expected_sha256,
                              char *error, size_t error_size);

/* SD callers must first pass player_install_preflight_fd with allow_missing_board_marker
 * false on the same descriptor and retain the matching path snapshot.
 * The installer rechecks inode, metadata and ELF, hashes the source itself,
 * and archives the SD source before commit. The snapshot supplies the size. */
bool player_install_sd_fd(int fd, const char *sd_path, const struct stat *snapshot,
                          char *error, size_t error_size);

/* Hash a pinned descriptor, bounded to PLAYER_INSTALL_MAX_BYTES. */
bool player_install_sha256_fd(int fd, uint64_t max_size, uint64_t *size_out, char hex[65]);

#endif
