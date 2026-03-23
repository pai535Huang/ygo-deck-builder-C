#include "card_sort.h"
#include "deck_slot.h"
#include "prerelease.h"
#include "offline_data.h"
#include <json-glib/json-glib.h>

// 卡片排序数据结构
typedef struct {
    int img_id;
    int card_id;
    gboolean is_extra;
    GdkPixbuf *pixbuf;
    char *en_name;
    uint32_t type;
    int level;
} CardSortData;

// 释放排序数据
static void free_card_sort_data(CardSortData *data) {
    if (!data) return;
    if (data->pixbuf) g_object_unref(data->pixbuf);
    g_free(data->en_name);
    g_free(data);
}

// 获取魔法卡类型优先级（通常=0, 仪式=1, 速攻=2, 装备=3, 永续=4, 场地=5）
static int get_spell_priority(uint32_t type) {
    if (type & 0x80000) return 5;  // TYPE_FIELD
    if (type & 0x20000) return 4;  // TYPE_CONTINUOUS
    if (type & 0x40000) return 3;  // TYPE_EQUIP
    if (type & 0x10000) return 2;  // TYPE_QUICKPLAY
    if (type & 0x80) return 1;     // TYPE_RITUAL
    return 0;  // 通常魔法
}

// 获取陷阱卡类型优先级（通常=0, 永续=1, 反击=2）
static int get_trap_priority(uint32_t type) {
    if (type & 0x100000) return 2;  // TYPE_COUNTER
    if (type & 0x20000) return 1;   // TYPE_CONTINUOUS
    return 0;  // 通常陷阱
}

// 获取额外怪兽类型优先级（融合=0, 同调=1, 超量=2, 连接=3）
static int get_extra_monster_priority(uint32_t type) {
    if (type & 0x4000000) return 3;  // TYPE_LINK
    if (type & 0x800000) return 2;   // TYPE_XYZ
    if (type & 0x2000) return 1;     // TYPE_SYNCHRO
    if (type & 0x40) return 0;       // TYPE_FUSION
    return 99;  // 未知类型排最后
}

// 卡片排序比较函数（用于Main和Side区域）
static gint compare_cards(gconstpointer a, gconstpointer b) {
    const CardSortData *card_a = *(const CardSortData **)a;
    const CardSortData *card_b = *(const CardSortData **)b;
    
    if (!card_a || !card_b) return 0;
    
    uint32_t type_a = card_a->type;
    uint32_t type_b = card_b->type;
    
    // 判断卡片类型
    gboolean is_monster_a = (type_a & 0x1) != 0;  // TYPE_MONSTER
    gboolean is_monster_b = (type_b & 0x1) != 0;
    gboolean is_spell_a = (type_a & 0x2) != 0;    // TYPE_SPELL
    gboolean is_spell_b = (type_b & 0x2) != 0;
    gboolean is_trap_a = (type_a & 0x4) != 0;     // TYPE_TRAP
    gboolean is_trap_b = (type_b & 0x4) != 0;
    
    // 首先按 怪兽-魔法-陷阱 排序
    if (is_monster_a && !is_monster_b) return -1;
    if (!is_monster_a && is_monster_b) return 1;
    if (is_spell_a && is_trap_b) return -1;
    if (is_trap_a && is_spell_b) return 1;
    
    // 怪兽：按等级从大到小排序
    if (is_monster_a && is_monster_b) {
        if (card_a->level != card_b->level) {
            return card_b->level - card_a->level;  // 降序
        }
        // level相同时，按卡片ID排序以保证稳定性
        return card_a->img_id - card_b->img_id;
    }
    
    // 魔法卡：按子类型排序
    if (is_spell_a && is_spell_b) {
        int priority_a = get_spell_priority(type_a);
        int priority_b = get_spell_priority(type_b);
        if (priority_a != priority_b) {
            return priority_a - priority_b;
        }
        // 优先级相同时，按卡片ID排序以保证稳定性
        return card_a->img_id - card_b->img_id;
    }
    
    // 陷阱卡：按子类型排序
    if (is_trap_a && is_trap_b) {
        int priority_a = get_trap_priority(type_a);
        int priority_b = get_trap_priority(type_b);
        if (priority_a != priority_b) {
            return priority_a - priority_b;
        }
        // 优先级相同时，按卡片ID排序以保证稳定性
        return card_a->img_id - card_b->img_id;
    }
    
    // 其他情况按卡片ID排序
    return card_a->img_id - card_b->img_id;
}

// Extra区域卡片排序比较函数（融合-同调-超量-连接，同类按level降序）
static gint compare_extra_cards(gconstpointer a, gconstpointer b) {
    const CardSortData *card_a = *(const CardSortData **)a;
    const CardSortData *card_b = *(const CardSortData **)b;
    
    if (!card_a || !card_b) return 0;
    
    uint32_t type_a = card_a->type;
    uint32_t type_b = card_b->type;
    
    // 获取额外怪兽类型优先级
    int priority_a = get_extra_monster_priority(type_a);
    int priority_b = get_extra_monster_priority(type_b);
    
    // 首先按类型排序（融合-同调-超量-连接）
    if (priority_a != priority_b) {
        return priority_a - priority_b;
    }
    
    // 同类型按level从大到小排序
    if (card_a->level != card_b->level) {
        return card_b->level - card_a->level;  // 降序
    }
    
    // level相同时，按卡片ID排序以保证稳定性
    return card_a->img_id - card_b->img_id;
}

static void fill_sort_fields_from_json(JsonObject *obj, CardSortData *data) {
    if (!obj || !data) return;

    if (json_object_has_member(obj, "type")) {
        data->type = (uint32_t)json_object_get_int_member(obj, "type");
    }
    if (json_object_has_member(obj, "level")) {
        data->level = json_object_get_int_member(obj, "level");
    }

    if ((data->type == 0 || data->level == 0) && json_object_has_member(obj, "data")) {
        JsonObject *inner = json_object_get_object_member(obj, "data");
        if (inner) {
            if (data->type == 0 && json_object_has_member(inner, "type")) {
                data->type = (uint32_t)json_object_get_int_member(inner, "type");
            }
            if (data->level == 0 && json_object_has_member(inner, "level")) {
                data->level = json_object_get_int_member(inner, "level");
            }
        }
    }
}

// 从先行卡/离线数据获取排序所需信息（本地读取，避免主线程网络阻塞）
static CardSortData* fetch_card_data_fast(int img_id, int card_id, GdkPixbuf *pixbuf, gboolean is_extra) {
    CardSortData *data = g_new0(CardSortData, 1);
    data->img_id = img_id;
    data->card_id = card_id;
    data->is_extra = is_extra;
    data->pixbuf = pixbuf ? g_object_ref(pixbuf) : NULL;
    data->level = 0;
    data->type = 0;
    
    // 首先尝试从先行卡中查找
    JsonObject *prerelease_card = find_prerelease_card_by_id(img_id);
    if (prerelease_card) {
        fill_sort_fields_from_json(prerelease_card, data);
        json_object_unref(prerelease_card);
        return data;
    }

    // 离线库优先使用 card_id（cid），失败再用 img_id 反查 cid
    JsonObject *offline_card = NULL;
    if (card_id > 0) {
        offline_card = get_card_by_id_offline(card_id);
    }
    if (!offline_card && img_id > 0) {
        int cid = offline_get_cid_by_img_id(img_id);
        if (cid > 0) {
            offline_card = get_card_by_id_offline(cid);
        }
    }
    // 兼容旧数据：极少数场景 key 可能直接是 img_id
    if (!offline_card && img_id > 0) {
        offline_card = get_card_by_id_offline(img_id);
    }
    if (offline_card) {
        fill_sort_fields_from_json(offline_card, data);
        json_object_unref(offline_card);
    }
    
    return data;
}

// 对指定区域的卡片进行排序
void sort_deck_region(GPtrArray *pics, int *count, GtkLabel *count_label) {
    if (!pics || !count || *count <= 0) return;
    
    // 收集所有非空槽位的卡片数据
    GPtrArray *cards = g_ptr_array_new_with_free_func((GDestroyNotify)free_card_sort_data);
    
    for (int i = 0; i < *count && i < (int)pics->len; i++) {
        GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(pics, i));
        int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "img_id"));
        int card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "card_id"));
        
        if (img_id > 0) {
            GdkPixbuf *pixbuf = slot_get_pixbuf(pic);
            gboolean is_extra = slot_get_is_extra(pic);
            
            // 获取卡片完整信息
            CardSortData *card_data = fetch_card_data_fast(img_id, card_id, pixbuf, is_extra);
            if (card_data) {
                card_data->en_name = g_strdup(slot_get_en_name(pic));
                g_ptr_array_add(cards, card_data);
            }
        }
    }
    
    // 排序
    g_ptr_array_sort(cards, compare_cards);
    
    // 清空原槽位
    for (int i = 0; i < (int)pics->len; i++) {
        GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(pics, i));
        slot_set_pixbuf(pic, NULL);
        slot_set_is_extra(pic, FALSE);
        g_object_set_data(G_OBJECT(pic), "card_id", GINT_TO_POINTER(0));
        g_object_set_data(G_OBJECT(pic), "img_id", GINT_TO_POINTER(0));
        slot_set_en_name(pic, NULL);
    }
    
    // 按排序后的顺序重新放置卡片
    for (guint i = 0; i < cards->len && i < pics->len; i++) {
        CardSortData *card = g_ptr_array_index(cards, i);
        GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(pics, i));
        
        if (card->pixbuf) {
            slot_set_pixbuf(pic, card->pixbuf);
        }
        slot_set_is_extra(pic, card->is_extra);
        g_object_set_data(G_OBJECT(pic), "card_id", GINT_TO_POINTER(card->card_id));
        g_object_set_data(G_OBJECT(pic), "img_id", GINT_TO_POINTER(card->img_id));
        slot_set_en_name(pic, card->en_name);
    }
    
    *count = cards->len;
    update_count_label(count_label, *count);
    
    g_ptr_array_free(cards, TRUE);
}

// 对Extra区域的卡片进行排序（使用Extra专用排序规则）
void sort_extra_region(GPtrArray *pics, int *count, GtkLabel *count_label) {
    if (!pics || !count || *count <= 0) return;
    
    // 收集所有非空槽位的卡片数据
    GPtrArray *cards = g_ptr_array_new_with_free_func((GDestroyNotify)free_card_sort_data);
    
    for (int i = 0; i < *count && i < (int)pics->len; i++) {
        GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(pics, i));
        int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "img_id"));
        int card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "card_id"));
        
        if (img_id > 0) {
            GdkPixbuf *pixbuf = slot_get_pixbuf(pic);
            gboolean is_extra = slot_get_is_extra(pic);
            
            // 获取卡片完整信息
            CardSortData *card_data = fetch_card_data_fast(img_id, card_id, pixbuf, is_extra);
            if (card_data) {
                card_data->en_name = g_strdup(slot_get_en_name(pic));
                g_ptr_array_add(cards, card_data);
            }
        }
    }
    
    // 使用Extra专用排序函数排序
    g_ptr_array_sort(cards, compare_extra_cards);
    
    // 清空原槽位
    for (int i = 0; i < (int)pics->len; i++) {
        GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(pics, i));
        slot_set_pixbuf(pic, NULL);
        slot_set_is_extra(pic, FALSE);
        g_object_set_data(G_OBJECT(pic), "card_id", GINT_TO_POINTER(0));
        g_object_set_data(G_OBJECT(pic), "img_id", GINT_TO_POINTER(0));
        slot_set_en_name(pic, NULL);
    }
    
    // 按排序后的顺序重新放置卡片
    for (guint i = 0; i < cards->len && i < pics->len; i++) {
        CardSortData *card = g_ptr_array_index(cards, i);
        GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(pics, i));
        
        if (card->pixbuf) {
            slot_set_pixbuf(pic, card->pixbuf);
        }
        slot_set_is_extra(pic, card->is_extra);
        g_object_set_data(G_OBJECT(pic), "card_id", GINT_TO_POINTER(card->card_id));
        g_object_set_data(G_OBJECT(pic), "img_id", GINT_TO_POINTER(card->img_id));
        slot_set_en_name(pic, card->en_name);
    }
    
    *count = cards->len;
    update_count_label(count_label, *count);
    
    g_ptr_array_free(cards, TRUE);
}
