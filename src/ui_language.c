#include "ui_language.h"
#include "prefs.h"

#include <gtk/gtk.h>
#include <locale.h>

static const char *const language_keys[UI_LANGUAGE_COUNT] = {"system", "zh_CN", "en"};
static gboolean english;
static gboolean system_english;
static GSimpleAction *language_state;

UiLanguage ui_language_get_preference(void) {
    g_autofree char *value = prefs_get_string("Preferences", "InterfaceLanguage");
    for (guint i = 0; i < G_N_ELEMENTS(language_keys); i++) {
        if (g_strcmp0(value, language_keys[i]) == 0)
            return i;
    }
    return UI_LANGUAGE_SYSTEM;
}

static gboolean language_is_english(UiLanguage language) {
    return language == UI_LANGUAGE_SYSTEM ? system_english : language == UI_LANGUAGE_ENGLISH;
}

gboolean ui_language_set_preference(UiLanguage language) {
    if ((guint)language >= G_N_ELEMENTS(language_keys) ||
        !prefs_set_string("Preferences", "InterfaceLanguage", language_keys[language]))
        return FALSE;

    english = language_is_english(language);
    if (language_state)
        g_simple_action_set_state(language_state, g_variant_new_boolean(english));
    return TRUE;
}

static gboolean system_prefers_english(void) {
    for (const char *const *name = g_get_language_names(); *name; name++) {
        if (g_str_has_prefix(*name, "zh"))
            return FALSE;
        if (g_str_has_prefix(*name, "en"))
            return TRUE;
    }
    return TRUE;
}

void ui_language_init(void) {
    setlocale(LC_ALL, "");
    system_english = system_prefers_english();
    UiLanguage preference = ui_language_get_preference();
    english = language_is_english(preference);
    if (preference != UI_LANGUAGE_SYSTEM) {
        // GTK and libadwaita use gettext for their own strings.
        g_setenv("LANGUAGE", english ? "en" : "zh_CN", TRUE);
        const char *messages = setlocale(LC_MESSAGES, NULL);
        if (g_strcmp0(messages, "C") == 0 || g_strcmp0(messages, "POSIX") == 0)
            setlocale(LC_MESSAGES, "C.UTF-8");
        gtk_disable_setlocale();
    }
}

gboolean ui_is_english(void) {
    return english;
}

static gboolean translate_bound_text(GBinding *binding, const GValue *source,
                                     GValue *target, gpointer user_data) {
    (void)binding;
    const char *const *strings = user_data;
    g_value_set_string(target, strings[g_variant_get_boolean(g_value_get_variant(source))]);
    return TRUE;
}

void ui_language_bind_text(GObject *target, const char *property,
                           const char *chinese, const char *english_text) {
    if (!language_state)
        language_state = g_simple_action_new_stateful("interface-language", NULL,
                                                       g_variant_new_boolean(english));
    char **strings = g_new0(char *, 3);
    strings[0] = g_strdup(chinese);
    strings[1] = g_strdup(english_text);
    g_object_bind_property_full(language_state, "state", target, property,
                                G_BINDING_SYNC_CREATE, translate_bound_text, NULL,
                                strings, (GDestroyNotify)g_strfreev);
}
