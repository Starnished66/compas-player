# Translations

UI text is translated with gettext catalogs. English text in the C source is the
key: `TR("Text")` returns the translation for the active language, or the
English text when there is none. The catalogs live here; `src/ui/i18n_catalog.c`
is generated from them and checked in, so a normal build needs no Python or
gettext.

## Updating or adding strings

1. Wrap user-visible text in the source (see "Rules" below).
2. `make i18n` extracts `po/compas.pot`, merges it into every `po/*.po` and
   regenerates `src/ui/i18n_catalog.c`.
3. Translate the new (empty or fuzzy) entries in `po/<lang>.po`.
   Fuzzy entries are skipped until you remove the `fuzzy` flag.
4. `make i18n` again, then `make i18n-check` (stale template, bad printf
   conversions and unwrapped UI strings).

## Rules

- `TR("text")` at the place the text is shown. Format strings keep their
  placeholders: `TR("%d of %d")`. Translate whole sentences, never fragments.
- `TR_N("%d song", "%d songs", n)` when a count changes the wording.
- `TR_C("context", "text")` only when the same English word needs different
  translations; it maps to gettext `msgctxt`.
- `N_("text")` marks text in a static table; call `TR()` where it is displayed.
- Do not translate: log text, file names and paths, settings keys and values,
  protocol and API strings, text compared as data, plugin text, track metadata.
- Do not compare a label's text against a literal; compare state.
- Do not apply `toupper()` or other case changes to translated text.
- Buffers that receive translated text need room for longer wording.
- Intentional English that the checker flags goes in
  `scripts/i18n/untranslated_allow.txt`.

## Adding a language

1. Create `po/<code>.po` (for example
   `msginit --no-translator -i po/compas.pot -l pt -o po/pt.po`). Set the header fields `Language`,
   `X-Native-Name` (the language's own name, shown in the picker) and
   `Plural-Forms`.
2. Add the plural rule for the code in `i18n_plural_index()` in
   `src/ui/i18n.c` (Russian needs three forms; pt_BR and fr treat 0 as singular).
3. `make i18n`, translate, `make i18n-check`.
4. Check the fonts cover the language's characters (the Montserrat tiers cover
   Latin-1 and Latin Extended-A; other scripts need the fallback fonts).

The language list in Settings > System > Language comes from the catalogs, so
no UI code changes.
