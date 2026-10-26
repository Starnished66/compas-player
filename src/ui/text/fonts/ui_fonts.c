#define _POSIX_C_SOURCE 200809L

#include "ui_fonts.h"

#include "lvgl/lvgl.h"
#include "ui_font_manifest.h"

#include "mbedtls/sha256.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UI_FONT_COUNT UI_FONT_MANIFEST_COUNT
#define UI_FONT_ROOT_ENV "COMPAS_FONT_ROOT"

/* These stable names are referenced throughout the application and by the
 * LVGL default-font setting. The descriptor implementation is supplied by
 * lv_binfont_create at startup, while the allocated owners below keep each
 * binfont's private descriptor and glyph tables alive for the process. */
lv_font_t lv_font_montserrat_12;
lv_font_t lv_font_montserrat_14;
lv_font_t lv_font_montserrat_16;
lv_font_t lv_font_montserrat_18;
lv_font_t lv_font_montserrat_20;
lv_font_t lv_font_montserrat_22;
lv_font_t lv_font_montserrat_24;
lv_font_t lv_font_montserrat_26;
lv_font_t lv_font_montserrat_28;
lv_font_t lv_font_montserrat_30;
lv_font_t lv_font_montserrat_32;
lv_font_t lv_font_montserrat_34;
lv_font_t lv_font_montserrat_40;
lv_font_t lv_font_unscii_16;

static lv_font_t * owned_fonts[UI_FONT_COUNT];
static bool initialized;

static lv_font_t * const font_slots[UI_FONT_COUNT] = {
    &lv_font_montserrat_12,
    &lv_font_montserrat_14,
    &lv_font_montserrat_16,
    &lv_font_montserrat_18,
    &lv_font_montserrat_20,
    &lv_font_montserrat_22,
    &lv_font_montserrat_24,
    &lv_font_montserrat_26,
    &lv_font_montserrat_28,
    &lv_font_montserrat_30,
    &lv_font_montserrat_32,
    &lv_font_montserrat_34,
    &lv_font_montserrat_40,
    &lv_font_unscii_16,
};

static const char * font_root(void)
{
    const char * override = getenv(UI_FONT_ROOT_ENV);
    if(override != NULL && override[0] != '\0') return override;

#ifdef HOST_BUILD
    return "assets/fonts";
#else
    return "/usr/resource/fonts";
#endif
}

static uint64_t monotonic_us(void)
{
    struct timespec ts;
    if(clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static bool build_disk_path(const char * root, const char * filename,
                            char disk_path[LV_FS_MAX_PATH_LENGTH])
{
    size_t root_len = strlen(root);
    const char * slash = root[root_len - 1] == '/' ? "" : "/";
    int disk_len = snprintf(disk_path, LV_FS_MAX_PATH_LENGTH, "%s%s%s",
                            root, slash, filename);
    return disk_len >= 0 && disk_len < LV_FS_MAX_PATH_LENGTH;
}

/* Read and verify the complete file before passing any asset to LVGL's
 * parser. Keeping this buffer lets startup validate the exact bytes later
 * parsed without reopening or rereading the file. */
static uint8_t * read_validated_font_file(const char * path,
                                          const ui_font_manifest_entry_t * expected)
{
    if(expected->size == 0 || expected->size > UINT32_MAX) {
        fprintf(stderr, "UI fonts: invalid manifest length for %s\n", path);
        return NULL;
    }

    FILE * file = fopen(path, "rb");
    if(file == NULL) {
        fprintf(stderr, "UI fonts: cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    uint8_t * data = malloc(expected->size);
    if(data == NULL) {
        fprintf(stderr, "UI fonts: out of memory reading %s (%zu bytes)\n",
                path, expected->size);
        fclose(file);
        return NULL;
    }

    bool ok = true;
    size_t total = fread(data, 1, expected->size, file);
    if(total != expected->size) {
        fprintf(stderr, "UI fonts: %s is truncated (expected %zu bytes, got %zu)\n",
                path, expected->size, total);
        ok = false;
    }
    if(ok) {
        int extra = fgetc(file);
        if(extra != EOF) {
            fprintf(stderr, "UI fonts: %s has extra data (expected %zu bytes)\n",
                    path, expected->size);
            ok = false;
        }
    }
    if(ferror(file)) {
        fprintf(stderr, "UI fonts: read error for %s: %s\n", path, strerror(errno));
        ok = false;
    }
    if(fclose(file) != 0) {
        fprintf(stderr, "UI fonts: close error for %s: %s\n", path, strerror(errno));
        ok = false;
    }

    uint8_t digest[32];
    if(ok && mbedtls_sha256(data, total, digest, 0) != 0) {
        fprintf(stderr, "UI fonts: SHA-256 failed for %s\n", path);
        ok = false;
    }
    if(ok && memcmp(digest, expected->sha256, sizeof(digest)) != 0) {
        fprintf(stderr, "UI fonts: SHA-256 mismatch for %s\n", path);
        ok = false;
    }
    if(!ok) {
        free(data);
        return NULL;
    }
    return data;
}

static void rollback(lv_font_t * candidates[UI_FONT_COUNT], size_t loaded)
{
    while(loaded > 0) lv_binfont_destroy(candidates[--loaded]);
}

bool ui_fonts_init(void)
{
    if(initialized) {
        for(size_t i = 0; i < UI_FONT_COUNT; i++) {
            if(owned_fonts[i] == NULL) {
                fprintf(stderr, "UI fonts: ownership state is incomplete\n");
                return false;
            }
        }
        return true;
    }

    const char * root = font_root();
    lv_font_t * candidates[UI_FONT_COUNT] = {0};
    uint8_t * buffers[UI_FONT_COUNT] = {0};
    char disk_path[LV_FS_MAX_PATH_LENGTH];
    const uint64_t start_us = monotonic_us();

    /* Validate every member before creating a single LVGL font object. The
     * buffers are bounded by the manifest sizes and retain the exact bytes
     * for the later parse pass. */
    for(size_t i = 0; i < UI_FONT_COUNT; i++) {
        const ui_font_manifest_entry_t * entry = &ui_font_manifest[i];
        if(!build_disk_path(root, entry->filename, disk_path)) {
            fprintf(stderr, "UI fonts: path too long for %s/%s\n", root, entry->filename);
            for(size_t j = 0; j < i; j++) free(buffers[j]);
            return false;
        }
        buffers[i] = read_validated_font_file(disk_path, entry);
        if(buffers[i] == NULL) {
            for(size_t j = 0; j < i; j++) free(buffers[j]);
            return false;
        }
    }

    for(size_t i = 0; i < UI_FONT_COUNT; i++) {
        const ui_font_manifest_entry_t * entry = &ui_font_manifest[i];
        candidates[i] = lv_binfont_create_from_buffer(buffers[i], (uint32_t)entry->size);
        free(buffers[i]);
        buffers[i] = NULL;
        if(candidates[i] == NULL) {
            for(size_t j = i + 1; j < UI_FONT_COUNT; j++) free(buffers[j]);
            rollback(candidates, i);
            fprintf(stderr, "UI fonts: LVGL failed to parse validated font %s\n", entry->filename);
            return false;
        }
    }

    /* lv_font_t is a public descriptor value in LVGL 9.5. Copying the
     * descriptor preserves its callbacks and private dsc pointer, while the
     * original allocation remains owned so LVGL can safely use that private
     * data for the whole process lifetime. */
    for(size_t i = 0; i < UI_FONT_COUNT; i++) {
        *font_slots[i] = *candidates[i];
        owned_fonts[i] = candidates[i];
    }

    initialized = true;
    uint64_t elapsed_us = monotonic_us() - start_us;
    fprintf(stderr, "UI fonts: loaded %d external bitmap fonts from %s in %" PRIu64 " ms\n",
            UI_FONT_COUNT, root, (uint64_t)(elapsed_us / 1000));
    return true;
}
