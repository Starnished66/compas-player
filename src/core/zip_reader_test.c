#include "zip_reader.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int failures = 0;
static char test_dir[256];

static void expect(int condition, const char * what) {
    if (condition) return;
    fprintf(stderr, "zip reader: %s\n", what);
    failures++;
}

static void cleanup(void) {
    if (test_dir[0]) {
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", test_dir);
        int rc = system(cmd);
        (void) rc;
    }
}

static unsigned char * read_file_bytes(const char * path, size_t * len) {
    FILE * f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    unsigned char * buf = malloc((size_t) sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t) sz, f) != (size_t) sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *len = (size_t) sz;
    return buf;
}

static bool write_file_bytes(const char * path, const unsigned char * buf, size_t len) {
    FILE * f = fopen(path, "wb");
    if (!f) return false;
    if (fwrite(buf, 1, len, f) != len) {
        fclose(f);
        return false;
    }
    fclose(f);
    return true;
}

static ssize_t find_bytes(const unsigned char * hay, size_t hay_len, const unsigned char * needle, size_t needle_len) {
    if (needle_len > hay_len) return -1;
    for (size_t i = 0; i <= hay_len - needle_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) return (ssize_t) i;
    }
    return -1;
}

int main(void) {
    snprintf(test_dir, sizeof(test_dir), "/tmp/zip_reader_test_XXXXXX");
    if (!mkdtemp(test_dir)) {
        perror("mkdtemp");
        return 1;
    }
    atexit(cleanup);

    char path[PATH_MAX];
    char cmd[PATH_MAX + 512];
    unsigned char * out = NULL;
    size_t out_len = 0;
    zip_entry_info info;
    zip_status st;
    char ** names = NULL;
    size_t count = 0;

    /* 1. Stored entry round-trip, including binary bytes with an interior NUL in the payload. */
    snprintf(path, sizeof(path), "%s/case1.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('bin.dat', b'abc\\x00def\\x00\\x01\\x02ghi'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 1: create zip");
    out = NULL;
    out_len = 0;
    memset(&info, 0, sizeof(info));
    st = zip_read_entry(path, "bin.dat", &out, &out_len, &info);
    expect(st == ZIP_OK, "case 1: zip_read_entry returns ZIP_OK");
    expect(out_len == 13, "case 1: out_len == 13");
    expect(out && memcmp(out, "abc\0def\0\1\2ghi", 13) == 0, "case 1: payload matches");
    expect(info.method == 0, "case 1: info.method == 0");
    expect(info.compressed == 13, "case 1: info.compressed == 13");
    expect(info.uncompressed == 13, "case 1: info.uncompressed == 13");
    free(out);

    /* 2. Deflated entry round-trip of a few KB of text. */
    snprintf(path, sizeof(path), "%s/case2.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_DEFLATED); "
             "z.writestr('text.txt', ('The quick brown fox jumps over the lazy dog.\\n' * 100).encode('utf-8')); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 2: create zip");
    out = NULL;
    out_len = 0;
    memset(&info, 0, sizeof(info));
    st = zip_read_entry(path, "text.txt", &out, &out_len, &info);
    expect(st == ZIP_OK, "case 2: zip_read_entry returns ZIP_OK");
    expect(out_len == 4500, "case 2: out_len == 4500");
    expect(info.method == 8, "case 2: info.method == 8");
    expect(info.uncompressed == 4500, "case 2: info.uncompressed == 4500");
    expect(info.compressed > 0 && info.compressed < 4500, "case 2: info.compressed < 4500");
    if (out && out_len == 4500) {
        bool match = true;
        const char * pat = "The quick brown fox jumps over the lazy dog.\n";
        for (int i = 0; i < 100; i++) {
            if (memcmp(out + i * 45, pat, 45) != 0) {
                match = false;
                break;
            }
        }
        expect(match, "case 2: text content matches pattern");
    }
    free(out);

    /* 3. Several names in one archive: exact match, proper prefix is NOT_FOUND, subdirectory match. */
    snprintf(path, sizeof(path), "%s/case3.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w'); "
             "z.writestr('OEBPS/', b''); "
             "z.writestr('OEBPS/chapter1.xhtml', b'<p>chap1</p>'); "
             "z.writestr('chapter', b'<p>chap</p>'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 3: create zip");
    names = NULL;
    count = 0;
    st = zip_list_entries(path, &names, &count);
    expect(st == ZIP_OK, "case 3: list entries returns ZIP_OK");
    expect(count == 3, "case 3: count == 3");
    if (names && count == 3) {
        expect(strcmp(names[0], "OEBPS/") == 0, "case 3: name 0 matches");
        expect(strcmp(names[1], "OEBPS/chapter1.xhtml") == 0, "case 3: name 1 matches");
        expect(strcmp(names[2], "chapter") == 0, "case 3: name 2 matches");
    }
    zip_free_names(names, count);

    out = NULL;
    st = zip_read_entry(path, "chap", &out, &out_len, &info);
    expect(st == ZIP_ERR_NOT_FOUND, "case 3: proper prefix 'chap' returns ZIP_ERR_NOT_FOUND");
    st = zip_read_entry(path, "chapter1.xhtml", &out, &out_len, &info);
    expect(st == ZIP_ERR_NOT_FOUND, "case 3: 'chapter1.xhtml' without prefix returns ZIP_ERR_NOT_FOUND");
    st = zip_read_entry(path, "chapter", &out, &out_len, &info);
    expect(st == ZIP_OK && out_len == 11, "case 3: 'chapter' reads successfully");
    free(out);
    st = zip_read_entry(path, "OEBPS/chapter1.xhtml", &out, &out_len, &info);
    expect(st == ZIP_OK && out_len == 12, "case 3: 'OEBPS/chapter1.xhtml' reads successfully");
    free(out);

    /* 4. Empty stored entry: ZIP_OK, out NULL, len 0, crc accepted. */
    snprintf(path, sizeof(path), "%s/case4.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('empty.txt', b''); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 4: create zip");
    out = (unsigned char *) 0x1234;
    out_len = 999;
    st = zip_read_entry(path, "empty.txt", &out, &out_len, &info);
    expect(st == ZIP_OK, "case 4: empty stored entry returns ZIP_OK");
    expect(out == NULL, "case 4: out is NULL");
    expect(out_len == 0, "case 4: out_len == 0");
    expect(info.uncompressed == 0, "case 4: info.uncompressed == 0");
    expect(info.compressed == 0, "case 4: info.compressed == 0");

    /* 5. Missing entry -> not_found. Missing file -> io_error. NULL args do not crash. */
    st = zip_read_entry(path, "nonexistent", &out, &out_len, &info);
    expect(st == ZIP_ERR_NOT_FOUND, "case 5: nonexistent entry returns ZIP_ERR_NOT_FOUND");
    st = zip_read_entry("/tmp/does_not_exist_compas_test.zip", "empty.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_IO, "case 5: nonexistent file returns ZIP_ERR_IO");
    st = zip_read_entry(NULL, "empty.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_IO, "case 5: NULL path returns ZIP_ERR_IO");
    st = zip_read_entry(path, NULL, &out, &out_len, &info);
    expect(st == ZIP_ERR_NOT_FOUND, "case 5: NULL entry_name returns ZIP_ERR_NOT_FOUND");
    st = zip_read_entry(path, "empty.txt", NULL, &out_len, &info);
    expect(st == ZIP_ERR_IO, "case 5: NULL out returns ZIP_ERR_IO");
    st = zip_read_entry(path, "empty.txt", &out, NULL, &info);
    expect(st == ZIP_ERR_IO, "case 5: NULL out_len returns ZIP_ERR_IO");
    st = zip_read_entry(path, "empty.txt", &out, &out_len, NULL);
    expect(st == ZIP_ERR_IO, "case 5: NULL info returns ZIP_ERR_IO");
    st = zip_list_entries(NULL, &names, &count);
    expect(st == ZIP_ERR_IO, "case 5: list NULL path returns ZIP_ERR_IO");
    st = zip_list_entries(path, NULL, &count);
    expect(st == ZIP_ERR_IO, "case 5: list NULL names returns ZIP_ERR_IO");
    st = zip_list_entries(path, &names, NULL);
    expect(st == ZIP_ERR_IO, "case 5: list NULL count returns ZIP_ERR_IO");
    zip_free_names(NULL, 0);

    /* 6. CRC mismatch: stored entry with a flipped payload byte. */
    snprintf(path, sizeof(path), "%s/case6.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('hello.txt', b'hello world'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 6: create zip");
    size_t file_len = 0;
    unsigned char * file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 6: read file bytes");
    if (file_bytes) {
        ssize_t pos = find_bytes(file_bytes, file_len, (const unsigned char *) "hello world", 11);
        expect(pos >= 0, "case 6: find payload");
        if (pos >= 0) {
            file_bytes[pos] = 'x';
            expect(write_file_bytes(path, file_bytes, file_len), "case 6: write modified file");
        }
        free(file_bytes);
    }
    out = (unsigned char *) 0x1234;
    st = zip_read_entry(path, "hello.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_CRC, "case 6: flipped payload byte returns ZIP_ERR_CRC");
    expect(strcmp(zip_status_reason(st), "crc_mismatch") == 0, "case 6: reason is crc_mismatch");
    expect(out == NULL, "case 6: out is NULL on crc error");

    /* 7. Truncated file (middle of payload, and shorter than 22 bytes). */
    snprintf(path, sizeof(path), "%s/case7a.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('data.bin', b'X' * 200); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 7a: create zip");
    file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 7a: read file");
    if (file_bytes) {
        /* Truncate file in the middle of payload (around offset 40) */
        expect(write_file_bytes(path, file_bytes, 40), "case 7a: truncate file");
        free(file_bytes);
    }
    st = zip_read_entry(path, "data.bin", &out, &out_len, &info);
    expect(st == ZIP_ERR_CORRUPT, "case 7a: truncated in payload returns ZIP_ERR_CORRUPT");

    snprintf(path, sizeof(path), "%s/case7b.zip", test_dir);
    unsigned char tiny[10] = "123456789";
    expect(write_file_bytes(path, tiny, 10), "case 7b: write 10 bytes file");
    st = zip_read_entry(path, "data.bin", &out, &out_len, &info);
    expect(st == ZIP_ERR_CORRUPT, "case 7b: < 22 bytes returns ZIP_ERR_CORRUPT");
    st = zip_list_entries(path, &names, &count);
    expect(st == ZIP_ERR_CORRUPT, "case 7b: list on < 22 bytes returns ZIP_ERR_CORRUPT");

    /* 8. Encrypted flag bit 0 set -> encrypted, output pointer stays NULL. */
    snprintf(path, sizeof(path), "%s/case8.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('secret.txt', b'secret text'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 8: create zip");
    file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 8: read file");
    if (file_bytes) {
        const unsigned char cd_sig[4] = {0x50, 0x4b, 0x01, 0x02};
        ssize_t cd_pos = find_bytes(file_bytes, file_len, cd_sig, 4);
        expect(cd_pos >= 0, "case 8: find CD header");
        if (cd_pos >= 0) {
            file_bytes[cd_pos + 8] |= 0x01; /* Set bit 0 of flags in CD header */
            expect(write_file_bytes(path, file_bytes, file_len), "case 8: write file");
        }
        free(file_bytes);
    }
    out = (unsigned char *) 0x1234;
    st = zip_read_entry(path, "secret.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_ENCRYPTED, "case 8: encrypted entry returns ZIP_ERR_ENCRYPTED");
    expect(strcmp(zip_status_reason(st), "encrypted") == 0, "case 8: reason is encrypted");
    expect(out == NULL, "case 8: out remains NULL");

    /* 9. Method 12 -> unsupported_method. */
    snprintf(path, sizeof(path), "%s/case9.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('bzip2.txt', b'some data'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 9: create zip");
    file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 9: read file");
    if (file_bytes) {
        const unsigned char cd_sig[4] = {0x50, 0x4b, 0x01, 0x02};
        ssize_t cd_pos = find_bytes(file_bytes, file_len, cd_sig, 4);
        expect(cd_pos >= 0, "case 9: find CD header");
        if (cd_pos >= 0) {
            file_bytes[cd_pos + 10] = 12; /* method = 12 */
            file_bytes[cd_pos + 11] = 0;
            expect(write_file_bytes(path, file_bytes, file_len), "case 9: write file");
        }
        free(file_bytes);
    }
    st = zip_read_entry(path, "bzip2.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_UNSUPPORTED_METHOD, "case 9: method 12 returns ZIP_ERR_UNSUPPORTED_METHOD");
    expect(strcmp(zip_status_reason(st), "unsupported_method") == 0, "case 9: reason is unsupported_method");

    /* 10. ZIP64: uncompressed size field 0xFFFFFFFF, and locator signature before EOCD. */
    snprintf(path, sizeof(path), "%s/case10a.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('huge.txt', b'content'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 10a: create zip");
    file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 10a: read file");
    if (file_bytes) {
        const unsigned char cd_sig[4] = {0x50, 0x4b, 0x01, 0x02};
        ssize_t cd_pos = find_bytes(file_bytes, file_len, cd_sig, 4);
        expect(cd_pos >= 0, "case 10a: find CD header");
        if (cd_pos >= 0) {
            memset(&file_bytes[cd_pos + 24], 0xFF, 4); /* uncompressed size = 0xFFFFFFFF */
            expect(write_file_bytes(path, file_bytes, file_len), "case 10a: write file");
        }
        free(file_bytes);
    }
    st = zip_read_entry(path, "huge.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_ZIP64, "case 10a: uncompressed size 0xFFFFFFFF returns ZIP_ERR_ZIP64");
    expect(strcmp(zip_status_reason(st), "zip64_unsupported") == 0, "case 10a: reason is zip64_unsupported");

    snprintf(path, sizeof(path), "%s/case10b.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('normal.txt', b'content'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 10b: create normal zip");
    file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 10b: read normal zip");
    if (file_bytes && file_len >= 22) {
        /* Insert 20 bytes of ZIP64 locator immediately before the 22-byte EOCD */
        size_t eocd_off = file_len - 22;
        unsigned char * mod_bytes = malloc(file_len + 20);
        expect(mod_bytes != NULL, "case 10b: alloc mod_bytes");
        if (mod_bytes) {
            memcpy(mod_bytes, file_bytes, eocd_off);
            unsigned char locator[20] = {0x50, 0x4b, 0x06, 0x07};
            memcpy(mod_bytes + eocd_off, locator, 20);
            memcpy(mod_bytes + eocd_off + 20, file_bytes + eocd_off, 22);
            expect(write_file_bytes(path, mod_bytes, file_len + 20), "case 10b: write file with locator");
            free(mod_bytes);
        }
        free(file_bytes);
    }
    st = zip_read_entry(path, "normal.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_ZIP64, "case 10b: locator signature returns ZIP_ERR_ZIP64");

    /* 11. Uncompressed size 1 << 20 with tiny compressed size -> entry_too_large before inflate. */
    snprintf(path, sizeof(path), "%s/case11.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('large.txt', b'tiny'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 11: create zip");
    file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 11: read file");
    if (file_bytes) {
        const unsigned char cd_sig[4] = {0x50, 0x4b, 0x01, 0x02};
        ssize_t cd_pos = find_bytes(file_bytes, file_len, cd_sig, 4);
        expect(cd_pos >= 0, "case 11: find CD header");
        if (cd_pos >= 0) {
            /* 1 << 20 = 0x00100000 */
            file_bytes[cd_pos + 24] = 0x00;
            file_bytes[cd_pos + 25] = 0x00;
            file_bytes[cd_pos + 26] = 0x10;
            file_bytes[cd_pos + 27] = 0x00;
            expect(write_file_bytes(path, file_bytes, file_len), "case 11: write file");
        }
        free(file_bytes);
    }
    st = zip_read_entry(path, "large.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_TOO_LARGE, "case 11: uncompressed > 256K returns ZIP_ERR_TOO_LARGE");
    expect(strcmp(zip_status_reason(st), "entry_too_large") == 0, "case 11: reason is entry_too_large");

    /* 12. Lying small uncompressed size: raw-deflate expands to ~1000, declared 32. */
    snprintf(path, sizeof(path), "%s/case12.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_DEFLATED); "
             "z.writestr('lying.txt', ('A' * 1000).encode('utf-8')); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 12: create zip");
    file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 12: read file");
    if (file_bytes) {
        const unsigned char loc_sig[4] = {0x50, 0x4b, 0x03, 0x04};
        const unsigned char cd_sig[4] = {0x50, 0x4b, 0x01, 0x02};
        ssize_t loc_pos = find_bytes(file_bytes, file_len, loc_sig, 4);
        ssize_t cd_pos = find_bytes(file_bytes, file_len, cd_sig, 4);
        expect(loc_pos >= 0 && cd_pos >= 0, "case 12: find headers");
        if (loc_pos >= 0 && cd_pos >= 0) {
            /* Declare uncompressed = 32 in both local and CD */
            file_bytes[loc_pos + 22] = 32;
            file_bytes[loc_pos + 23] = 0;
            file_bytes[loc_pos + 24] = 0;
            file_bytes[loc_pos + 25] = 0;

            file_bytes[cd_pos + 24] = 32;
            file_bytes[cd_pos + 25] = 0;
            file_bytes[cd_pos + 26] = 0;
            file_bytes[cd_pos + 27] = 0;
            expect(write_file_bytes(path, file_bytes, file_len), "case 12: write modified file");
        }
        free(file_bytes);
    }
    out = (unsigned char *) 0x1234;
    st = zip_read_entry(path, "lying.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_CORRUPT || st == ZIP_ERR_TOO_LARGE, "case 12: lying uncompressed size rejected");
    expect(out == NULL, "case 12: out remains NULL");

    /* 13. Local-header offset past EOF -> corrupt. */
    snprintf(path, sizeof(path), "%s/case13.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('file.txt', b'content'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 13: create zip");
    file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 13: read file");
    if (file_bytes) {
        const unsigned char cd_sig[4] = {0x50, 0x4b, 0x01, 0x02};
        ssize_t cd_pos = find_bytes(file_bytes, file_len, cd_sig, 4);
        expect(cd_pos >= 0, "case 13: find CD header");
        if (cd_pos >= 0) {
            file_bytes[cd_pos + 42] = 0x00;
            file_bytes[cd_pos + 43] = 0x00;
            file_bytes[cd_pos + 44] = 0x01; /* 0x00010000 = 65536 > file_len */
            file_bytes[cd_pos + 45] = 0x00;
            expect(write_file_bytes(path, file_bytes, file_len), "case 13: write file");
        }
        free(file_bytes);
    }
    st = zip_read_entry(path, "file.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_CORRUPT, "case 13: local header offset past EOF returns ZIP_ERR_CORRUPT");

    /* 14. Central directory size 300000 on file large enough (~320KB), and on tiny file. */
    snprintf(path, sizeof(path), "%s/case14a.zip", test_dir);
    size_t big_sz = 320000;
    unsigned char * big_buf = malloc(big_sz);
    expect(big_buf != NULL, "case 14a: alloc big_buf");
    if (big_buf) {
        memset(big_buf, 0, big_sz);
        unsigned char * eocd = big_buf + big_sz - 22;
        eocd[0] = 0x50; eocd[1] = 0x4b; eocd[2] = 0x05; eocd[3] = 0x06;
        eocd[8] = 1; eocd[9] = 0; /* disk entries = 1 */
        eocd[10] = 1; eocd[11] = 0; /* total entries = 1 */
        /* cd_size = 300000 = 0x000493E0 */
        eocd[12] = 0xe0; eocd[13] = 0x93; eocd[14] = 0x04; eocd[15] = 0x00;
        /* cd_offset = 0 */
        eocd[16] = 0; eocd[17] = 0; eocd[18] = 0; eocd[19] = 0;
        expect(write_file_bytes(path, big_buf, big_sz), "case 14a: write big file");
        free(big_buf);
    }
    st = zip_read_entry(path, "test.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_CORRUPT, "case 14a: cd_size > ZIP_MAX_CD_BYTES returns ZIP_ERR_CORRUPT");

    snprintf(path, sizeof(path), "%s/case14b.zip", test_dir);
    unsigned char tiny_eocd[100];
    memset(tiny_eocd, 0, sizeof(tiny_eocd));
    unsigned char * eocd_b = tiny_eocd + 100 - 22;
    eocd_b[0] = 0x50; eocd_b[1] = 0x4b; eocd_b[2] = 0x05; eocd_b[3] = 0x06;
    eocd_b[8] = 1; eocd_b[9] = 0;
    eocd_b[10] = 1; eocd_b[11] = 0;
    eocd_b[12] = 0xe0; eocd_b[13] = 0x93; eocd_b[14] = 0x04; eocd_b[15] = 0x00; /* 300000 */
    expect(write_file_bytes(path, tiny_eocd, 100), "case 14b: write tiny file with 300000 cd_size");
    st = zip_read_entry(path, "test.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_CORRUPT, "case 14b: cd_size not fitting returns ZIP_ERR_CORRUPT");

    /* 15. 2000 tiny stored entries -> list succeeds. 2001 -> corrupt. */
    snprintf(path, sizeof(path), "%s/case15_2000.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "[z.writestr(f'{i}', b'x') for i in range(2000)]; z.close()\"",
             path);
    expect(system(cmd) == 0, "case 15: create 2000 entries zip");
    names = NULL;
    count = 0;
    st = zip_list_entries(path, &names, &count);
    expect(st == ZIP_OK, "case 15: 2000 entries returns ZIP_OK");
    expect(count == 2000, "case 15: count == 2000");
    zip_free_names(names, count);

    snprintf(path, sizeof(path), "%s/case15_2001.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "[z.writestr(f'{i}', b'x') for i in range(2001)]; z.close()\"",
             path);
    expect(system(cmd) == 0, "case 15: create 2001 entries zip");
    names = NULL;
    count = 0;
    st = zip_list_entries(path, &names, &count);
    expect(st == ZIP_ERR_CORRUPT, "case 15: 2001 entries returns ZIP_ERR_CORRUPT");

    /* 16. EOCD comment and stored payload containing EOCD signature bytes. */
    snprintf(path, sizeof(path), "%s/case16.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('fake.bin', b'ABC\\x50\\x4b\\x05\\x06' + b'\\x00'*18 + b'XYZ'); "
             "z.comment = b'archive comment'; z.close()\"",
             path);
    expect(system(cmd) == 0, "case 16: create zip with comment and fake EOCD signature");
    out = NULL;
    out_len = 0;
    st = zip_read_entry(path, "fake.bin", &out, &out_len, &info);
    expect(st == ZIP_OK, "case 16: read fake.bin returns ZIP_OK");
    expect(out_len == 3 + 4 + 18 + 3, "case 16: payload length matches");
    free(out);

    /* 17. Data-descriptor flag (bit 3): local header sizes/crc 0, CD has real values. */
    snprintf(path, sizeof(path), "%s/case17.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('desc.txt', b'data descriptor payload'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 17: create zip");
    file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 17: read file");
    if (file_bytes) {
        const unsigned char loc_sig[4] = {0x50, 0x4b, 0x03, 0x04};
        ssize_t loc_pos = find_bytes(file_bytes, file_len, loc_sig, 4);
        expect(loc_pos >= 0, "case 17: find local header");
        if (loc_pos >= 0) {
            file_bytes[loc_pos + 6] |= 0x08; /* set bit 3 (data descriptor) */
            memset(&file_bytes[loc_pos + 14], 0, 12); /* zero local crc, comp, uncomp */
            expect(write_file_bytes(path, file_bytes, file_len), "case 17: write file");
        }
        free(file_bytes);
    }
    out = NULL;
    out_len = 0;
    st = zip_read_entry(path, "desc.txt", &out, &out_len, &info);
    expect(st == ZIP_OK, "case 17: read data descriptor entry returns ZIP_OK");
    expect(out && memcmp(out, "data descriptor payload", 23) == 0, "case 17: payload matches");
    free(out);

    /* 18. zip_status_reason strings and zip_crc32 vectors. */
    expect(strcmp(zip_status_reason(ZIP_OK), "ok") == 0, "case 18: reason ok");
    expect(strcmp(zip_status_reason(ZIP_ERR_IO), "io_error") == 0, "case 18: reason io_error");
    expect(strcmp(zip_status_reason(ZIP_ERR_CORRUPT), "corrupt") == 0, "case 18: reason corrupt");
    expect(strcmp(zip_status_reason(ZIP_ERR_NOT_FOUND), "not_found") == 0, "case 18: reason not_found");
    expect(strcmp(zip_status_reason(ZIP_ERR_ENCRYPTED), "encrypted") == 0, "case 18: reason encrypted");
    expect(strcmp(zip_status_reason(ZIP_ERR_ZIP64), "zip64_unsupported") == 0, "case 18: reason zip64_unsupported");
    expect(strcmp(zip_status_reason(ZIP_ERR_UNSUPPORTED_METHOD), "unsupported_method") == 0, "case 18: reason unsupported_method");
    expect(strcmp(zip_status_reason(ZIP_ERR_CRC), "crc_mismatch") == 0, "case 18: reason crc_mismatch");
    expect(strcmp(zip_status_reason(ZIP_ERR_TOO_LARGE), "entry_too_large") == 0, "case 18: reason entry_too_large");
    expect(strcmp(zip_status_reason(ZIP_ERR_NOMEM), "nomem") == 0, "case 18: reason nomem");
    expect(strcmp(zip_status_reason((zip_status) 999), "corrupt") == 0, "case 18: reason corrupt fallback");
    expect(zip_crc32(0, "123456789", 9) == 0xCBF43926u, "case 18: CRC-32 of '123456789' is 0xCBF43926");
    expect(zip_crc32(0, "", 0) == 0, "case 18: CRC-32 of empty is 0");

    /* 19. Multi-disk (disk field == 1) -> corrupt. */
    snprintf(path, sizeof(path), "%s/case19.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w', zipfile.ZIP_STORED); "
             "z.writestr('file.txt', b'abc'); z.close()\"",
             path);
    expect(system(cmd) == 0, "case 19: create zip");
    file_bytes = read_file_bytes(path, &file_len);
    expect(file_bytes != NULL, "case 19: read file");
    if (file_bytes && file_len >= 22) {
        const unsigned char eocd_sig[4] = {0x50, 0x4b, 0x05, 0x06};
        ssize_t eocd_pos = find_bytes(file_bytes, file_len, eocd_sig, 4);
        expect(eocd_pos >= 0, "case 19: find EOCD");
        if (eocd_pos >= 0) {
            file_bytes[eocd_pos + 4] = 1; /* disk number = 1 */
            file_bytes[eocd_pos + 5] = 0;
            expect(write_file_bytes(path, file_bytes, file_len), "case 19: write file");
        }
        free(file_bytes);
    }
    st = zip_read_entry(path, "file.txt", &out, &out_len, &info);
    expect(st == ZIP_ERR_CORRUPT, "case 19: multi-disk returns ZIP_ERR_CORRUPT");

    /* 20. Happy-path list + read of an archive with both stored and deflated entries. */
    snprintf(path, sizeof(path), "%s/case20.zip", test_dir);
    snprintf(cmd, sizeof(cmd),
             "python3 -c \"import zipfile; z = zipfile.ZipFile('%s', 'w'); "
             "z.writestr('s.txt', b'stored data', compress_type=zipfile.ZIP_STORED); "
             "z.writestr('d.txt', ('deflated data ' * 40).encode('utf-8'), compress_type=zipfile.ZIP_DEFLATED); "
             "z.close()\"",
             path);
    expect(system(cmd) == 0, "case 20: create zip with stored and deflated entries");
    names = NULL;
    count = 0;
    st = zip_list_entries(path, &names, &count);
    expect(st == ZIP_OK && count == 2, "case 20: list entries returns ZIP_OK with count 2");
    if (names && count == 2) {
        expect(strcmp(names[0], "s.txt") == 0, "case 20: name 0 matches");
        expect(strcmp(names[1], "d.txt") == 0, "case 20: name 1 matches");
    }
    zip_free_names(names, count);

    out = NULL;
    out_len = 0;
    st = zip_read_entry(path, "s.txt", &out, &out_len, &info);
    expect(st == ZIP_OK && info.method == 0 && out_len == 11, "case 20: read stored entry ok");
    expect(out && memcmp(out, "stored data", 11) == 0, "case 20: stored payload matches");
    free(out);

    out = NULL;
    out_len = 0;
    st = zip_read_entry(path, "d.txt", &out, &out_len, &info);
    expect(st == ZIP_OK && info.method == 8 && out_len == 14 * 40, "case 20: read deflated entry ok");
    free(out);

    /* 21. Central directory cd_size includes 8 extra trailing bytes -> ZIP_ERR_CORRUPT on both read and list. */
    snprintf(path, sizeof(path), "%s/case21.zip", test_dir);
    {
        unsigned char zip_bytes[] = {
            /* Local file header (30 bytes) */
            0x50, 0x4b, 0x03, 0x04,
            20, 0,
            0, 0,
            0, 0,
            0, 0,
            0, 0,
            0xac, 0x2a, 0x93, 0xd8, /* crc32 of "hi" */
            2, 0, 0, 0,
            2, 0, 0, 0,
            1, 0,
            0, 0,
            /* Filename */
            'a',
            /* Payload */
            'h', 'i',

            /* Central directory header (46 bytes) */
            0x50, 0x4b, 0x01, 0x02,
            20, 0,
            20, 0,
            0, 0,
            0, 0,
            0, 0,
            0, 0,
            0xac, 0x2a, 0x93, 0xd8,
            2, 0, 0, 0,
            2, 0, 0, 0,
            1, 0,
            0, 0,
            0, 0,
            0, 0,
            0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            /* Filename */
            'a',
            /* 8 extra trailing bytes in central directory */
            0, 0, 0, 0, 0, 0, 0, 0,

            /* End of central directory record (22 bytes) */
            0x50, 0x4b, 0x05, 0x06,
            0, 0,
            0, 0,
            1, 0,
            1, 0,
            55, 0, 0, 0,            /* cd_size = 47 + 8 = 55 */
            33, 0, 0, 0,            /* cd_offset = 33 */
            0, 0
        };
        expect(write_file_bytes(path, zip_bytes, sizeof(zip_bytes)), "case 21: write zip file");
    }
    out = (unsigned char *) 0x1234;
    out_len = 999;
    st = zip_read_entry(path, "a", &out, &out_len, &info);
    expect(st == ZIP_ERR_CORRUPT, "case 21: read entry with trailing CD bytes returns ZIP_ERR_CORRUPT");
    expect(out == NULL, "case 21: out is NULL");

    names = (char **) 0x1234;
    count = 999;
    st = zip_list_entries(path, &names, &count);
    expect(st == ZIP_ERR_CORRUPT, "case 21: list entries with trailing CD bytes returns ZIP_ERR_CORRUPT");
    expect(names == NULL, "case 21: names is NULL");
    expect(count == 0, "case 21: count is 0");

    /* 22. Local flag bit 0 set, CD flags 0 -> zip_read_entry returns ZIP_ERR_ENCRYPTED and NULL buffer. */
    snprintf(path, sizeof(path), "%s/case22.zip", test_dir);
    {
        unsigned char zip_bytes[] = {
            /* Local file header (30 bytes) */
            0x50, 0x4b, 0x03, 0x04,
            20, 0,
            1, 0,                   /* flags: bit 0 set */
            0, 0,
            0, 0,
            0, 0,
            0xac, 0x2a, 0x93, 0xd8, /* crc32 of "hi" */
            2, 0, 0, 0,
            2, 0, 0, 0,
            1, 0,
            0, 0,
            /* Filename */
            'a',
            /* Payload */
            'h', 'i',

            /* Central directory header (46 bytes) */
            0x50, 0x4b, 0x01, 0x02,
            20, 0,
            20, 0,
            0, 0,                   /* flags: left 0 */
            0, 0,
            0, 0,
            0, 0,
            0xac, 0x2a, 0x93, 0xd8,
            2, 0, 0, 0,
            2, 0, 0, 0,
            1, 0,
            0, 0,
            0, 0,
            0, 0,
            0, 0,
            0, 0, 0, 0,
            0, 0, 0, 0,
            /* Filename */
            'a',

            /* End of central directory record (22 bytes) */
            0x50, 0x4b, 0x05, 0x06,
            0, 0,
            0, 0,
            1, 0,
            1, 0,
            47, 0, 0, 0,            /* cd_size = 47 */
            33, 0, 0, 0,            /* cd_offset = 33 */
            0, 0
        };
        expect(write_file_bytes(path, zip_bytes, sizeof(zip_bytes)), "case 22: write zip file");
    }
    out = (unsigned char *) 0x1234;
    out_len = 999;
    st = zip_read_entry(path, "a", &out, &out_len, &info);
    expect(st == ZIP_ERR_ENCRYPTED, "case 22: local flag bit 0 set returns ZIP_ERR_ENCRYPTED");
    expect(out == NULL, "case 22: out is NULL on encrypted entry");

    /* 23. EOCD tests: empty archive (0 entries, cd_size 0) vs 0 entries with cd_size 46 */
    snprintf(path, sizeof(path), "%s/case23a.zip", test_dir);
    {
        unsigned char eocd_only[22] = {
            0x50, 0x4b, 0x05, 0x06,
            0, 0,
            0, 0,
            0, 0,
            0, 0,
            0, 0, 0, 0,             /* cd_size = 0 */
            0, 0, 0, 0,             /* cd_offset = 0 */
            0, 0
        };
        expect(write_file_bytes(path, eocd_only, sizeof(eocd_only)), "case 23a: write 22-byte EOCD file");
    }
    names = (char **) 0x1234;
    count = 999;
    st = zip_list_entries(path, &names, &count);
    expect(st == ZIP_OK, "case 23a: empty zip list returns ZIP_OK");
    expect(count == 0, "case 23a: count is 0");
    expect(names == NULL, "case 23a: names is NULL");

    out = (unsigned char *) 0x1234;
    out_len = 999;
    st = zip_read_entry(path, "a", &out, &out_len, &info);
    expect(st == ZIP_ERR_NOT_FOUND, "case 23a: read of 'a' returns ZIP_ERR_NOT_FOUND");
    expect(out == NULL, "case 23a: out is NULL");

    snprintf(path, sizeof(path), "%s/case23b.zip", test_dir);
    {
        unsigned char second_file[46 + 22];
        memset(second_file, 0, sizeof(second_file));
        unsigned char eocd[22] = {
            0x50, 0x4b, 0x05, 0x06,
            0, 0,
            0, 0,
            0, 0,
            0, 0,
            46, 0, 0, 0,            /* cd_size = 46 */
            0, 0, 0, 0,             /* cd_offset = 0 */
            0, 0
        };
        memcpy(second_file + 46, eocd, 22);
        expect(write_file_bytes(path, second_file, sizeof(second_file)), "case 23b: write file with 0 entries and cd_size 46");
    }
    names = (char **) 0x1234;
    count = 999;
    st = zip_list_entries(path, &names, &count);
    expect(st == ZIP_ERR_CORRUPT, "case 23b: list returns ZIP_ERR_CORRUPT");
    expect(names == NULL, "case 23b: names is NULL");
    expect(count == 0, "case 23b: count is 0");

    /* 24. Named pipe with no writer: open must not block, and both calls reject it. */
    snprintf(path, sizeof(path), "%s/case24.fifo", test_dir);
    expect(mkfifo(path, 0600) == 0, "case 24: mkfifo");
    alarm(2);
    names = (char **) 0x1234;
    count = 999;
    st = zip_list_entries(path, &names, &count);
    alarm(0);
    expect(st == ZIP_ERR_IO, "case 24: list of fifo returns ZIP_ERR_IO");
    expect(names == NULL, "case 24: names is NULL");
    expect(count == 0, "case 24: count is 0");
    alarm(2);
    out = (unsigned char *) 0x1234;
    out_len = 999;
    st = zip_read_entry(path, "a", &out, &out_len, &info);
    alarm(0);
    expect(st == ZIP_ERR_IO, "case 24: read of fifo returns ZIP_ERR_IO");
    expect(out == NULL, "case 24: out is NULL");
    expect(out_len == 0, "case 24: out_len is 0");

    if (failures) {
        fprintf(stderr, "zip reader: %d failure%s\n", failures, failures == 1 ? "" : "s");
        return 1;
    }
    puts("zip reader: PASS");
    return 0;
}
