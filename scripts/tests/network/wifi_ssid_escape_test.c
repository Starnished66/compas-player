#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../../src/network/wifi/wifi_control.h"
#include "../../../src/core/process/subprocess.h"
#include <mbedtls/pkcs5.h>

static const char * status_ssid;
static const char * scan_rows;
static const char * saved_rows;
static char seen_ssid_hex[80];
static char seen_salt[64];
static size_t seen_salt_len;
static int subprocess_calls;

bool subprocess_run(char *const argv[], char *out, size_t size) {
    subprocess_calls++;
    const char *cmd = argv[3];
    if (!out || !size) return true;
    const char *src = "OK\n";
    if (!strcmp(cmd, "scan_results")) src = scan_rows;
    else if (!strcmp(cmd, "status")) {
        static char status[512];
        snprintf(status, sizeof(status), "wpa_state=COMPLETED\nssid=%s\nip_address=1.2.3.4\n", status_ssid);
        src = status;
    } else if (!strcmp(cmd, "add_network")) src = "7\n";
    else if (!strcmp(cmd, "set_network") && !strcmp(argv[5], "ssid")) {
        snprintf(seen_ssid_hex, sizeof(seen_ssid_hex), "%s", argv[6]);
    }
    snprintf(out, size, "%s", src);
    return true;
}
bool subprocess_run_checked(char *const argv[], char *out, size_t size, int timeout, int *code) {
    (void)timeout; if (code) *code = 0;
    if (!strcmp(argv[3], "list_networks")) { snprintf(out, size, "%s", saved_rows); return true; }
    return subprocess_run(argv, out, size);
}
bool subprocess_run_checked_group(char *const a[], char *o, size_t n, int t, int *c) {(void)a;(void)o;(void)n;(void)t;if(c)*c=0;return true;}
bool subprocess_run_timeout(char *const a[], char *o, size_t n, int t) {(void)a;(void)o;(void)n;(void)t;return true;}
bool subprocess_run_low_priority(char *const a[], char *o, size_t n) {(void)a;(void)o;(void)n;return true;}
bool subprocess_spawn_daemon(char *const a[]) {(void)a;return true;}
bool subprocess_spawn_daemon_logged(char *const a[], const char *p) {(void)a;(void)p;return true;}
bool subprocess_popen(char *const a[], pid_t *p, int *f) {(void)a;(void)p;(void)f;return false;}
bool subprocess_popen_stdin(char *const a[], pid_t *p, int *f) {(void)a;(void)p;(void)f;return false;}
void subprocess_terminate(pid_t p) {(void)p;}
void subprocess_kill_all_matching(const char *p) {(void)p;}
int mbedtls_pkcs5_pbkdf2_hmac_ext(mbedtls_md_type_t md, const unsigned char *password, size_t plen,
 const unsigned char *salt, size_t slen, unsigned int rounds, uint32_t keylen, unsigned char *out) {
    assert(md == MBEDTLS_MD_SHA1 && rounds == 4096 && keylen == 32);
    assert(plen == 8 && !memcmp(password, "password", 8));
    seen_salt_len=slen; memcpy(seen_salt,salt,slen); memset(out,0,keylen); return 0;
}

#include "../../../src/network/wifi/wifi_control.c"

int main(void) {
    /* Ten CJK codepoints occupy 30 bytes but their escaped wpa_cli form is
     * 120 characters, so parsing must decode before enforcing the byte cap. */
    const char *cjk = "\\xe4\\xb8\\xad\\xe4\\xb8\\xad\\xe4\\xb8\\xad\\xe4\\xb8\\xad\\xe4\\xb8\\xad\\xe4\\xb8\\xad\\xe4\\xb8\\xad\\xe4\\xb8\\xad\\xe4\\xb8\\xad\\xe4\\xb8\\xad";
    status_ssid = cjk;
    char scan[1024];
    snprintf(scan, sizeof(scan), "bssid / frequency / signal level / flags / ssid\n00:00\t2412\t-45\t[WPA2]\t%s\n00:01\t2412\t-60\t[WPA2]\t%s\n", cjk, cjk);
    scan_rows = scan;
    wifi_network_t nets[8];
    int n = wifi_control_get_results(nets, 8);
    assert(n == 1 && !strcmp(nets[0].ssid, "中中中中中中中中中中") && nets[0].is_current);

    /* Escaped literal backslash-x text is decoded once, then passed to
     * connect as the exact bytes; an empty flags column still parses. */
    status_ssid = "\\\\x41";
    scan_rows = "header\n00:00\t2412\t-82\t\t\\\\x41\n00:01\t2412\t-45\t\t\\\\x41\n";
    n = wifi_control_get_results(nets, 8);
    assert(n == 1 && !strcmp(nets[0].ssid, "\\x41") && nets[0].is_current);
    assert(nets[0].signal_level == 3 && !nets[0].secured);
    assert(wifi_control_connect(nets[0].ssid, NULL));
    assert(!strcmp(seen_ssid_hex, "5c783431"));

    saved_rows = "network id / ssid / bssid / flags\n0\t\\\"quoted\\\"\\\\slash\tany\t[CURRENT]\n1\t\\x5c\\x78literal\tany\t\n2\t\\x00bad\tany\t\n3\toversize-123456789012345678901234567890\tany\t\n4\t\tany\t\n5\topen-empty-bssid\t\t[CURRENT]\n";
    wifi_saved_network_t saved[8];
    n = wifi_control_list_saved(saved, 8);
    assert(n == 3);
    assert(!strcmp(saved[0].ssid, "\"quoted\"\\slash") && saved[0].current);
    assert(!strcmp(saved[1].ssid, "\\xliteral"));
    assert(!strcmp(saved[2].ssid, "open-empty-bssid") && saved[2].current);

    status_ssid = "\\x5c\\x78literal";
    wifi_info_t info;
    /* get_info proceeds after decoding status; subsequent OS queries may fail on host. */
    assert(wifi_control_get_info(&info));
    assert(!strcmp(info.ssid, "\\xliteral"));

    assert(wifi_control_connect("中中中中中中中中中中", "password"));
    assert(!strcmp(seen_ssid_hex, "e4b8ade4b8ade4b8ade4b8ade4b8ade4b8ade4b8ade4b8ade4b8ade4b8ad"));
    assert(seen_salt_len == 30 && !memcmp(seen_salt, "中中中中中中中中中中", 30));
    assert(!wifi_control_connect("123456789012345678901234567890123", "x"));
    assert(decode_wpa_ssid("bad\\xG0", info.ssid, sizeof(info.ssid)) < 0);
    assert(decode_wpa_ssid("bad\\x00", info.ssid, sizeof(info.ssid)) < 0);
    assert(decode_wpa_ssid("bad\\", info.ssid, sizeof(info.ssid)) < 0);
    assert(decode_wpa_ssid("bad\\x1", info.ssid, sizeof(info.ssid)) < 0);
    assert(decode_wpa_ssid("bad\\q", info.ssid, sizeof(info.ssid)) < 0);
    const char raw_non_utf8[] = { (char)0xff, '\0' };
    assert(decode_wpa_ssid(raw_non_utf8, info.ssid, sizeof(info.ssid)) == 1);
    assert((unsigned char)info.ssid[0] == 0xff);
    assert(decode_wpa_ssid("quote\\\" slash\\\\ tab\\t line\\n", info.ssid, sizeof(info.ssid)) > 0);
    assert(!strcmp(info.ssid, "quote\" slash\\ tab\t line\n"));
    assert(decode_wpa_ssid("123456789012345678901234567890123", info.ssid, sizeof(info.ssid)) < 0);
    assert(decode_wpa_ssid("12345678901234567890123456789012", info.ssid, sizeof(info.ssid)) == 32);
    int calls_before_invalid_connect = subprocess_calls;
    assert(!wifi_control_connect(NULL, "x") && !wifi_control_connect("", "x"));
    assert(!wifi_control_connect("123456789012345678901234567890123", "x"));
    assert(subprocess_calls == calls_before_invalid_connect);
    assert(wifi_control_connect("12345678901234567890123456789012", NULL));
    assert(!strcmp(seen_ssid_hex, "3132333435363738393031323334353637383930313233343536373839303132"));
    puts("wifi SSID escape tests passed");
    return 0;
}
