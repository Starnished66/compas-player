#include <glib.h>

#include <stdarg.h>

/* The constructor loads this DSO only into sys_server. Clear the inherited
 * variable after the loader has mapped it so child utilities do not load it. */
__attribute__((constructor))
static void sys_server_mpris_compat_init(void)
{
    g_unsetenv("LD_PRELOAD");
}

static gboolean text_equal(const gchar *a, const gchar *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/* sys_server publishes xesam:artist (and xesam:genre) as strings, although
 * MPRIS requires string arrays. Correct only those malformed a{sv} entries;
 * every other g_variant_builder_add() call keeps GLib's normal format-string
 * behavior through g_variant_new_va(). */
__attribute__((visibility("default")))
void g_variant_builder_add(GVariantBuilder *builder, const gchar *format_string, ...)
{
    va_list args;
    va_start(args, format_string);
    GVariant *entry = g_variant_new_va(format_string, NULL, &args);
    va_end(args);

    if (!entry) return;

    if (g_variant_is_of_type(entry, G_VARIANT_TYPE("{sv}"))) {
        GVariant *key_value = g_variant_get_child_value(entry, 0);
        GVariant *boxed_value = g_variant_get_child_value(entry, 1);
        const gchar *key = g_variant_get_string(key_value, NULL);

        if ((text_equal(key, "xesam:artist") ||
             text_equal(key, "xesam:genre")) &&
            g_variant_is_of_type(boxed_value, G_VARIANT_TYPE_VARIANT)) {
            GVariant *value = g_variant_get_variant(boxed_value);
            if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
                const gchar *text = g_variant_get_string(value, NULL);
                GVariant *strings;
                if (text[0] == '\0') {
                    strings = g_variant_new_strv(NULL, 0);
                } else {
                    const gchar *items[] = { text, NULL };
                    strings = g_variant_new_strv(items, 1);
                }

                GVariant *fixed_entry = g_variant_new_dict_entry(
                    g_variant_new_string(key), g_variant_new_variant(strings));
                g_variant_builder_add_value(builder, fixed_entry);
                g_variant_unref(value);
                g_variant_unref(boxed_value);
                g_variant_unref(key_value);
                g_variant_unref(entry);
                return;
            }
            g_variant_unref(value);
        }

        g_variant_unref(boxed_value);
        g_variant_unref(key_value);
    }

    g_variant_builder_add_value(builder, entry);
}
