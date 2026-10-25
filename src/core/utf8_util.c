#include "utf8_util.h"
#include <string.h>

typedef struct {
    const unsigned char *p;
    unsigned char pending[8];
    unsigned count, at;
    bool started;
    bool legacy_punctuation;
} collate_iter_t;

static bool decode_one(const unsigned char **p, uint32_t *cp) {
    const unsigned char *s = *p;
    unsigned n;
    if (s[0] < 0x80) { *cp = *s; *p = s + 1; return true; }
    if (s[0] >= 0xC2 && s[0] <= 0xDF) n = 2;
    else if (s[0] >= 0xE0 && s[0] <= 0xEF) n = 3;
    else if (s[0] >= 0xF0 && s[0] <= 0xF4) n = 4;
    else { *cp = 0x110000u + s[0]; *p = s + 1; return false; }
    for (unsigned i = 1; i < n; i++) if (!s[i] || (s[i] & 0xC0) != 0x80) {
        *cp = 0x110000u + s[0]; *p = s + 1; return false;
    }
    if ((n == 3 && ((s[0] == 0xE0 && s[1] < 0xA0) || (s[0] == 0xED && s[1] >= 0xA0))) ||
        (n == 4 && ((s[0] == 0xF0 && s[1] < 0x90) || (s[0] == 0xF4 && s[1] >= 0x90)))) {
        *cp = 0x110000u + s[0]; *p = s + 1; return false;
    }
    uint32_t v = s[0] & (n == 2 ? 0x1F : n == 3 ? 0x0F : 0x07);
    for (unsigned i = 1; i < n; i++) v = (v << 6) | (s[i] & 0x3F);
    *cp = v; *p = s + n; return true;
}

static bool ignorable_leading(uint32_t c) {
    if (c < 128) return c <= 0x20 || (c >= 0x21 && c <= 0x2F) ||
        (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) ||
        (c >= 0x7B && c <= 0x7F);
    if ((c >= 0x2000 && c <= 0x206F) || (c >= 0x3000 && c <= 0x303F) ||
        (c >= 0xFE10 && c <= 0xFE1F) || (c >= 0xFE30 && c <= 0xFE6F)) return true;
    return c == 0x00A0 || c == 0x00A1 || c == 0x00AB || c == 0x00BB || c == 0x00BF ||
           c == 0x2018 || c == 0x2019 ||
           c == 0x201C || c == 0x201D || c == 0x2039 || c == 0x203A;
}

/* Generated from Unicode NFKD mappings for U+00C0..U+024F and U+1E00..U+1EFF
 * where decomposition is one ASCII Latin letter plus combining marks. The
 * explicit entries cover eth, stroked D/O/L whose decompositions are absent. */
typedef struct { uint16_t codepoint; unsigned char base; } latin_fold_t;
static const latin_fold_t latin_folds[] = {
    {0x00C0u, 'a'}, {0x00C1u, 'a'}, {0x00C2u, 'a'}, {0x00C3u, 'a'},
    {0x00C4u, 'a'}, {0x00C5u, 'a'}, {0x00C7u, 'c'}, {0x00C8u, 'e'},
    {0x00C9u, 'e'}, {0x00CAu, 'e'}, {0x00CBu, 'e'}, {0x00CCu, 'i'},
    {0x00CDu, 'i'}, {0x00CEu, 'i'}, {0x00CFu, 'i'}, {0x00D0u, 'd'},
    {0x00D1u, 'n'}, {0x00D2u, 'o'}, {0x00D3u, 'o'}, {0x00D4u, 'o'},
    {0x00D5u, 'o'}, {0x00D6u, 'o'}, {0x00D8u, 'o'}, {0x00D9u, 'u'},
    {0x00DAu, 'u'}, {0x00DBu, 'u'}, {0x00DCu, 'u'}, {0x00DDu, 'y'},
    {0x00E0u, 'a'}, {0x00E1u, 'a'}, {0x00E2u, 'a'}, {0x00E3u, 'a'},
    {0x00E4u, 'a'}, {0x00E5u, 'a'}, {0x00E7u, 'c'}, {0x00E8u, 'e'},
    {0x00E9u, 'e'}, {0x00EAu, 'e'}, {0x00EBu, 'e'}, {0x00ECu, 'i'},
    {0x00EDu, 'i'}, {0x00EEu, 'i'}, {0x00EFu, 'i'}, {0x00F0u, 'd'},
    {0x00F1u, 'n'}, {0x00F2u, 'o'}, {0x00F3u, 'o'}, {0x00F4u, 'o'},
    {0x00F5u, 'o'}, {0x00F6u, 'o'}, {0x00F8u, 'o'}, {0x00F9u, 'u'},
    {0x00FAu, 'u'}, {0x00FBu, 'u'}, {0x00FCu, 'u'}, {0x00FDu, 'y'},
    {0x00FFu, 'y'}, {0x0100u, 'a'}, {0x0101u, 'a'}, {0x0102u, 'a'},
    {0x0103u, 'a'}, {0x0104u, 'a'}, {0x0105u, 'a'}, {0x0106u, 'c'},
    {0x0107u, 'c'}, {0x0108u, 'c'}, {0x0109u, 'c'}, {0x010Au, 'c'},
    {0x010Bu, 'c'}, {0x010Cu, 'c'}, {0x010Du, 'c'}, {0x010Eu, 'd'},
    {0x010Fu, 'd'}, {0x0110u, 'd'}, {0x0111u, 'd'}, {0x0112u, 'e'},
    {0x0113u, 'e'}, {0x0114u, 'e'}, {0x0115u, 'e'}, {0x0116u, 'e'},
    {0x0117u, 'e'}, {0x0118u, 'e'}, {0x0119u, 'e'}, {0x011Au, 'e'},
    {0x011Bu, 'e'}, {0x011Cu, 'g'}, {0x011Du, 'g'}, {0x011Eu, 'g'},
    {0x011Fu, 'g'}, {0x0120u, 'g'}, {0x0121u, 'g'}, {0x0122u, 'g'},
    {0x0123u, 'g'}, {0x0124u, 'h'}, {0x0125u, 'h'}, {0x0128u, 'i'},
    {0x0129u, 'i'}, {0x012Au, 'i'}, {0x012Bu, 'i'}, {0x012Cu, 'i'},
    {0x012Du, 'i'}, {0x012Eu, 'i'}, {0x012Fu, 'i'}, {0x0130u, 'i'},
    {0x0134u, 'j'}, {0x0135u, 'j'}, {0x0136u, 'k'}, {0x0137u, 'k'},
    {0x0139u, 'l'}, {0x013Au, 'l'}, {0x013Bu, 'l'}, {0x013Cu, 'l'},
    {0x013Du, 'l'}, {0x013Eu, 'l'}, {0x0141u, 'l'}, {0x0142u, 'l'},
    {0x0143u, 'n'}, {0x0144u, 'n'}, {0x0145u, 'n'}, {0x0146u, 'n'},
    {0x0147u, 'n'}, {0x0148u, 'n'}, {0x014Cu, 'o'}, {0x014Du, 'o'},
    {0x014Eu, 'o'}, {0x014Fu, 'o'}, {0x0150u, 'o'}, {0x0151u, 'o'},
    {0x0154u, 'r'}, {0x0155u, 'r'}, {0x0156u, 'r'}, {0x0157u, 'r'},
    {0x0158u, 'r'}, {0x0159u, 'r'}, {0x015Au, 's'}, {0x015Bu, 's'},
    {0x015Cu, 's'}, {0x015Du, 's'}, {0x015Eu, 's'}, {0x015Fu, 's'},
    {0x0160u, 's'}, {0x0161u, 's'}, {0x0162u, 't'}, {0x0163u, 't'},
    {0x0164u, 't'}, {0x0165u, 't'}, {0x0168u, 'u'}, {0x0169u, 'u'},
    {0x016Au, 'u'}, {0x016Bu, 'u'}, {0x016Cu, 'u'}, {0x016Du, 'u'},
    {0x016Eu, 'u'}, {0x016Fu, 'u'}, {0x0170u, 'u'}, {0x0171u, 'u'},
    {0x0172u, 'u'}, {0x0173u, 'u'}, {0x0174u, 'w'}, {0x0175u, 'w'},
    {0x0176u, 'y'}, {0x0177u, 'y'}, {0x0178u, 'y'}, {0x0179u, 'z'},
    {0x017Au, 'z'}, {0x017Bu, 'z'}, {0x017Cu, 'z'}, {0x017Du, 'z'},
    {0x017Eu, 'z'}, {0x017Fu, 's'}, {0x01A0u, 'o'}, {0x01A1u, 'o'},
    {0x01AFu, 'u'}, {0x01B0u, 'u'}, {0x01CDu, 'a'}, {0x01CEu, 'a'},
    {0x01CFu, 'i'}, {0x01D0u, 'i'}, {0x01D1u, 'o'}, {0x01D2u, 'o'},
    {0x01D3u, 'u'}, {0x01D4u, 'u'}, {0x01D5u, 'u'}, {0x01D6u, 'u'},
    {0x01D7u, 'u'}, {0x01D8u, 'u'}, {0x01D9u, 'u'}, {0x01DAu, 'u'},
    {0x01DBu, 'u'}, {0x01DCu, 'u'}, {0x01DEu, 'a'}, {0x01DFu, 'a'},
    {0x01E0u, 'a'}, {0x01E1u, 'a'}, {0x01E6u, 'g'}, {0x01E7u, 'g'},
    {0x01E8u, 'k'}, {0x01E9u, 'k'}, {0x01EAu, 'o'}, {0x01EBu, 'o'},
    {0x01ECu, 'o'}, {0x01EDu, 'o'}, {0x01F0u, 'j'}, {0x01F4u, 'g'},
    {0x01F5u, 'g'}, {0x01F8u, 'n'}, {0x01F9u, 'n'}, {0x01FAu, 'a'},
    {0x01FBu, 'a'}, {0x0200u, 'a'}, {0x0201u, 'a'}, {0x0202u, 'a'},
    {0x0203u, 'a'}, {0x0204u, 'e'}, {0x0205u, 'e'}, {0x0206u, 'e'},
    {0x0207u, 'e'}, {0x0208u, 'i'}, {0x0209u, 'i'}, {0x020Au, 'i'},
    {0x020Bu, 'i'}, {0x020Cu, 'o'}, {0x020Du, 'o'}, {0x020Eu, 'o'},
    {0x020Fu, 'o'}, {0x0210u, 'r'}, {0x0211u, 'r'}, {0x0212u, 'r'},
    {0x0213u, 'r'}, {0x0214u, 'u'}, {0x0215u, 'u'}, {0x0216u, 'u'},
    {0x0217u, 'u'}, {0x0218u, 's'}, {0x0219u, 's'}, {0x021Au, 't'},
    {0x021Bu, 't'}, {0x021Eu, 'h'}, {0x021Fu, 'h'}, {0x0226u, 'a'},
    {0x0227u, 'a'}, {0x0228u, 'e'}, {0x0229u, 'e'}, {0x022Au, 'o'},
    {0x022Bu, 'o'}, {0x022Cu, 'o'}, {0x022Du, 'o'}, {0x022Eu, 'o'},
    {0x022Fu, 'o'}, {0x0230u, 'o'}, {0x0231u, 'o'}, {0x0232u, 'y'},
    {0x0233u, 'y'}, {0x1E00u, 'a'}, {0x1E01u, 'a'}, {0x1E02u, 'b'},
    {0x1E03u, 'b'}, {0x1E04u, 'b'}, {0x1E05u, 'b'}, {0x1E06u, 'b'},
    {0x1E07u, 'b'}, {0x1E08u, 'c'}, {0x1E09u, 'c'}, {0x1E0Au, 'd'},
    {0x1E0Bu, 'd'}, {0x1E0Cu, 'd'}, {0x1E0Du, 'd'}, {0x1E0Eu, 'd'},
    {0x1E0Fu, 'd'}, {0x1E10u, 'd'}, {0x1E11u, 'd'}, {0x1E12u, 'd'},
    {0x1E13u, 'd'}, {0x1E14u, 'e'}, {0x1E15u, 'e'}, {0x1E16u, 'e'},
    {0x1E17u, 'e'}, {0x1E18u, 'e'}, {0x1E19u, 'e'}, {0x1E1Au, 'e'},
    {0x1E1Bu, 'e'}, {0x1E1Cu, 'e'}, {0x1E1Du, 'e'}, {0x1E1Eu, 'f'},
    {0x1E1Fu, 'f'}, {0x1E20u, 'g'}, {0x1E21u, 'g'}, {0x1E22u, 'h'},
    {0x1E23u, 'h'}, {0x1E24u, 'h'}, {0x1E25u, 'h'}, {0x1E26u, 'h'},
    {0x1E27u, 'h'}, {0x1E28u, 'h'}, {0x1E29u, 'h'}, {0x1E2Au, 'h'},
    {0x1E2Bu, 'h'}, {0x1E2Cu, 'i'}, {0x1E2Du, 'i'}, {0x1E2Eu, 'i'},
    {0x1E2Fu, 'i'}, {0x1E30u, 'k'}, {0x1E31u, 'k'}, {0x1E32u, 'k'},
    {0x1E33u, 'k'}, {0x1E34u, 'k'}, {0x1E35u, 'k'}, {0x1E36u, 'l'},
    {0x1E37u, 'l'}, {0x1E38u, 'l'}, {0x1E39u, 'l'}, {0x1E3Au, 'l'},
    {0x1E3Bu, 'l'}, {0x1E3Cu, 'l'}, {0x1E3Du, 'l'}, {0x1E3Eu, 'm'},
    {0x1E3Fu, 'm'}, {0x1E40u, 'm'}, {0x1E41u, 'm'}, {0x1E42u, 'm'},
    {0x1E43u, 'm'}, {0x1E44u, 'n'}, {0x1E45u, 'n'}, {0x1E46u, 'n'},
    {0x1E47u, 'n'}, {0x1E48u, 'n'}, {0x1E49u, 'n'}, {0x1E4Au, 'n'},
    {0x1E4Bu, 'n'}, {0x1E4Cu, 'o'}, {0x1E4Du, 'o'}, {0x1E4Eu, 'o'},
    {0x1E4Fu, 'o'}, {0x1E50u, 'o'}, {0x1E51u, 'o'}, {0x1E52u, 'o'},
    {0x1E53u, 'o'}, {0x1E54u, 'p'}, {0x1E55u, 'p'}, {0x1E56u, 'p'},
    {0x1E57u, 'p'}, {0x1E58u, 'r'}, {0x1E59u, 'r'}, {0x1E5Au, 'r'},
    {0x1E5Bu, 'r'}, {0x1E5Cu, 'r'}, {0x1E5Du, 'r'}, {0x1E5Eu, 'r'},
    {0x1E5Fu, 'r'}, {0x1E60u, 's'}, {0x1E61u, 's'}, {0x1E62u, 's'},
    {0x1E63u, 's'}, {0x1E64u, 's'}, {0x1E65u, 's'}, {0x1E66u, 's'},
    {0x1E67u, 's'}, {0x1E68u, 's'}, {0x1E69u, 's'}, {0x1E6Au, 't'},
    {0x1E6Bu, 't'}, {0x1E6Cu, 't'}, {0x1E6Du, 't'}, {0x1E6Eu, 't'},
    {0x1E6Fu, 't'}, {0x1E70u, 't'}, {0x1E71u, 't'}, {0x1E72u, 'u'},
    {0x1E73u, 'u'}, {0x1E74u, 'u'}, {0x1E75u, 'u'}, {0x1E76u, 'u'},
    {0x1E77u, 'u'}, {0x1E78u, 'u'}, {0x1E79u, 'u'}, {0x1E7Au, 'u'},
    {0x1E7Bu, 'u'}, {0x1E7Cu, 'v'}, {0x1E7Du, 'v'}, {0x1E7Eu, 'v'},
    {0x1E7Fu, 'v'}, {0x1E80u, 'w'}, {0x1E81u, 'w'}, {0x1E82u, 'w'},
    {0x1E83u, 'w'}, {0x1E84u, 'w'}, {0x1E85u, 'w'}, {0x1E86u, 'w'},
    {0x1E87u, 'w'}, {0x1E88u, 'w'}, {0x1E89u, 'w'}, {0x1E8Au, 'x'},
    {0x1E8Bu, 'x'}, {0x1E8Cu, 'x'}, {0x1E8Du, 'x'}, {0x1E8Eu, 'y'},
    {0x1E8Fu, 'y'}, {0x1E90u, 'z'}, {0x1E91u, 'z'}, {0x1E92u, 'z'},
    {0x1E93u, 'z'}, {0x1E94u, 'z'}, {0x1E95u, 'z'}, {0x1E96u, 'h'},
    {0x1E97u, 't'}, {0x1E98u, 'w'}, {0x1E99u, 'y'}, {0x1E9Bu, 's'},
    {0x1EA0u, 'a'}, {0x1EA1u, 'a'}, {0x1EA2u, 'a'}, {0x1EA3u, 'a'},
    {0x1EA4u, 'a'}, {0x1EA5u, 'a'}, {0x1EA6u, 'a'}, {0x1EA7u, 'a'},
    {0x1EA8u, 'a'}, {0x1EA9u, 'a'}, {0x1EAAu, 'a'}, {0x1EABu, 'a'},
    {0x1EACu, 'a'}, {0x1EADu, 'a'}, {0x1EAEu, 'a'}, {0x1EAFu, 'a'},
    {0x1EB0u, 'a'}, {0x1EB1u, 'a'}, {0x1EB2u, 'a'}, {0x1EB3u, 'a'},
    {0x1EB4u, 'a'}, {0x1EB5u, 'a'}, {0x1EB6u, 'a'}, {0x1EB7u, 'a'},
    {0x1EB8u, 'e'}, {0x1EB9u, 'e'}, {0x1EBAu, 'e'}, {0x1EBBu, 'e'},
    {0x1EBCu, 'e'}, {0x1EBDu, 'e'}, {0x1EBEu, 'e'}, {0x1EBFu, 'e'},
    {0x1EC0u, 'e'}, {0x1EC1u, 'e'}, {0x1EC2u, 'e'}, {0x1EC3u, 'e'},
    {0x1EC4u, 'e'}, {0x1EC5u, 'e'}, {0x1EC6u, 'e'}, {0x1EC7u, 'e'},
    {0x1EC8u, 'i'}, {0x1EC9u, 'i'}, {0x1ECAu, 'i'}, {0x1ECBu, 'i'},
    {0x1ECCu, 'o'}, {0x1ECDu, 'o'}, {0x1ECEu, 'o'}, {0x1ECFu, 'o'},
    {0x1ED0u, 'o'}, {0x1ED1u, 'o'}, {0x1ED2u, 'o'}, {0x1ED3u, 'o'},
    {0x1ED4u, 'o'}, {0x1ED5u, 'o'}, {0x1ED6u, 'o'}, {0x1ED7u, 'o'},
    {0x1ED8u, 'o'}, {0x1ED9u, 'o'}, {0x1EDAu, 'o'}, {0x1EDBu, 'o'},
    {0x1EDCu, 'o'}, {0x1EDDu, 'o'}, {0x1EDEu, 'o'}, {0x1EDFu, 'o'},
    {0x1EE0u, 'o'}, {0x1EE1u, 'o'}, {0x1EE2u, 'o'}, {0x1EE3u, 'o'},
    {0x1EE4u, 'u'}, {0x1EE5u, 'u'}, {0x1EE6u, 'u'}, {0x1EE7u, 'u'},
    {0x1EE8u, 'u'}, {0x1EE9u, 'u'}, {0x1EEAu, 'u'}, {0x1EEBu, 'u'},
    {0x1EECu, 'u'}, {0x1EEDu, 'u'}, {0x1EEEu, 'u'}, {0x1EEFu, 'u'},
    {0x1EF0u, 'u'}, {0x1EF1u, 'u'}, {0x1EF2u, 'y'}, {0x1EF3u, 'y'},
    {0x1EF4u, 'y'}, {0x1EF5u, 'y'}, {0x1EF6u, 'y'}, {0x1EF7u, 'y'},
    {0x1EF8u, 'y'}, {0x1EF9u, 'y'},
};

static unsigned latin_base(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 'a' && c <= 'z') return c;
    size_t lo = 0, hi = sizeof(latin_folds) / sizeof(latin_folds[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (latin_folds[mid].codepoint < c) lo = mid + 1;
        else hi = mid;
    }
    return lo < sizeof(latin_folds) / sizeof(latin_folds[0]) && latin_folds[lo].codepoint == c
        ? latin_folds[lo].base : 0;
}

static void lower_nonlatin(uint32_t *c) {
    /* Greek and Cyrillic basic uppercase blocks have regular lowercase maps. */
    if (*c == 0x0386) *c = 0x03AC;
    else if (*c == 0x0388) *c = 0x03AD;
    else if (*c == 0x0389) *c = 0x03AE;
    else if (*c == 0x038A) *c = 0x03AF;
    else if (*c == 0x038C) *c = 0x03CC;
    else if (*c == 0x038E) *c = 0x03CD;
    else if (*c == 0x038F) *c = 0x03CE;
    else if (*c == 0x03AA) *c = 0x03CA;
    else if (*c == 0x03AB) *c = 0x03CB;
    else if (*c >= 0x0391 && *c <= 0x03A1) *c += 0x20;
    else if (*c >= 0x03A3 && *c <= 0x03AB) *c += 0x20;
    else if (*c >= 0x0410 && *c <= 0x042F) *c += 0x20;
    else if (*c >= 0x0400 && *c <= 0x040F) *c += 0x50;
}

static void append_utf8(collate_iter_t *it, uint32_t c) {
    if (c < 0x80) it->pending[it->count++] = (unsigned char)c;
    else if (c < 0x800) {
        it->pending[it->count++] = (unsigned char)(0xC0 | (c >> 6));
        it->pending[it->count++] = (unsigned char)(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        it->pending[it->count++] = (unsigned char)(0xE0 | (c >> 12));
        it->pending[it->count++] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        it->pending[it->count++] = (unsigned char)(0x80 | (c & 0x3F));
    } else {
        it->pending[it->count++] = (unsigned char)(0xF0 | (c >> 18));
        it->pending[it->count++] = (unsigned char)(0x80 | ((c >> 12) & 0x3F));
        it->pending[it->count++] = (unsigned char)(0x80 | ((c >> 6) & 0x3F));
        it->pending[it->count++] = (unsigned char)(0x80 | (c & 0x3F));
    }
}

static bool next_primary_byte(collate_iter_t *it, unsigned char *key) {
    if (it->at < it->count) {
        *key = it->pending[it->at++];
        return true;
    }
    it->count = it->at = 0;
    for (;;) {
        uint32_t c;
        if (!*it->p) return false;
        bool valid = decode_one(&it->p, &c);
        if (c >= 0x0300 && c <= 0x036F) continue;
        if (!it->started && (!it->legacy_punctuation || (c != 0x00A1 && c != 0x00BF && c != 0x7F)) &&
            ignorable_leading(c)) continue;
        it->started = true;
        if (!valid) {
            /* Invalid bytes sort after valid Unicode, with stable byte order. */
            unsigned bad = (unsigned)(c - 0x110000u);
            it->pending[it->count++] = 0xFF;
            it->pending[it->count++] = (unsigned char)(2 + (bad >> 7));
            it->pending[it->count++] = (unsigned char)(2 + (bad & 0x7F));
            *key = it->pending[it->at++];
            return true;
        }
        if (c < 0x80) {
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            *key = (unsigned char)c + 2;
            return true;
        }
        if (c == 0x00C6 || c == 0x00E6) {
            it->pending[it->count++] = 'a' + 2;
            it->pending[it->count++] = 'e' + 2;
        } else if (c == 0x0152 || c == 0x0153) {
            it->pending[it->count++] = 'o' + 2;
            it->pending[it->count++] = 'e' + 2;
        } else if (c == 0x00DF || c == 0x1E9E) {
            it->pending[it->count++] = 's' + 2;
            it->pending[it->count++] = 's' + 2;
        } else if (c == 0x00DE || c == 0x00FE) {
            it->pending[it->count++] = 't' + 2;
            it->pending[it->count++] = 'h' + 2;
        } else if (c == 0x0132 || c == 0x0133) {
            it->pending[it->count++] = 'i' + 2;
            it->pending[it->count++] = 'j' + 2;
        } else {
            unsigned base = latin_base(c);
            if (c == 0x0126 || c == 0x0127) base = 'h';
            else if (c == 0x0166 || c == 0x0167) base = 't';
            else if (c == 0x013F || c == 0x0140) base = 'l';
            else if (c == 0x0131) base = 'i';
            if (base) {
                *key = (unsigned char)base + 2;
                return true;
            }
            lower_nonlatin(&c);
            append_utf8(it, c);
        }
        *key = it->pending[it->at++]; return true;
    }
}

static int collate_compare_internal(const char *a, const char *b, bool legacy_punctuation) {
    collate_iter_t x = {(const unsigned char *)(a ? a : ""),{0},0,0,false,legacy_punctuation};
    collate_iter_t y = {(const unsigned char *)(b ? b : ""),{0},0,0,false,legacy_punctuation};
    for (;;) {
        unsigned char ka = 0, kb = 0;
        bool has_a = next_primary_byte(&x, &ka);
        bool has_b = next_primary_byte(&y, &kb);
        if (!has_a || !has_b) return has_a ? 1 : has_b ? -1 : 0;
        if (ka != kb) return ka < kb ? -1 : 1;
    }
}

int utf8_collate_compare(const char *a, const char *b) {
    return collate_compare_internal(a, b, false);
}

static int collate_identity_compare(const char *a, const char *b, bool legacy_punctuation) {
    int c = collate_compare_internal(a, b, legacy_punctuation);
    if (c) return c;

    const unsigned char *x = (const unsigned char *)(a ? a : "");
    const unsigned char *y = (const unsigned char *)(b ? b : "");
    while (*x && *y) {
        unsigned cx = *x++;
        unsigned cy = *y++;
        if (cx >= 'A' && cx <= 'Z') cx += 'a' - 'A';
        if (cy >= 'A' && cy <= 'Z') cy += 'a' - 'A';
        if (cx != cy) return cx < cy ? -1 : 1;
    }
    return *x ? 1 : *y ? -1 : 0;
}

int utf8_collate_identity_compare(const char *a, const char *b) {
    return collate_identity_compare(a, b, false);
}

int utf8_collate_identity_compare_v6(const char *a, const char *b) {
    return collate_identity_compare(a, b, true);
}

static bool encode_key(const char *value, unsigned char *out, size_t width, bool identity) {
    if (!out || !width) return false;
    memset(out, 0, width);
    size_t used = 0;
    collate_iter_t it = {(const unsigned char *)(value ? value : ""), {0}, 0, 0, false, false};
    unsigned char key;
    while (next_primary_byte(&it, &key)) {
        if (used == width) return false;
        out[used++] = key;
    }
    if (identity) {
        if (used == width) return false;
        out[used++] = 1;
        const unsigned char *p = (const unsigned char *)(value ? value : "");
        while (*p) {
            unsigned c = *p++;
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (used == width) return false;
            out[used++] = (unsigned char)(2 + (c >> 4));
            if (used == width) return false;
            out[used++] = (unsigned char)(2 + (c & 0x0F));
        }
    }
    return used < width;
}

bool utf8_collate_prefix(const char *value, unsigned char *out, size_t width) {
    return encode_key(value, out, width, false);
}

bool utf8_collate_identity_prefix(const char *value, unsigned char *out, size_t width) {
    return encode_key(value, out, width, true);
}

bool utf8_is_valid(const char * text, size_t size) {
    if (!text) return size == 0;
    size_t i = 0;
    while (i < size) {
        unsigned char c = (unsigned char) text[i++];
        if (c < 0x80) continue;
        unsigned need;
        if (c >= 0xC2 && c <= 0xDF) need = 1;
        else if (c >= 0xE0 && c <= 0xEF) need = 2;
        else if (c >= 0xF0 && c <= 0xF4) need = 3;
        else return false;
        if (i + need > size) return false;
        unsigned char first = (unsigned char) text[i];
        if ((c == 0xE0 && first < 0xA0) || (c == 0xED && first >= 0xA0) ||
            (c == 0xF0 && first < 0x90) || (c == 0xF4 && first >= 0x90)) return false;
        for (unsigned j = 0; j < need; j++) {
            if (((unsigned char) text[i++] & 0xC0) != 0x80) return false;
        }
    }
    return true;
}

void utf8_sanitize(char * str) {
    if (!str) return;
    uint8_t * p = (uint8_t *)str;
    while (*p) {
        if (*p < 0x80) {
            /* 1-byte ASCII (0x00..0x7F) */
            p++;
            continue;
        }

        int len = 0;
        if (*p >= 0xC2 && *p <= 0xDF) {
            len = 2;
        } else if (*p >= 0xE0 && *p <= 0xEF) {
            len = 3;
        } else if (*p >= 0xF0 && *p <= 0xF4) {
            len = 4;
        }

        /* Second-byte boundary constraints: reject overlong encodings (E0, F0),
         * UTF-16 surrogates U+D800..U+DFFF (ED), and codepoints > U+10FFFF (F4). */
        uint8_t min_b1 = 0x80, max_b1 = 0xBF;
        if (*p == 0xE0) {
            min_b1 = 0xA0;
        } else if (*p == 0xED) {
            max_b1 = 0x9F;
        } else if (*p == 0xF0) {
            min_b1 = 0x90;
        } else if (*p == 0xF4) {
            max_b1 = 0x8F;
        }

        bool valid = len > 0 && p[1] >= min_b1 && p[1] <= max_b1;
        for (int i = 2; valid && i < len; i++) {
            valid = (p[i] & 0xC0) == 0x80;
        }

        if (valid) {
            p += len;
        } else {
            *p = '?';
            p++;
        }
    }
}

size_t utf8_truncate_safe_bounded(char * dst, size_t dst_size, const char * src, size_t src_len) {
    if (!dst || dst_size == 0) return 0;
    if (!src || src_len == 0) {
        dst[0] = '\0';
        return 0;
    }

    size_t max_copy = dst_size - 1;
    if (max_copy > src_len) {
        max_copy = src_len;
    }
    if (max_copy == 0) {
        dst[0] = '\0';
        return 0;
    }

    /* Count trailing continuation bytes at the end of the candidate slice src[0 .. max_copy - 1] */
    size_t k = 0;
    while (k < max_copy && k < 4 && ((uint8_t)src[max_copy - 1 - k] & 0xC0) == 0x80) {
        k++;
    }

    size_t cut = max_copy;
    if (k < max_copy) {
        size_t lead_idx = max_copy - 1 - k;
        uint8_t lead = (uint8_t)src[lead_idx];
        size_t seq_len = 1;
        if (lead < 0x80) {
            seq_len = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            seq_len = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            seq_len = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            seq_len = 4;
        }

        if (lead_idx + seq_len > max_copy) {
            /* The multibyte sequence crosses past max_copy; drop its incomplete prefix */
            cut = lead_idx;
        } else {
            cut = max_copy;
        }
    }

    memcpy(dst, src, cut);
    dst[cut] = '\0';
    utf8_sanitize(dst);
    return cut;
}

size_t utf8_truncate_safe(char * dst, const char * src, size_t max_bytes) {
    if (!src) return utf8_truncate_safe_bounded(dst, max_bytes, NULL, 0);
    return utf8_truncate_safe_bounded(dst, max_bytes, src, strnlen(src, max_bytes));
}
