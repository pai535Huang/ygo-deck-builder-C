#ifndef CARD_LANGUAGE_H
#define CARD_LANGUAGE_H

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>

const char *card_english_name(JsonObject *card);
const char *card_deck_sheet_name(JsonObject *card);
const char *card_original_name(JsonObject *card);
const char *card_name_for_language(const char *original, const char *english);
char *card_display_metadata(const char *metadata);

// Returns TRUE if this card's English preview is already shown or being loaded.
gboolean card_show_english_preview(GtkLabel *label, SoupSession *session, int cid,
                                   const char *metadata);

#endif
