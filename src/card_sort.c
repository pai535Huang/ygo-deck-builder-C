#include "card_sort.h"
#include "deck_slot.h"
#include "prerelease.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

// 卡片排序数据结构
typedef struct {
    int img_id;
    int card_id;
    gboolean is_extra;
    GdkPixbuf *pixbuf;
    uint32_t type;
    int level;
} CardSortData;

// 释放排序数据
static void free_card_sort_data(CardSortData *data) {
    if (!data) return;
    if (data->pixbuf) g_object_unref(data->pixbuf);
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

// 从API或先行卡数据获取卡片信息（同步方式，简化处理）
static CardSortData* fetch_card_data_sync(int img_id, GdkPixbuf *pixbuf, gboolean is_extra) {
    CardSortData *data = g_new0(CardSortData, 1);
    data->img_id = img_id;
    data->card_id = img_id;
    data->is_extra = is_extra;
    data->pixbuf = pixbuf ? g_object_ref(pixbuf) : NULL;
    data->level = 0;
    data->type = 0;
    
    // 首先尝试从先行卡中查找
    JsonObject *prerelease_card = find_prerelease_card_by_id(img_id);
    if (prerelease_card) {
        // 获取type
        if (json_object_has_member(prerelease_card, "type")) {
            data->type = (uint32_t)json_object_get_int_member(prerelease_card, "type");
        }
        // 获取level
        if (json_object_has_member(prerelease_card, "level")) {
            data->level = json_object_get_int_member(prerelease_card, "level");
        }
        json_object_unref(prerelease_card);
        return data;
    }
    
    // 如果不是先行卡，则从在线API获取（这里使用同步方式）
    // 注意：在实际应用中，这可能会阻塞UI，但为了简化实现，暂时使用同步方式
    char url[256];
    g_snprintf(url, sizeof url, "https://ygocdb.com/api/v0/card/%d", img_id);
    
    SoupSession *session = soup_session_new();
    SoupMessage *msg = soup_message_new("GET", url);
    if (msg) {
        GInputStream *in = soup_session_send(session, msg, NULL, NULL);
        if (in) {
            GByteArray *ba = g_byte_array_new();
            guint8 bufread[4096];
            gssize n;
            while ((n = g_input_stream_read(in, bufread, sizeof bufread, NULL, NULL)) > 0) {
                g_byte_array_append(ba, bufread, (guint)n);
            }
            
            if (ba->len > 0) {
                JsonParser *parser = json_parser_new();
                if (json_parser_load_from_data(parser, (const char*)ba->data, (gssize)ba->len, NULL)) {
                    JsonNode *root = json_parser_get_root(parser);
                    if (root && JSON_NODE_HOLDS_OBJECT(root)) {
                        JsonObject *obj = json_node_get_object(root);
                        
                        // 获取type - 从data对象中读取
                        if (json_object_has_member(obj, "data")) {
                            JsonObject *data_obj = json_object_get_object_member(obj, "data");
                            if (data_obj) {
                                if (json_object_has_member(data_obj, "type")) {
                                    data->type = (uint32_t)json_object_get_int_member(data_obj, "type");
                                }
                                if (json_object_has_member(data_obj, "level")) {
                                    data->level = json_object_get_int_member(data_obj, "level");
                                }
                            }
                        }
                    }
                }
                g_object_unref(parser);
            }
            
            g_byte_array_free(ba, TRUE);
            g_object_unref(in);
        }
        g_object_unref(msg);
    }
    g_object_unref(session);
    
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
        
        if (img_id > 0) {
            GdkPixbuf *pixbuf = slot_get_pixbuf(pic);
            gboolean is_extra = slot_get_is_extra(pic);
            
            // 获取卡片完整信息
            CardSortData *card_data = fetch_card_data_sync(img_id, pixbuf, is_extra);
            if (card_data) {
                card_data->card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "card_id"));
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
        
        if (img_id > 0) {
            GdkPixbuf *pixbuf = slot_get_pixbuf(pic);
            gboolean is_extra = slot_get_is_extra(pic);
            
            // 获取卡片完整信息
            CardSortData *card_data = fetch_card_data_sync(img_id, pixbuf, is_extra);
            if (card_data) {
                card_data->card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "card_id"));
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
    }
    
    *count = cards->len;
    update_count_label(count_label, *count);
    
    g_ptr_array_free(cards, TRUE);
}
