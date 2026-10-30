#include "aic_radio_policy.h"

#include <ctype.h>
#include <string.h>

/* Copies the first whitespace-delimited word of text into word. Returns the
 * length, or 0 for an empty or oversize word. */
static size_t first_word(const char * text, char * word, size_t cap) {
    if (!text) return 0;
    while (*text && isspace((unsigned char) *text)) text++;
    size_t n = 0;
    while (text[n] && !isspace((unsigned char) text[n])) n++;
    if (n == 0 || n >= cap) return 0;
    memcpy(word, text, n);
    word[n] = '\0';
    return n;
}

bool aic_radio_type_is_aic(const char * text) {
    char word[16];
    return first_word(text, word, sizeof(word)) && strcmp(word, "aic") == 0;
}

aic_base_state_t aic_radio_parse_base(const char * text) {
    if (!text) return AIC_BASE_PENDING;
    char word[16];
    size_t n = first_word(text, word, sizeof(word));
    if (n == 0) {
        /* Blank file: the writer has not finished. A word too long to be "ok" is a failure. */
        while (*text && isspace((unsigned char) *text)) text++;
        return *text ? AIC_BASE_FAILED : AIC_BASE_PENDING;
    }
    return strcmp(word, "ok") == 0 ? AIC_BASE_READY : AIC_BASE_FAILED;
}

aic_radio_decision_t aic_radio_decide(const aic_radio_state_t * state, aic_radio_request_t request) {
    if (request == AIC_RADIO_WIFI_OFF || request == AIC_RADIO_BT_OFF) return AIC_RADIO_RUN;
    if (state->base != AIC_BASE_READY) return AIC_RADIO_REFUSE_UNAVAILABLE;
    if (request == AIC_RADIO_WIFI_ON && state->bt_active) return AIC_RADIO_REFUSE_BT_ACTIVE;
    if (request == AIC_RADIO_BT_ON && state->wifi_active) return AIC_RADIO_REFUSE_WIFI_ACTIVE;
    return AIC_RADIO_RUN;
}

bool aic_radio_suspend_safe(bool hci0_present, bool fdrv_loaded) {
    return !hci0_present && !fdrv_loaded;
}
