#!/usr/bin/env python3
"""Compile every catalog and check sorting, counts and the production lookup."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[5]
CATALOGS = ROOT / 'src/ui/text/i18n'
code = ['#include "i18n.h"', '#include <assert.h>', '#include <stdio.h>',
        '#include <stdlib.h>', '#include <string.h>']
languages = []
for path in sorted(CATALOGS.glob('i18n_catalog_*.c')):
    languages.append(path.stem.removeprefix('i18n_catalog_'))
    code.append(f'#include "{path.name}"')
code.append('''
static void check(const char *code, const i18n_entry_t *entries, size_t count) {
    const i18n_lang_t *lang = NULL;
    for (size_t i = 0; i < i18n_catalog_lang_count; ++i)
        if (!strcmp(code, i18n_catalog_langs[i].code)) lang = &i18n_catalog_langs[i];
    assert(lang && lang->entries == entries && *lang->count == count);
    assert(i18n_set_language(code));
    for (size_t i = 0; i < count; ++i) {
        const i18n_entry_t *entry = &entries[i];
        if (i) assert(strcmp(entries[i - 1].key, entry->key) < 0);
        assert(entry->nforms && entry->forms[0] && entry->forms[0][0]);
        char *key = malloc(strlen(entry->key) + 1);
        assert(key);
        strcpy(key, entry->key);
        /* Pointer equality distinguishes a translation from English fallback. */
        assert(i18n_tr(key) == entry->forms[0]);
        for (long n = 0; n <= 200; ++n) {
            unsigned form = i18n_plural_index(code, n);
            if (form >= entry->nforms) form = entry->nforms - 1;
            assert(i18n_tr_n(key, "missing plural", n) == entry->forms[form]);
        }
        free(key);
    }
    printf("PASS %s: %zu sorted, reachable entries\\n", code, count);
}
int main(void) {
''')
code.append(f'assert(i18n_catalog_lang_count == {len(languages)});')
for lang in languages:
    code.append(f'check("{lang}", i18n_entries_{lang}, '
                f'sizeof(i18n_entries_{lang}) / sizeof(i18n_entries_{lang}[0]));')
code.append('return 0;\n}')
with tempfile.TemporaryDirectory(prefix='i18n-catalogs-') as tmp:
    source = Path(tmp) / 'test.c'
    binary = Path(tmp) / 'test'
    source.write_text('\n'.join(code))
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                    '-I', str(CATALOGS), str(source), str(CATALOGS / 'i18n.c'),
                    str(CATALOGS / 'i18n_catalog.c'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
