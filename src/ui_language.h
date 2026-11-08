#ifndef UI_LANGUAGE_H
#define UI_LANGUAGE_H

#include <glib-object.h>

typedef enum {
    UI_LANGUAGE_SYSTEM,
    UI_LANGUAGE_CHINESE,
    UI_LANGUAGE_ENGLISH,
    UI_LANGUAGE_COUNT
} UiLanguage;

void ui_language_init(void);
UiLanguage ui_language_get_preference(void);
gboolean ui_language_set_preference(UiLanguage language);
gboolean ui_is_english(void);
void ui_language_bind_text(GObject *target, const char *property,
                           const char *chinese, const char *english);

#define TR(chinese, english) (ui_is_english() ? (english) : (chinese))

#endif
