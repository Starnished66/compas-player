/* Host tests for firmware_ota.c's pure helpers: weekly release selection
 * from a GitHub releases list, SHA256SUMS parsing, and installed-version
 * dates. `make firmware-ota-selftest` */
#include "firmware_ota.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Order and flags follow real API quirks: newest first but not always,
 * recent weeklies are not marked prerelease, and the base image is a
 * published (non-draft) release that must never be chosen. */
static const char releases[] =
    "["
    "{\"tag_name\":\"staging-image-base\",\"draft\":false,\"prerelease\":false,\"assets\":["
    "  {\"name\":\"r1.upt\",\"size\":38696960,\"browser_download_url\":\"https://github.com/o/r/releases/download/staging-image-base/r1.upt\"}]},"
    "{\"tag_name\":\"weekly-beta-2026-09-29\",\"draft\":true,\"assets\":["
    "  {\"name\":\"r1.upt\",\"size\":38696960,\"browser_download_url\":\"https://github.com/o/r/releases/download/weekly-beta-2026-09-29/r1.upt\"},"
    "  {\"name\":\"SHA256SUMS\",\"size\":411,\"browser_download_url\":\"https://github.com/o/r/releases/download/weekly-beta-2026-09-29/SHA256SUMS\"}]},"
    "{\"tag_name\":\"weekly-beta-2026-09-16\",\"draft\":false,\"prerelease\":false,\"assets\":["
    "  {\"name\":\"r1.upt\",\"size\":38156288,\"browser_download_url\":\"https://github.com/o/r/releases/download/weekly-beta-2026-09-16/r1.upt\"},"
    "  {\"name\":\"SHA256SUMS\",\"size\":266,\"browser_download_url\":\"https://github.com/o/r/releases/download/weekly-beta-2026-09-16/SHA256SUMS\"}]},"
    "{\"tag_name\":\"weekly-beta-2026-09-22\",\"draft\":false,\"prerelease\":false,\"assets\":["
    "  {\"name\":\"r1.upt\",\"size\":38696960,\"browser_download_url\":\"https://github.com/o/r/releases/download/weekly-beta-2026-09-22/r1.upt\"},"
    "  {\"name\":\"r3proii.upt\",\"size\":37980160,\"digest\":\"sha256:5ACFD880b99fd8980c9dcf6fd8c6e6badb95d05fc54160ee833814117801d405\",\"browser_download_url\":\"https://github.com/o/r/releases/download/weekly-beta-2026-09-22/r3proii.upt\"},"
    "  {\"name\":\"SHA256SUMS\",\"size\":411,\"browser_download_url\":\"https://github.com/o/r/releases/download/weekly-beta-2026-09-22/SHA256SUMS\"}]},"
    "{\"tag_name\":\"weekly-beta-2026-09-23x\",\"draft\":false,\"assets\":[]},"
    "{\"tag_name\":\"weekly-beta-2026-09-25\",\"draft\":false,\"assets\":["
    "  {\"name\":\"r1.upt\",\"size\":500,\"browser_download_url\":\"https://github.com/o/r/releases/download/weekly-beta-2026-09-25/r1.upt\"},"
    "  {\"name\":\"SHA256SUMS\",\"size\":411,\"browser_download_url\":\"https://github.com/o/r/releases/download/weekly-beta-2026-09-25/SHA256SUMS\"}]},"
    "{\"tag_name\":\"weekly-beta-2026-09-26\",\"draft\":false,\"assets\":["
    "  {\"name\":\"r1.upt\",\"size\":38696960,\"browser_download_url\":\"http://insecure.example/r1.upt\"},"
    "  {\"name\":\"SHA256SUMS\",\"size\":411,\"browser_download_url\":\"https://github.com/o/r/releases/download/weekly-beta-2026-09-26/SHA256SUMS\"}]}"
    "]";

int main(void) {
    firmware_ota_release_t release;
    char error[128];

    /* Newest usable weekly by tag date: 09-29 is a draft, 09-26 has an
     * insecure URL, 09-25 is too small to be an image, 09-23x is malformed. */
    assert(firmware_ota_parse_releases(releases, sizeof(releases) - 1, "r1.upt", &release, error, sizeof(error)));
    assert(strcmp(release.tag, "weekly-beta-2026-09-22") == 0 && strcmp(release.date, "2026-09-22") == 0);
    assert(release.asset_size == 38696960 && strcmp(release.asset_name, "r1.upt") == 0);
    assert(strstr(release.asset_url, "/weekly-beta-2026-09-22/r1.upt"));
    assert(strstr(release.sums_url, "/weekly-beta-2026-09-22/SHA256SUMS"));

    /* A board present only in some weeks gets its newest week. */
    assert(firmware_ota_parse_releases(releases, sizeof(releases) - 1, "r3proii.upt", &release, error, sizeof(error)));
    assert(strcmp(release.date, "2026-09-22") == 0);
    assert(strcmp(release.asset_digest, "5acfd880b99fd8980c9dcf6fd8c6e6badb95d05fc54160ee833814117801d405") == 0);
    assert(firmware_ota_parse_releases(releases, sizeof(releases) - 1, "r1.upt", &release, error, sizeof(error)) &&
           release.asset_digest[0] == '\0'); /* no digest published */
    assert(!firmware_ota_parse_releases(releases, sizeof(releases) - 1, "r3ii_2025.upt", &release, error, sizeof(error)));
    assert(strstr(error, "r3ii_2025.upt"));
    assert(!firmware_ota_parse_releases("{\"message\":\"API rate limit\"}", 29, "r1.upt", &release, error, sizeof(error)));
    assert(!firmware_ota_parse_releases("not json", 8, "r1.upt", &release, error, sizeof(error)));

    /* SHA256SUMS: exact name match, both separators, CRLF, case folded. */
    const char sums[] =
        "1111111111111111111111111111111111111111111111111111111111111111  r1.upt.parked\n"
        "2222222222222222222222222222222222222222222222222222222222222222  r3proii.upt\r\n"
        "ABCDEFabcdef0123456789abcdef0123456789abcdef0123456789abcdef0123 *r1.upt\n"
        "zz22222222222222222222222222222222222222222222222222222222222222  r3ii_2025.upt\n";
    char hex[65];
    assert(firmware_ota_parse_sums(sums, sizeof(sums) - 1, "r1.upt", hex));
    assert(strcmp(hex, "abcdefabcdef0123456789abcdef0123456789abcdef0123456789abcdef0123") == 0);
    assert(firmware_ota_parse_sums(sums, sizeof(sums) - 1, "r3proii.upt", hex) && hex[0] == '2');
    assert(!firmware_ota_parse_sums(sums, sizeof(sums) - 1, "r3ii_2025.upt", hex)); /* not hex */
    assert(!firmware_ota_parse_sums(sums, sizeof(sums) - 1, "r1", hex));
    const char old_sums[] =
        "deaa2f9ebc8d6b8c076947057e3da1558ceefd4f6cf14677fd9f54de877b0e3d  release/r1.upt\n"
        "3333333333333333333333333333333333333333333333333333333333333333  release/xr1.upt\n";
    assert(firmware_ota_parse_sums(old_sums, sizeof(old_sums) - 1, "r1.upt", hex) && hex[0] == 'd');
    assert(!firmware_ota_parse_sums("3333333333333333333333333333333333333333333333333333333333333333  xr1.upt\n",
                                    74, "r1.upt", hex));

    /* Installed version dates. */
    char date[11];
    assert(firmware_ota_label_date("Weekly Beta 2026-09-22", date) && strcmp(date, "2026-09-22") == 0);
    assert(firmware_ota_label_date("Weekly Beta 2026-09-22 (R3II 2025 Early Alpha)", date) &&
           strcmp(date, "2026-09-22") == 0);
    assert(!firmware_ota_label_date("Development Build", date) && date[0] == '\0');
    assert(!firmware_ota_label_date("Weekly Beta 2026-9-22", date));
    assert(!firmware_ota_label_date("Test Build x", date));

    puts("firmware OTA helper tests passed");
    return 0;
}
