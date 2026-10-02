/*
 * UI translation layer. English text is the lookup key: a string with no
 * translation, or the "en" language, returns the argument itself. Catalogs are
 * generated from the po/ catalogs into src/ui/i18n_catalog.c (see po/README.md); every
 * table lives in .rodata and lookups are binary searches, so there is no heap
 * use and no per-call-site state.
 */
#ifndef I18N_H
#define I18N_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define I18N_DEFAULT_LANGUAGE "en"
#define I18N_CODE_MAX 8

/* One catalog entry. `forms` holds nforms translated strings (1 for a plain
 * entry, the plural forms in gettext order otherwise). */
typedef struct {
    const char * key;
    const char * const * forms;
    unsigned char nforms;
} i18n_entry_t;

/* One language table, generated. `entries` is sorted by key in byte order. */
typedef struct {
    const char * code;
    const char * name;      /* the language's own name */
    const i18n_entry_t * entries;
    size_t count;
} i18n_lang_t;

/* Generated in i18n_catalog.c (non-English languages only). */
extern const i18n_lang_t i18n_catalog_langs[];
extern const size_t i18n_catalog_lang_count;

const char * i18n_tr(const char * english);
const char * i18n_tr_n(const char * singular, const char * plural, long n);

/* Unknown code: the language falls back to "en" and false is returned. */
bool i18n_set_language(const char * code);
const char * i18n_get_language(void);

/* Picker enumeration. Index 0 is always English. */
size_t i18n_language_count(void);
const char * i18n_language_code(size_t index);
const char * i18n_language_name(size_t index);

/* Plural form index for a language code and count (exposed for tests). */
unsigned i18n_plural_index(const char * code, long n);

#define TR(s) i18n_tr(s)
#define TR_N(singular, plural, n) i18n_tr_n((singular), (plural), (n))
/* Marks text in a static table; the display site calls TR() on it. */
#define N_(s) (s)
/* Context for ambiguous English words, gettext msgctxt: key is "ctx\004text". */
#define TR_C(ctx, s) i18n_tr(ctx "\004" s)

#ifdef __cplusplus
}
#endif

#endif
