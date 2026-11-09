#include "card_shuffle.h"
#include "deck_slot.h"

typedef struct {
    int img_id;
    gpointer card_id;
    gboolean is_extra;
    GdkPixbuf *pixbuf;
    char *en_name;
    char *sheet_name;
    uint32_t type;
    gpointer needs_metadata;
} ShuffleCard;

static void shuffle_card_free(gpointer data) {
    ShuffleCard *card = data;
    g_clear_object(&card->pixbuf);
    g_free(card->en_name);
    g_free(card->sheet_name);
    g_free(card);
}

void shuffle_deck_region(GPtrArray *pics, int *count, GtkLabel *count_label) {
    if (!pics || !count || *count <= 1) return;

    g_autoptr(GPtrArray) cards = g_ptr_array_new_with_free_func(shuffle_card_free);
    for (int i = 0; i < *count && i < (int)pics->len; i++) {
        GtkWidget *slot = g_ptr_array_index(pics, i);
        int img_id = slot_get_img_id(slot);
        if (img_id <= 0) continue;

        ShuffleCard *card = g_new0(ShuffleCard, 1);
        card->img_id = img_id;
        card->card_id = g_object_get_data(G_OBJECT(slot), "card_id");
        card->is_extra = slot_get_is_extra(slot);
        card->pixbuf = slot_get_pixbuf(slot);
        if (card->pixbuf) g_object_ref(card->pixbuf);
        card->en_name = g_strdup(slot_get_en_name(slot));
        card->sheet_name = g_strdup(slot_get_sheet_name(slot));
        card->type = slot_get_card_type(slot);
        card->needs_metadata = g_object_get_data(G_OBJECT(slot), "needs-card-metadata");
        g_ptr_array_add(cards, card);
    }

    for (guint remaining = cards->len; remaining > 1; remaining--) {
        guint i = remaining - 1;
        guint j = g_random_int_range(0, remaining);
        gpointer temp = g_ptr_array_index(cards, i);
        g_ptr_array_index(cards, i) = g_ptr_array_index(cards, j);
        g_ptr_array_index(cards, j) = temp;
    }

    for (guint i = 0; i < pics->len; i++)
        slot_clear_card(g_ptr_array_index(pics, i));

    for (guint i = 0; i < cards->len; i++) {
        const ShuffleCard *card = g_ptr_array_index(cards, i);
        GtkWidget *slot = g_ptr_array_index(pics, i);
        slot_set_pixbuf(slot, card->pixbuf);
        slot_set_is_extra(slot, card->is_extra);
        g_object_set_data(G_OBJECT(slot), "card_id", card->card_id);
        g_object_set_data(G_OBJECT(slot), "img_id", GINT_TO_POINTER(card->img_id));
        slot_set_en_name(slot, card->en_name);
        slot_set_card_details(slot, card->type, card->sheet_name);
        g_object_set_data(G_OBJECT(slot), "needs-card-metadata", card->needs_metadata);
    }

    *count = cards->len;
    update_count_label(count_label, *count);
}
