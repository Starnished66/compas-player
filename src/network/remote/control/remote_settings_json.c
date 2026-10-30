#include "remote_settings.h"
#include "../../../core/persistence/settings_registry.h"
#include "../../../core/system/runtime/app_version.h"
#include "../../../ui/text/i18n/i18n.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char * data;
    size_t length, capacity;
    bool failed;
} sbuf_t;

static void sbuf_appendf(sbuf_t * sb, const char * fmt, ...) {
    if (sb->failed) return;
    for (int attempt = 0; attempt < 2; attempt++) {
        va_list args;
        va_start(args, fmt);
        int n = vsnprintf(sb->data ? sb->data + sb->length : NULL, sb->capacity - sb->length, fmt, args);
        va_end(args);
        if (n < 0) { sb->failed = true; return; }
        if ((size_t) n < sb->capacity - sb->length) { sb->length += (size_t) n; return; }
        size_t capacity = sb->capacity ? sb->capacity * 2 : 4096;
        while (capacity - sb->length <= (size_t) n) capacity *= 2;
        char * grown = realloc(sb->data, capacity);
        if (!grown) { sb->failed = true; return; }
        sb->data = grown;
        sb->capacity = capacity;
    }
    sb->failed = true;
}

void remote_settings_json_escape(char * out, size_t size, const char * text) {
    size_t used = 0;
    for (const unsigned char * p = (const unsigned char *) (text ? text : ""); *p && used + 7 < size; p++) {
        if (*p == '"' || *p == '\\') { out[used++] = '\\'; out[used++] = (char) *p; }
        else if (*p < 0x20) used += (size_t) snprintf(out + used, size - used, "\\u%04x", *p);
        else out[used++] = (char) *p;
    }
    if (size) out[used] = '\0';
}

static void append_string(sbuf_t * sb, const char * text) {
    char escaped[512];
    remote_settings_json_escape(escaped, sizeof(escaped), text);
    sbuf_appendf(sb, "\"%s\"", escaped);
}

static const char * type_name(const setting_desc_t * d) {
    switch (d->type) {
        case SET_BOOL: return "bool";
        case SET_ENUM: return "enum";
        case SET_INT: return d->unit && d->unit[0] == '%' ? "percent" : "int";
        case SET_PRESET: return "preset";
        case SET_STRING: return "string";
    }
    return "int";
}

const char * remote_settings_apply_name(set_apply_t apply) {
    switch (apply) {
        case SET_APPLY_NEXT_TRACK: return "next_track";
        case SET_APPLY_NEXT_USE: return "next_use";
        case SET_APPLY_NEXT_BOOT: return "next_boot";
        case SET_APPLY_LIVE: break;
    }
    return "live";
}

static void append_label(sbuf_t * sb, const char * msgid) {
    sbuf_appendf(sb, "\"label\":");
    append_string(sb, TR(msgid));
    sbuf_appendf(sb, ",\"labelId\":");
    append_string(sb, msgid);
}

static void append_schema_entry(sbuf_t * sb, const setting_desc_t * d) {
    const char * why = d->reason;
    bool available = settings_registry_available(d, &why);
    sbuf_appendf(sb, "{\"key\":\"%s\",\"group\":\"%s\",", d->key, d->group);
    append_label(sb, d->label);
    sbuf_appendf(sb, ",\"type\":\"%s\",\"apply\":\"%s\",\"available\":%s", type_name(d), remote_settings_apply_name(d->apply),
                 available ? "true" : "false");
    if (!available && why) sbuf_appendf(sb, ",\"reason\":\"%s\"", why);
    if (d->flags & SET_F_READONLY) {
        sbuf_appendf(sb, ",\"readOnly\":true");
        if (d->reason) sbuf_appendf(sb, ",\"reason\":\"%s\"", d->reason);
    }
    if (d->type == SET_INT) sbuf_appendf(sb, ",\"min\":%d,\"max\":%d", d->min, d->max);
    if (d->unit) sbuf_appendf(sb, ",\"unit\":\"%s\"", d->unit);
    if (d->type == SET_PRESET) {
        sbuf_appendf(sb, ",\"values\":[");
        for (int i = 0; i < d->preset_count; i++) sbuf_appendf(sb, "%s%d", i ? "," : "", d->presets[i]);
        sbuf_appendf(sb, "]");
    }
    if (d->type == SET_ENUM) {
        sbuf_appendf(sb, ",\"options\":[");
        for (int i = 0; i < d->option_count; i++) {
            sbuf_appendf(sb, "%s{\"value\":%d,", i ? "," : "", d->options[i].value);
            append_label(sb, d->options[i].label);
            sbuf_appendf(sb, "}");
        }
        sbuf_appendf(sb, "]");
    }
    if (d->depends) sbuf_appendf(sb, ",\"dependsOn\":\"%s\"", d->depends);
    if (d->flags & SET_F_CONFIRM) sbuf_appendf(sb, ",\"confirmAbove\":%d", settings_registry_loud_threshold());
    sbuf_appendf(sb, "}");
}

char * remote_settings_build_schema(void) {
    sbuf_t sb = {0};
    const char * model = "HiBy R1", * board = "r1";
#if defined(BOARD_R3PROII)
    model = "HiBy R3 Pro II"; board = "r3proii";
#elif defined(BOARD_R3II_2025)
    model = "HiBy R3 II 2025"; board = "r3ii_2025";
#endif
    sbuf_appendf(&sb, "{\"schemaVersion\":%d,\"device\":{\"model\":", SETTINGS_REGISTRY_SCHEMA_VERSION);
    append_string(&sb, model);
    sbuf_appendf(&sb, ",\"board\":\"%s\",\"playerVersion\":", board);
    append_string(&sb, app_version_label());
    sbuf_appendf(&sb, "},\"groups\":[");
    for (size_t i = 0; i < settings_registry_group_count(); i++) {
        const setting_group_t * g = settings_registry_group_at(i);
        sbuf_appendf(&sb, "%s{\"id\":\"%s\",", i ? "," : "", g->id);
        append_label(&sb, g->label);
        sbuf_appendf(&sb, "}");
    }
    sbuf_appendf(&sb, "],\"settings\":[");
    for (size_t i = 0; i < settings_registry_count(); i++) {
        if (i) sbuf_appendf(&sb, ",");
        append_schema_entry(&sb, settings_registry_at(i));
    }
    sbuf_appendf(&sb, "],\"actions\":[{\"id\":\"library.rescan\",\"group\":\"library\",");
    append_label(&sb, "Update Music Database");
    sbuf_appendf(&sb, "}]}");
    if (sb.failed) { free(sb.data); return NULL; }
    return sb.data;
}

char * remote_settings_build_values(uint32_t since, bool writable) {
    sbuf_t sb = {0};
    sbuf_appendf(&sb, "{\"epoch\":%u,\"revision\":%u,\"full\":%s,\"writable\":%s,\"values\":{",
                 settings_registry_epoch(), settings_registry_revision(), since ? "false" : "true",
                 writable ? "true" : "false");
    bool first = true;
    for (size_t i = 0; i < settings_registry_count(); i++) {
        const setting_desc_t * d = settings_registry_at(i);
        if (since && settings_registry_changed_at(d) <= since) continue;
        if (!settings_registry_available(d, NULL)) continue;
        if (d->type == SET_STRING) {
            char text[SETTINGS_REGISTRY_STRING_MAX];
            if (!settings_registry_get_string(d, text, sizeof(text))) continue;
            sbuf_appendf(&sb, "%s\"%s\":", first ? "" : ",", d->key);
            append_string(&sb, text);
        } else {
            int value;
            if (!settings_registry_get_int(d, &value)) continue;
            if (d->type == SET_BOOL) sbuf_appendf(&sb, "%s\"%s\":%s", first ? "" : ",", d->key, value ? "true" : "false");
            else sbuf_appendf(&sb, "%s\"%s\":%d", first ? "" : ",", d->key, value);
        }
        first = false;
    }
    sbuf_appendf(&sb, "}}");
    if (sb.failed) { free(sb.data); return NULL; }
    return sb.data;
}
