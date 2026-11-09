#include "deck_clear.h"
#include "deck_slot.h"

// 清空指定区域的所有卡片
void clear_deck_region(GPtrArray *pics, int *count, GtkLabel *count_label) {
    if (!pics || !count) return;
    
    // 清空所有槽位
    for (guint i = 0; i < pics->len; i++) {
        GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(pics, i));
        slot_clear_card(pic);
    }
    
    *count = 0;
    update_count_label(count_label, 0);
}

// 清空所有卡组区域
void clear_all_deck_regions(
    GPtrArray *main_pics, int *main_count, GtkLabel *main_label,
    GPtrArray *extra_pics, int *extra_count, GtkLabel *extra_label,
    GPtrArray *side_pics, int *side_count, GtkLabel *side_label
) {
    if (main_pics && main_count) {
        clear_deck_region(main_pics, main_count, main_label);
    }
    
    if (extra_pics && extra_count) {
        clear_deck_region(extra_pics, extra_count, extra_label);
    }
    
    if (side_pics && side_count) {
        clear_deck_region(side_pics, side_count, side_label);
    }
}
