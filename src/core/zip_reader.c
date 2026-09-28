#include "zip_reader.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "miniz_tinfl.h"

static const uint32_t crc_table[256] = {
    0x00000000u, 0x77073096u, 0xee0e612cu, 0x990951bau, 0x076dc419u, 0x706af48fu, 0xe963a535u, 0x9e6495a3u,
    0x0edb8832u, 0x79dcb8a4u, 0xe0d5e91eu, 0x97d2d988u, 0x09b64c2bu, 0x7eb17cbdu, 0xe7b82d07u, 0x90bf1d91u,
    0x1db71064u, 0x6ab020f2u, 0xf3b97148u, 0x84be41deu, 0x1adad47du, 0x6ddde4ebu, 0xf4d4b551u, 0x83d385c7u,
    0x136c9856u, 0x646ba8c0u, 0xfd62f97au, 0x8a65c9ecu, 0x14015c4fu, 0x63066cd9u, 0xfa0f3d63u, 0x8d080df5u,
    0x3b6e20c8u, 0x4c69105eu, 0xd56041e4u, 0xa2677172u, 0x3c03e4d1u, 0x4b04d447u, 0xd20d85fdu, 0xa50ab56bu,
    0x35b5a8fau, 0x42b2986cu, 0xdbbbc9d6u, 0xacbcf940u, 0x32d86ce3u, 0x45df5c75u, 0xdcd60dcfu, 0xabd13d59u,
    0x26d930acu, 0x51de003au, 0xc8d75180u, 0xbfd06116u, 0x21b4f4b5u, 0x56b3c423u, 0xcfba9599u, 0xb8bda50fu,
    0x2802b89eu, 0x5f058808u, 0xc60cd9b2u, 0xb10be924u, 0x2f6f7c87u, 0x58684c11u, 0xc1611dabu, 0xb6662d3du,
    0x76dc4190u, 0x01db7106u, 0x98d220bcu, 0xefd5102au, 0x71b18589u, 0x06b6b51fu, 0x9fbfe4a5u, 0xe8b8d433u,
    0x7807c9a2u, 0x0f00f934u, 0x9609a88eu, 0xe10e9818u, 0x7f6a0dbbu, 0x086d3d2du, 0x91646c97u, 0xe6635c01u,
    0x6b6b51f4u, 0x1c6c6162u, 0x856530d8u, 0xf262004eu, 0x6c0695edu, 0x1b01a57bu, 0x8208f4c1u, 0xf50fc457u,
    0x65b0d9c6u, 0x12b7e950u, 0x8bbeb8eau, 0xfcb9887cu, 0x62dd1ddfu, 0x15da2d49u, 0x8cd37cf3u, 0xfbd44c65u,
    0x4db26158u, 0x3ab551ceu, 0xa3bc0074u, 0xd4bb30e2u, 0x4adfa541u, 0x3dd895d7u, 0xa4d1c46du, 0xd3d6f4fbu,
    0x4369e96au, 0x346ed9fcu, 0xad678846u, 0xda60b8d0u, 0x44042d73u, 0x33031de5u, 0xaa0a4c5fu, 0xdd0d7cc9u,
    0x5005713cu, 0x270241aau, 0xbe0b1010u, 0xc90c2086u, 0x5768b525u, 0x206f85b3u, 0xb966d409u, 0xce61e49fu,
    0x5edef90eu, 0x29d9c998u, 0xb0d09822u, 0xc7d7a8b4u, 0x59b33d17u, 0x2eb40d81u, 0xb7bd5c3bu, 0xc0ba6cadu,
    0xedb88320u, 0x9abfb3b6u, 0x03b6e20cu, 0x74b1d29au, 0xead54739u, 0x9dd277afu, 0x04db2615u, 0x73dc1683u,
    0xe3630b12u, 0x94643b84u, 0x0d6d6a3eu, 0x7a6a5aa8u, 0xe40ecf0bu, 0x9309ff9du, 0x0a00ae27u, 0x7d079eb1u,
    0xf00f9344u, 0x8708a3d2u, 0x1e01f268u, 0x6906c2feu, 0xf762575du, 0x806567cbu, 0x196c3671u, 0x6e6b06e7u,
    0xfed41b76u, 0x89d32be0u, 0x10da7a5au, 0x67dd4accu, 0xf9b9df6fu, 0x8ebeeff9u, 0x17b7be43u, 0x60b08ed5u,
    0xd6d6a3e8u, 0xa1d1937eu, 0x38d8c2c4u, 0x4fdff252u, 0xd1bb67f1u, 0xa6bc5767u, 0x3fb506ddu, 0x48b2364bu,
    0xd80d2bdau, 0xaf0a1b4cu, 0x36034af6u, 0x41047a60u, 0xdf60efc3u, 0xa867df55u, 0x316e8eefu, 0x4669be79u,
    0xcb61b38cu, 0xbc66831au, 0x256fd2a0u, 0x5268e236u, 0xcc0c7795u, 0xbb0b4703u, 0x220216b9u, 0x5505262fu,
    0xc5ba3bbeu, 0xb2bd0b28u, 0x2bb45a92u, 0x5cb36a04u, 0xc2d7ffa7u, 0xb5d0cf31u, 0x2cd99e8bu, 0x5bdeae1du,
    0x9b64c2b0u, 0xec63f226u, 0x756aa39cu, 0x026d930au, 0x9c0906a9u, 0xeb0e363fu, 0x72076785u, 0x05005713u,
    0x95bf4a82u, 0xe2b87a14u, 0x7bb12baeu, 0x0cb61b38u, 0x92d28e9bu, 0xe5d5be0du, 0x7cdcefb7u, 0x0bdbdf21u,
    0x86d3d2d4u, 0xf1d4e242u, 0x68ddb3f8u, 0x1fda836eu, 0x81be16cdu, 0xf6b9265bu, 0x6fb077e1u, 0x18b74777u,
    0x88085ae6u, 0xff0f6a70u, 0x66063bcau, 0x11010b5cu, 0x8f659effu, 0xf862ae69u, 0x616bffd3u, 0x166ccf45u,
    0xa00ae278u, 0xd70dd2eeu, 0x4e048354u, 0x3903b3c2u, 0xa7672661u, 0xd06016f7u, 0x4969474du, 0x3e6e77dbu,
    0xaed16a4au, 0xd9d65adcu, 0x40df0b66u, 0x37d83bf0u, 0xa9bcae53u, 0xdebb9ec5u, 0x47b2cf7fu, 0x30b5ffe9u,
    0xbdbdf21cu, 0xcabac28au, 0x53b39330u, 0x24b4a3a6u, 0xbad03605u, 0xcdd70693u, 0x54de5729u, 0x23d967bfu,
    0xb3667a2eu, 0xc4614ab8u, 0x5d681b02u, 0x2a6f2b94u, 0xb40bbe37u, 0xc30c8ea1u, 0x5a05df1bu, 0x2d02ef8du,
};

uint32_t zip_crc32(uint32_t crc, const void * data, size_t len) {
    uint32_t c = crc ^ 0xFFFFFFFFu;
    const unsigned char * p = (const unsigned char *) data;
    for (size_t i = 0; i < len; i++) {
        c = crc_table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

const char * zip_status_reason(zip_status status) {
    switch (status) {
        case ZIP_OK: return "ok";
        case ZIP_ERR_IO: return "io_error";
        case ZIP_ERR_CORRUPT: return "corrupt";
        case ZIP_ERR_NOT_FOUND: return "not_found";
        case ZIP_ERR_ENCRYPTED: return "encrypted";
        case ZIP_ERR_ZIP64: return "zip64_unsupported";
        case ZIP_ERR_UNSUPPORTED_METHOD: return "unsupported_method";
        case ZIP_ERR_CRC: return "crc_mismatch";
        case ZIP_ERR_TOO_LARGE: return "entry_too_large";
        case ZIP_ERR_NOMEM: return "nomem";
        default: return "corrupt";
    }
}

void zip_free_names(char ** names, size_t count) {
    if (!names) return;
    for (size_t i = 0; i < count; i++) {
        free(names[i]);
    }
    free(names);
}

static inline uint16_t read_le16(const unsigned char * p) {
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static inline uint32_t read_le32(const unsigned char * p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) |
           ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static bool read_all(int fd, void * buf, size_t len, uint64_t offset) {
    if (offset > (uint64_t) INT64_MAX) return false;
    unsigned char * ptr = (unsigned char *) buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, ptr + done, len - done, (off_t) (offset + (uint64_t) done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        done += (size_t) n;
    }
    return true;
}

typedef struct {
    int fd;
    uint64_t file_size;
    uint16_t total_entries;
    uint32_t cd_size;
    unsigned char * cd_buf;
} zip_archive;

static void close_archive(zip_archive * arch) {
    if (arch->cd_buf) {
        free(arch->cd_buf);
        arch->cd_buf = NULL;
    }
    if (arch->fd >= 0) {
        close(arch->fd);
        arch->fd = -1;
    }
}

static zip_status open_and_read_cd(const char * path, zip_archive * arch) {
    arch->fd = -1;
    arch->file_size = 0;
    arch->total_entries = 0;
    arch->cd_size = 0;
    arch->cd_buf = NULL;

    /* O_NONBLOCK: a FIFO with no writer would otherwise block in open(),
     * before the regular-file check, and the UI could not be interrupted. */
    arch->fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (arch->fd < 0) return ZIP_ERR_IO;

    struct stat st;
    if (fstat(arch->fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close_archive(arch);
        return ZIP_ERR_IO;
    }
    arch->file_size = (uint64_t) st.st_size;
    if (arch->file_size < 22ULL) {
        close_archive(arch);
        return ZIP_ERR_CORRUPT;
    }

    uint64_t search_len = arch->file_size < (65536ULL + 22ULL) ? arch->file_size : (65536ULL + 22ULL);
    uint64_t search_start = arch->file_size - search_len;
    unsigned char * search_buf = malloc((size_t) search_len);
    if (!search_buf) {
        close_archive(arch);
        return ZIP_ERR_NOMEM;
    }
    if (!read_all(arch->fd, search_buf, (size_t) search_len, search_start)) {
        free(search_buf);
        close_archive(arch);
        return ZIP_ERR_IO;
    }

    int64_t eocd_idx = -1;
    for (int64_t i = (int64_t) (search_len - 22); i >= 0; i--) {
        if (read_le32(&search_buf[i]) == 0x06054b50u) {
            uint64_t e = search_start + (uint64_t) i;
            uint16_t comment_len = read_le16(&search_buf[i + 20]);
            /* Candidate is valid only when reaching EOF without trailing data or payload embedding. */
            if (e + 22ULL + (uint64_t) comment_len == arch->file_size) {
                eocd_idx = i;
                break;
            }
        }
    }
    if (eocd_idx < 0) {
        free(search_buf);
        close_archive(arch);
        return ZIP_ERR_CORRUPT;
    }

    uint64_t eocd_off = search_start + (uint64_t) eocd_idx;
    if (eocd_off >= 20ULL) {
        uint32_t loc_sig = 0;
        if (eocd_idx >= 20) {
            loc_sig = read_le32(&search_buf[eocd_idx - 20]);
        } else {
            unsigned char loc_bytes[4];
            if (!read_all(arch->fd, loc_bytes, 4, eocd_off - 20ULL)) {
                free(search_buf);
                close_archive(arch);
                return ZIP_ERR_IO;
            }
            loc_sig = read_le32(loc_bytes);
        }
        if (loc_sig == 0x07064b50u) {
            free(search_buf);
            close_archive(arch);
            return ZIP_ERR_ZIP64;
        }
    }

    uint16_t disk = read_le16(&search_buf[eocd_idx + 4]);
    uint16_t disk_with_cd = read_le16(&search_buf[eocd_idx + 6]);
    uint16_t disk_entries = read_le16(&search_buf[eocd_idx + 8]);
    uint16_t total_entries = read_le16(&search_buf[eocd_idx + 10]);
    uint32_t cd_size = read_le32(&search_buf[eocd_idx + 12]);
    uint32_t cd_offset = read_le32(&search_buf[eocd_idx + 16]);

    free(search_buf);

    if (disk == 0xFFFFu || disk_with_cd == 0xFFFFu ||
        disk_entries == 0xFFFFu || total_entries == 0xFFFFu ||
        cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) {
        close_archive(arch);
        return ZIP_ERR_ZIP64;
    }

    if (disk != 0 || disk_with_cd != 0) {
        close_archive(arch);
        return ZIP_ERR_CORRUPT;
    }
    if (disk_entries != total_entries) {
        close_archive(arch);
        return ZIP_ERR_CORRUPT;
    }

    uint64_t cd_off = (uint64_t) cd_offset;
    uint64_t cd_sz = (uint64_t) cd_size;
    if (total_entries > ZIP_MAX_ENTRIES || cd_sz > (uint64_t) ZIP_MAX_CD_BYTES) {
        close_archive(arch);
        return ZIP_ERR_CORRUPT;
    }
    if (cd_off > arch->file_size || cd_sz > arch->file_size - cd_off) {
        close_archive(arch);
        return ZIP_ERR_CORRUPT;
    }
    if ((cd_size == 0 && total_entries > 0) || (total_entries == 0 && cd_size > 0)) {
        close_archive(arch);
        return ZIP_ERR_CORRUPT;
    }

    arch->total_entries = total_entries;
    arch->cd_size = cd_size;
    if (cd_size > 0) {
        arch->cd_buf = malloc(cd_size);
        if (!arch->cd_buf) {
            close_archive(arch);
            return ZIP_ERR_NOMEM;
        }
        if (!read_all(arch->fd, arch->cd_buf, cd_size, cd_off)) {
            close_archive(arch);
            return ZIP_ERR_IO;
        }
    }

    return ZIP_OK;
}

zip_status zip_list_entries(const char * path, char *** names, size_t * count) {
    if (names) *names = NULL;
    if (count) *count = 0;
    if (!path || !names || !count) return ZIP_ERR_IO;

    zip_archive arch;
    zip_status st = open_and_read_cd(path, &arch);
    if (st != ZIP_OK) return st;

    if (arch.total_entries == 0) {
        close_archive(&arch);
        *names = NULL;
        *count = 0;
        return ZIP_OK;
    }

    char ** name_list = malloc(sizeof(char *) * arch.total_entries);
    if (!name_list) {
        close_archive(&arch);
        return ZIP_ERR_NOMEM;
    }

    uint64_t cd_pos = 0;
    uint64_t cd_sz = (uint64_t) arch.cd_size;
    for (size_t i = 0; i < arch.total_entries; i++) {
        if (cd_sz - cd_pos < 46ULL) {
            zip_free_names(name_list, i);
            close_archive(&arch);
            return ZIP_ERR_CORRUPT;
        }
        const unsigned char * h = &arch.cd_buf[cd_pos];
        if (read_le32(h) != 0x02014b50u) {
            zip_free_names(name_list, i);
            close_archive(&arch);
            return ZIP_ERR_CORRUPT;
        }
        uint16_t name_len = read_le16(h + 28);
        uint16_t extra_len = read_le16(h + 30);
        uint16_t comm_len = read_le16(h + 32);
        uint64_t rec_len = 46ULL + (uint64_t) name_len + (uint64_t) extra_len + (uint64_t) comm_len;
        if (rec_len > cd_sz - cd_pos) {
            zip_free_names(name_list, i);
            close_archive(&arch);
            return ZIP_ERR_CORRUPT;
        }

        const unsigned char * extra_ptr = h + 46 + name_len;
        size_t extra_idx = 0;
        while (extra_idx < extra_len) {
            if (extra_len - extra_idx < 4) {
                zip_free_names(name_list, i);
                close_archive(&arch);
                return ZIP_ERR_CORRUPT;
            }
            uint16_t extra_sz = read_le16(extra_ptr + extra_idx + 2);
            extra_idx += 4;
            if ((size_t) extra_sz > extra_len - extra_idx) {
                zip_free_names(name_list, i);
                close_archive(&arch);
                return ZIP_ERR_CORRUPT;
            }
            extra_idx += extra_sz;
        }

        char * name = malloc((size_t) name_len + 1);
        if (!name) {
            zip_free_names(name_list, i);
            close_archive(&arch);
            return ZIP_ERR_NOMEM;
        }
        memcpy(name, h + 46, name_len);
        name[name_len] = '\0';
        name_list[i] = name;

        cd_pos += rec_len;
    }

    if (cd_pos != cd_sz) {
        zip_free_names(name_list, arch.total_entries);
        close_archive(&arch);
        return ZIP_ERR_CORRUPT;
    }

    close_archive(&arch);
    *names = name_list;
    *count = arch.total_entries;
    return ZIP_OK;
}

zip_status zip_read_entry(const char * path, const char * entry_name,
                          unsigned char ** out, size_t * out_len, zip_entry_info * info) {
    if (out) *out = NULL;
    if (out_len) *out_len = 0;
    if (info) memset(info, 0, sizeof(*info));

    if (!path || !out || !out_len || !info) return ZIP_ERR_IO;
    if (!entry_name) return ZIP_ERR_NOT_FOUND;

    zip_archive arch;
    zip_status st = open_and_read_cd(path, &arch);
    if (st != ZIP_OK) return st;

    size_t query_len = strlen(entry_name);
    uint64_t cd_pos = 0;
    uint64_t cd_sz = (uint64_t) arch.cd_size;
    bool found = false;

    uint16_t match_flags = 0;
    uint16_t match_method = 0;
    uint32_t match_crc = 0;
    uint32_t match_comp = 0;
    uint32_t match_uncomp = 0;
    uint16_t match_name_len = 0;
    uint32_t match_local_off = 0;
    bool match_zip64 = false;
    uint64_t match_cd_pos = 0;

    for (size_t i = 0; i < arch.total_entries; i++) {
        if (cd_sz - cd_pos < 46ULL) {
            close_archive(&arch);
            return ZIP_ERR_CORRUPT;
        }
        const unsigned char * h = &arch.cd_buf[cd_pos];
        if (read_le32(h) != 0x02014b50u) {
            close_archive(&arch);
            return ZIP_ERR_CORRUPT;
        }
        uint16_t flags = read_le16(h + 8);
        uint16_t method = read_le16(h + 10);
        uint32_t crc = read_le32(h + 16);
        uint32_t comp_size = read_le32(h + 20);
        uint32_t uncomp_size = read_le32(h + 24);
        uint16_t name_len = read_le16(h + 28);
        uint16_t extra_len = read_le16(h + 30);
        uint16_t comm_len = read_le16(h + 32);
        uint32_t local_header_offset = read_le32(h + 42);

        uint64_t rec_len = 46ULL + (uint64_t) name_len + (uint64_t) extra_len + (uint64_t) comm_len;
        if (rec_len > cd_sz - cd_pos) {
            close_archive(&arch);
            return ZIP_ERR_CORRUPT;
        }

        bool is_zip64 = (comp_size == 0xFFFFFFFFu || uncomp_size == 0xFFFFFFFFu ||
                         local_header_offset == 0xFFFFFFFFu);

        const unsigned char * extra_ptr = h + 46 + name_len;
        size_t extra_idx = 0;
        while (extra_idx < extra_len) {
            if (extra_len - extra_idx < 4) {
                close_archive(&arch);
                return ZIP_ERR_CORRUPT;
            }
            uint16_t extra_id = read_le16(extra_ptr + extra_idx);
            uint16_t extra_sz = read_le16(extra_ptr + extra_idx + 2);
            extra_idx += 4;
            if ((size_t) extra_sz > extra_len - extra_idx) {
                close_archive(&arch);
                return ZIP_ERR_CORRUPT;
            }
            if (extra_id == 0x0001u) {
                is_zip64 = true;
            }
            extra_idx += extra_sz;
        }

        if (!found && (size_t) name_len == query_len && memcmp(h + 46, entry_name, query_len) == 0) {
            found = true;
            match_flags = flags;
            match_method = method;
            match_crc = crc;
            match_comp = comp_size;
            match_uncomp = uncomp_size;
            match_name_len = name_len;
            match_local_off = local_header_offset;
            match_zip64 = is_zip64;
            match_cd_pos = cd_pos;
        }

        cd_pos += rec_len;
    }

    if (cd_pos != cd_sz) {
        close_archive(&arch);
        return ZIP_ERR_CORRUPT;
    }

    if (!found) {
        close_archive(&arch);
        return ZIP_ERR_NOT_FOUND;
    }

    if ((match_flags & 0x0001u) || (match_flags & 0x0040u)) {
        close_archive(&arch);
        return ZIP_ERR_ENCRYPTED;
    }
    if (match_zip64) {
        close_archive(&arch);
        return ZIP_ERR_ZIP64;
    }
    if (match_method != 0 && match_method != 8) {
        close_archive(&arch);
        return ZIP_ERR_UNSUPPORTED_METHOD;
    }
    if (match_uncomp > ZIP_MAX_UNCOMPRESSED_BYTES) {
        close_archive(&arch);
        return ZIP_ERR_TOO_LARGE;
    }
    if (match_comp > ZIP_MAX_COMPRESSED_BYTES) {
        close_archive(&arch);
        return ZIP_ERR_TOO_LARGE;
    }

    uint64_t loc_off = (uint64_t) match_local_off;
    if (loc_off > arch.file_size || 30ULL > arch.file_size - loc_off) {
        close_archive(&arch);
        return ZIP_ERR_CORRUPT;
    }
    unsigned char loc_hdr[30];
    if (!read_all(arch.fd, loc_hdr, 30, loc_off)) {
        close_archive(&arch);
        return ZIP_ERR_IO;
    }
    if (read_le32(loc_hdr) != 0x04034b50u) {
        close_archive(&arch);
        return ZIP_ERR_CORRUPT;
    }

    uint16_t loc_flags = read_le16(loc_hdr + 6);
    uint16_t loc_method = read_le16(loc_hdr + 8);
    uint32_t loc_crc = read_le32(loc_hdr + 14);
    uint32_t loc_comp = read_le32(loc_hdr + 18);
    uint32_t loc_uncomp = read_le32(loc_hdr + 22);
    uint16_t loc_name_len = read_le16(loc_hdr + 26);
    uint16_t loc_extra_len = read_le16(loc_hdr + 28);

    if (loc_name_len != match_name_len || loc_method != match_method) {
        close_archive(&arch);
        return ZIP_ERR_CORRUPT;
    }
    /* Bit 3 sets data descriptor; otherwise local sizes and CRC must equal CD values. */
    if ((loc_flags & 0x0008u) == 0) {
        if (loc_crc != match_crc || loc_comp != match_comp || loc_uncomp != match_uncomp) {
            close_archive(&arch);
            return ZIP_ERR_CORRUPT;
        }
    }

    uint64_t loc_name_off = loc_off + 30ULL;
    if (loc_name_off > arch.file_size || (uint64_t) loc_name_len > arch.file_size - loc_name_off) {
        close_archive(&arch);
        return ZIP_ERR_CORRUPT;
    }
    if (loc_name_len > 0) {
        unsigned char * loc_name = malloc(loc_name_len);
        if (!loc_name) {
            close_archive(&arch);
            return ZIP_ERR_NOMEM;
        }
        if (!read_all(arch.fd, loc_name, loc_name_len, loc_name_off)) {
            free(loc_name);
            close_archive(&arch);
            return ZIP_ERR_IO;
        }
        if (memcmp(loc_name, arch.cd_buf + match_cd_pos + 46, loc_name_len) != 0) {
            free(loc_name);
            close_archive(&arch);
            return ZIP_ERR_CORRUPT;
        }
        free(loc_name);
    }

    uint64_t data_off = loc_off + 30ULL + (uint64_t) loc_name_len + (uint64_t) loc_extra_len;
    if (data_off > arch.file_size || (uint64_t) match_comp > arch.file_size - data_off) {
        close_archive(&arch);
        return ZIP_ERR_CORRUPT;
    }

    int fd = arch.fd;
    arch.fd = -1;
    close_archive(&arch);

    if ((loc_flags & 0x0001u) || (loc_flags & 0x0040u)) {
        close(fd);
        *out = NULL;
        return ZIP_ERR_ENCRYPTED;
    }

    if (match_method == 0) {
        if (match_comp != match_uncomp) {
            close(fd);
            return ZIP_ERR_CORRUPT;
        }
        if (match_uncomp == 0) {
            close(fd);
            if (match_crc != 0) return ZIP_ERR_CRC;
            /* Empty entry: return NULL with out_len 0. */
            *out = NULL;
            *out_len = 0;
            info->method = 0;
            info->compressed = 0;
            info->uncompressed = 0;
            return ZIP_OK;
        }
        unsigned char * payload = malloc(match_uncomp);
        if (!payload) {
            close(fd);
            return ZIP_ERR_NOMEM;
        }
        if (!read_all(fd, payload, match_uncomp, data_off)) {
            free(payload);
            close(fd);
            return ZIP_ERR_IO;
        }
        close(fd);
        uint32_t calc_crc = zip_crc32(0, payload, match_uncomp);
        if (calc_crc != match_crc) {
            free(payload);
            return ZIP_ERR_CRC;
        }
        *out = payload;
        *out_len = match_uncomp;
        info->method = 0;
        info->compressed = match_comp;
        info->uncompressed = match_uncomp;
        return ZIP_OK;
    }

    /* match_method == 8 (raw deflate) */
    if (match_uncomp == 0) {
        if (match_comp == 0) {
            close(fd);
            if (match_crc != 0) return ZIP_ERR_CRC;
            /* Empty entry: return NULL with out_len 0. */
            *out = NULL;
            *out_len = 0;
            info->method = 8;
            info->compressed = 0;
            info->uncompressed = 0;
            return ZIP_OK;
        }
        unsigned char * comp_buf = malloc(match_comp);
        if (!comp_buf) {
            close(fd);
            return ZIP_ERR_NOMEM;
        }
        if (!read_all(fd, comp_buf, match_comp, data_off)) {
            free(comp_buf);
            close(fd);
            return ZIP_ERR_IO;
        }
        close(fd);

        tinfl_decompressor * decomp = malloc(sizeof(tinfl_decompressor));
        if (!decomp) {
            free(comp_buf);
            return ZIP_ERR_NOMEM;
        }
        tinfl_init(decomp);

        unsigned char dummy = 0;
        size_t in_bytes = match_comp;
        size_t out_bytes = 0;
        tinfl_status status = tinfl_decompress(decomp, comp_buf, &in_bytes, &dummy, &dummy, &out_bytes,
                                               TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        free(decomp);
        free(comp_buf);

        if (status == TINFL_STATUS_DONE && out_bytes == 0) {
            if (match_crc != 0) return ZIP_ERR_CRC;
            /* Empty entry: return NULL with out_len 0. */
            *out = NULL;
            *out_len = 0;
            info->method = 8;
            info->compressed = match_comp;
            info->uncompressed = 0;
            return ZIP_OK;
        }
        return ZIP_ERR_CORRUPT;
    }

    if (match_comp == 0) {
        close(fd);
        return ZIP_ERR_CORRUPT;
    }
    unsigned char * comp_buf = malloc(match_comp);
    if (!comp_buf) {
        close(fd);
        return ZIP_ERR_NOMEM;
    }
    if (!read_all(fd, comp_buf, match_comp, data_off)) {
        free(comp_buf);
        close(fd);
        return ZIP_ERR_IO;
    }
    close(fd);

    tinfl_decompressor * decomp = malloc(sizeof(tinfl_decompressor));
    if (!decomp) {
        free(comp_buf);
        return ZIP_ERR_NOMEM;
    }
    tinfl_init(decomp);

    unsigned char * out_buf = malloc(match_uncomp);
    if (!out_buf) {
        free(decomp);
        free(comp_buf);
        return ZIP_ERR_NOMEM;
    }

    size_t in_bytes = match_comp;
    size_t out_bytes = match_uncomp;
    tinfl_status status = tinfl_decompress(decomp, comp_buf, &in_bytes, out_buf, out_buf, &out_bytes,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    free(decomp);
    free(comp_buf);

    if (status == TINFL_STATUS_DONE && out_bytes == match_uncomp) {
        uint32_t calc_crc = zip_crc32(0, out_buf, match_uncomp);
        if (calc_crc != match_crc) {
            free(out_buf);
            return ZIP_ERR_CRC;
        }
        *out = out_buf;
        *out_len = match_uncomp;
        info->method = 8;
        info->compressed = match_comp;
        info->uncompressed = match_uncomp;
        return ZIP_OK;
    }

    free(out_buf);
    if (status == TINFL_STATUS_HAS_MORE_OUTPUT) {
        if (match_uncomp == ZIP_MAX_UNCOMPRESSED_BYTES) return ZIP_ERR_TOO_LARGE;
        return ZIP_ERR_CORRUPT;
    }
    return ZIP_ERR_CORRUPT;
}
