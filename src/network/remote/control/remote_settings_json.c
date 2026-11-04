#include "remote_settings.h"
#include "remote_grow_buf.h"
#include "../../../core/persistence/settings_registry.h"
#include "../../../core/system/runtime/app_version.h"
#include "../../../ui/text/i18n/i18n.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void remote_settings_json_escape(char * out, size_t size, const char * text) {
    size_t used = 0;
    for (const unsigned char * p = (const unsigned char *) (text ? text : ""); *p && used + 7 < size; p++) {
        if (*p == '"' || *p == '\\') { out[used++] = '\\'; out[used++] = (char) *p; }
        else if (*p < 0x20) used += (size_t) snprintf(out + used, size - used, "\\u%04x", *p);
        else out[used++] = (char) *p;
    }
    if (size) out[used] = '\0';
}

static void append_string(grow_buf_t * sb, const char * text) {
    char escaped[512];
    remote_settings_json_escape(escaped, sizeof(escaped), text);
    grow_buf_appendf(sb, "\"%s\"", escaped);
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

static void append_label(grow_buf_t * sb, const char * msgid) {
    grow_buf_appendf(sb, "\"label\":");
    append_string(sb, TR(msgid));
    grow_buf_appendf(sb, ",\"labelId\":");
    append_string(sb, msgid);
}

static void append_schema_entry(grow_buf_t * sb, const setting_desc_t * d) {
    const char * why = d->reason;
    bool available = settings_registry_available(d, &why);
    grow_buf_appendf(sb, "{\"key\":\"%s\",\"group\":\"%s\",", d->key, d->group);
    append_label(sb, d->label);
    grow_buf_appendf(sb, ",\"type\":\"%s\",\"apply\":\"%s\",\"available\":%s", type_name(d), remote_settings_apply_name(d->apply),
                 available ? "true" : "false");
    if (!available && why) grow_buf_appendf(sb, ",\"reason\":\"%s\"", why);
    if (d->flags & SET_F_READONLY) {
        grow_buf_appendf(sb, ",\"readOnly\":true");
        if (d->reason) grow_buf_appendf(sb, ",\"reason\":\"%s\"", d->reason);
    }
    if (d->type == SET_INT) grow_buf_appendf(sb, ",\"min\":%d,\"max\":%d", d->min, d->max);
    if (d->unit) grow_buf_appendf(sb, ",\"unit\":\"%s\"", d->unit);
    if (d->type == SET_PRESET) {
        grow_buf_appendf(sb, ",\"values\":[");
        for (int i = 0; i < d->preset_count; i++) grow_buf_appendf(sb, "%s%d", i ? "," : "", d->presets[i]);
        grow_buf_appendf(sb, "]");
    }
    if (d->type == SET_ENUM) {
        grow_buf_appendf(sb, ",\"options\":[");
        for (int i = 0; i < d->option_count; i++) {
            grow_buf_appendf(sb, "%s{\"value\":%d,", i ? "," : "", d->options[i].value);
            append_label(sb, d->options[i].label);
            grow_buf_appendf(sb, "}");
        }
        grow_buf_appendf(sb, "]");
    }
    if (d->depends) grow_buf_appendf(sb, ",\"dependsOn\":\"%s\"", d->depends);
    if (d->flags & SET_F_CONFIRM) grow_buf_appendf(sb, ",\"confirmAbove\":%d", settings_registry_loud_threshold());
    grow_buf_appendf(sb, "}");
}

char * remote_settings_build_schema(void) {
    grow_buf_t sb = {0};
    grow_buf_appendf(&sb, "{\"schemaVersion\":%d,\"device\":{\"model\":", SETTINGS_REGISTRY_SCHEMA_VERSION);
    append_string(&sb, app_board_display_name());
    grow_buf_appendf(&sb, ",\"board\":\"%s\",\"playerVersion\":", app_board_model());
    append_string(&sb, app_version_label());
    grow_buf_appendf(&sb, "},\"groups\":[");
    for (size_t i = 0; i < settings_registry_group_count(); i++) {
        const setting_group_t * g = settings_registry_group_at(i);
        grow_buf_appendf(&sb, "%s{\"id\":\"%s\",", i ? "," : "", g->id);
        append_label(&sb, g->label);
        grow_buf_appendf(&sb, "}");
    }
    grow_buf_appendf(&sb, "],\"settings\":[");
    for (size_t i = 0; i < settings_registry_count(); i++) {
        if (i) grow_buf_appendf(&sb, ",");
        append_schema_entry(&sb, settings_registry_at(i));
    }
    grow_buf_appendf(&sb, "],\"actions\":[{\"id\":\"library.rescan\",\"group\":\"library\",");
    append_label(&sb, "Update Music Database");
    grow_buf_appendf(&sb, "}]}");
    if (sb.failed) { free(sb.data); return NULL; }
    return sb.data;
}

char * remote_settings_build_values(uint32_t since, bool writable) {
    grow_buf_t sb = {0};
    grow_buf_appendf(&sb, "{\"epoch\":%u,\"revision\":%u,\"full\":%s,\"writable\":%s,\"values\":{",
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
            grow_buf_appendf(&sb, "%s\"%s\":", first ? "" : ",", d->key);
            append_string(&sb, text);
        } else {
            int value;
            if (!settings_registry_get_int(d, &value)) continue;
            if (d->type == SET_BOOL) grow_buf_appendf(&sb, "%s\"%s\":%s", first ? "" : ",", d->key, value ? "true" : "false");
            else grow_buf_appendf(&sb, "%s\"%s\":%d", first ? "" : ",", d->key, value);
        }
        first = false;
    }
    grow_buf_appendf(&sb, "}}");
    if (sb.failed) { free(sb.data); return NULL; }
    return sb.data;
}
