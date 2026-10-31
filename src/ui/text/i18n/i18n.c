#include "i18n.h"

#include <string.h>

static const i18n_lang_t * s_lang = NULL; /* NULL means English */
static char s_code[I18N_CODE_MAX] = I18N_DEFAULT_LANGUAGE;

static const i18n_lang_t * find_lang(const char * code)
{
    if (!code) return NULL;
    for (size_t i = 0; i < i18n_catalog_lang_count; i++) {
        if (strcmp(i18n_catalog_langs[i].code, code) == 0) return &i18n_catalog_langs[i];
    }
    return NULL;
}

bool i18n_set_language(const char * code)
{
    if (code && strcmp(code, I18N_DEFAULT_LANGUAGE) == 0) {
        s_lang = NULL;
        strcpy(s_code, I18N_DEFAULT_LANGUAGE);
        return true;
    }
    const i18n_lang_t * lang = find_lang(code);
    if (!lang) {
        s_lang = NULL;
        strcpy(s_code, I18N_DEFAULT_LANGUAGE);
        return false;
    }
    s_lang = lang;
    strncpy(s_code, lang->code, sizeof(s_code) - 1);
    s_code[sizeof(s_code) - 1] = '\0';
    return true;
}

const char * i18n_get_language(void)
{
    return s_code;
}

size_t i18n_language_count(void)
{
    return 1 + i18n_catalog_lang_count;
}

const char * i18n_language_code(size_t index)
{
    if (index == 0) return I18N_DEFAULT_LANGUAGE;
    if (index > i18n_catalog_lang_count) return NULL;
    return i18n_catalog_langs[index - 1].code;
}

const char * i18n_language_name(size_t index)
{
    if (index == 0) return "English";
    if (index > i18n_catalog_lang_count) return NULL;
    return i18n_catalog_langs[index - 1].name;
}

static const i18n_entry_t * lookup(const i18n_lang_t * lang, const char * key)
{
    size_t lo = 0;
    size_t hi = lang->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = strcmp(key, lang->entries[mid].key);
        if (c == 0) return &lang->entries[mid];
        if (c < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* A TR_C key is "context\004text"; without a translation show only the text. */
static const char * strip_context(const char * key)
{
    const char * sep = strchr(key, '\004');
    return sep ? sep + 1 : key;
}

const char * i18n_tr(const char * english)
{
    if (!english) return english;
    if (s_lang) {
        const i18n_entry_t * e = lookup(s_lang, english);
        if (e && e->nforms > 0 && e->forms[0] && e->forms[0][0]) return e->forms[0];
    }
    return strip_context(english);
}

unsigned i18n_plural_index(const char * code, long n)
{
    /* Must match each catalog's Plural-Forms header. Russian and Ukrainian
     * distinguish one, few, and many; the teens always use the many form.
     * pt_BR and fr treat 0 and 1 as singular. zh_CN, ja, and vi have a single form.
     * The other two-form catalogs,
     * including European Portuguese, use singular only for exactly one. */
    if (code && (strcmp(code, "ru") == 0 || strcmp(code, "uk") == 0)) {
        long ones = n % 10;
        long tens = n % 100;
        if (ones == 1 && tens != 11) return 0u;
        if (ones >= 2 && ones <= 4 && (tens < 12 || tens > 14)) return 1u;
        return 2u;
    }
    if (code && (strcmp(code, "pt_BR") == 0 || strcmp(code, "fr") == 0))
        return n > 1 ? 1u : 0u;
    /* Chinese, Japanese, and Vietnamese use one form (nplurals=1). */
    if (code && (strcmp(code, "zh_CN") == 0 || strcmp(code, "ja") == 0 ||
                 strcmp(code, "vi") == 0)) return 0u;
    return n == 1 ? 0u : 1u;
}

const char * i18n_tr_n(const char * singular, const char * plural, long n)
{
    if (s_lang) {
        const i18n_entry_t * e = lookup(s_lang, singular);
        if (e && e->nforms > 0) {
            unsigned idx = i18n_plural_index(s_lang->code, n);
            if (idx >= e->nforms) idx = e->nforms - 1;
            if (e->forms[idx] && e->forms[idx][0]) return e->forms[idx];
        }
    }
    return n == 1 ? singular : plural;
}
