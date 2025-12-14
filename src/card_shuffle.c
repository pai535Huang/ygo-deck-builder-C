#include "card_shuffle.h"
#include "deck_slot.h"

// 使用Fisher-Yates洗牌算法打乱指定区域的卡片
void shuffle_deck_region(GPtrArray *pics, int *count, GtkLabel *count_label) {
    if (!pics || !count || *count <= 1) return;
    
    // 收集所有非空槽位的卡片数据
    GPtrArray *cards = g_ptr_array_new();
    
    for (int i = 0; i < *count && i < (int)pics->len; i++) {
        GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(pics, i));
        int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "img_id"));
        
        if (img_id > 0) {
            // 存储卡片信息：img_id, card_id, pixbuf, is_extra
            gpointer *card_info = g_new(gpointer, 4);
            card_info[0] = GINT_TO_POINTER(img_id);
            card_info[1] = g_object_get_data(G_OBJECT(pic), "card_id");
            GdkPixbuf *pixbuf = slot_get_pixbuf(pic);
            if (pixbuf) g_object_ref(pixbuf);
            card_info[2] = pixbuf;
            card_info[3] = GINT_TO_POINTER(slot_get_is_extra(pic));
            g_ptr_array_add(cards, card_info);
        }
    }
    
    // Fisher-Yates洗牌算法
    for (guint i = cards->len - 1; i > 0; i--) {
        guint j = g_random_int_range(0, i + 1);
        gpointer temp = g_ptr_array_index(cards, i);
        g_ptr_array_index(cards, i) = g_ptr_array_index(cards, j);
        g_ptr_array_index(cards, j) = temp;
    }
    
    // 清空原槽位
    for (int i = 0; i < (int)pics->len; i++) {
        GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(pics, i));
        slot_set_pixbuf(pic, NULL);
        slot_set_is_extra(pic, FALSE);
        g_object_set_data(G_OBJECT(pic), "card_id", GINT_TO_POINTER(0));
        g_object_set_data(G_OBJECT(pic), "img_id", GINT_TO_POINTER(0));
    }
    
    // 按打乱后的顺序重新放置卡片（严格从序号0开始紧密排列）
    for (guint i = 0; i < cards->len && i < pics->len; i++) {
        gpointer *card_info = g_ptr_array_index(cards, i);
        GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(pics, i));
        
        int img_id = GPOINTER_TO_INT(card_info[0]);
        int card_id = GPOINTER_TO_INT(card_info[1]);
        GdkPixbuf *pixbuf = (GdkPixbuf *)card_info[2];
        gboolean is_extra = GPOINTER_TO_INT(card_info[3]);
        
        if (pixbuf) {
            slot_set_pixbuf(pic, pixbuf);
            g_object_unref(pixbuf);
        }
        slot_set_is_extra(pic, is_extra);
        g_object_set_data(G_OBJECT(pic), "card_id", GINT_TO_POINTER(card_id));
        g_object_set_data(G_OBJECT(pic), "img_id", GINT_TO_POINTER(img_id));
        
        g_free(card_info);
    }
    
    *count = cards->len;
    update_count_label(count_label, *count);
    
    g_ptr_array_free(cards, TRUE);
}
