#include "deck_export_sheet.h"

#include "offline_data.h"
#include <cairo-pdf.h>
#include <pango/pangocairo.h>
#include <ctype.h>
#include <string.h>

#define TYPE_SPELL 0x2u
#define TYPE_TRAP  0x4u

// 版面基准参数（单位：PDF point）
#define TOP_AREA_ROW_HEIGHT    13.98
#define BOTTOM_AREA_ROW_HEIGHT 13.98
#define CARD_NAME_FONT_SIZE 4
#define MONSTER_NAME_X 20.0
#define SPELL_NAME_X 189.2
#define TRAP_NAME_X 359.6
#define EXTRA_NAME_X 20.0
#define SIDE_NAME_X 189.2
#define TOP_NAME_START_Y 125.22
#define BOTTOM_NAME_START_Y 579.54
#define TOP_QTY_CENTER_START_Y 122.16
#define BOTTOM_QTY_CENTER_START_Y 575.48
#define KONAMI_ID_X 103
#define KONAMI_ID_Y 50
#define KONAMI_ID_LETTER_SPACING 10.55
#define MAIN_TOTAL_X 169.25
#define MAIN_TOTAL_Y 90.42
#define MONSTER_TOTAL_X 169.0
#define MONSTER_TOTAL_Y 544.5
#define SPELL_TOTAL_X 339.0
#define SPELL_TOTAL_Y 544.5
#define TRAP_TOTAL_X 509.0
#define TRAP_TOTAL_Y 544.5
#define EXTRA_TOTAL_X 169.0
#define EXTRA_TOTAL_Y 788.3
#define SIDE_TOTAL_X 339.0
#define SIDE_TOTAL_Y 788.3

typedef struct {
    int card_id;
    int count;
    char *card_name;
} DeckCardCount;

static GQuark deck_export_sheet_error_quark(void) {
    return g_quark_from_static_string("deck-export-sheet-error");
}

static void deck_card_count_free(gpointer data) {
    DeckCardCount *entry = (DeckCardCount*)data;
    if (!entry) return;
    g_free(entry->card_name);
    g_free(entry);
}

static int compare_deck_card_count(gconstpointer a, gconstpointer b) {
    const DeckCardCount *left = *((const DeckCardCount* const*)a);
    const DeckCardCount *right = *((const DeckCardCount* const*)b);
    if (!left || !right) return 0;
    if (!left->card_name && !right->card_name) return left->card_id - right->card_id;
    if (!left->card_name) return 1;
    if (!right->card_name) return -1;
    int cmp = g_utf8_collate(left->card_name, right->card_name);
    if (cmp != 0) return cmp;
    return left->card_id - right->card_id;
}

static char* pick_card_name_from_card(JsonObject *card_obj) {
    if (!card_obj) return NULL;

    if (json_object_has_member(card_obj, "jp_name")) {
        const char *name_text = json_object_get_string_member(card_obj, "jp_name");
        if (name_text && *name_text) return g_strdup(name_text);
    }

    if (json_object_has_member(card_obj, "cn_name")) {
        const char *cn = json_object_get_string_member(card_obj, "cn_name");
        if (cn && *cn) return g_strdup(cn);
    }

    if (json_object_has_member(card_obj, "en_name")) {
        const char *en = json_object_get_string_member(card_obj, "en_name");
        if (en && *en) return g_strdup(en);
    }

    return NULL;
}

static uint32_t pick_type_from_card(JsonObject *card_obj) {
    if (!card_obj) return 0;

    if (json_object_has_member(card_obj, "type")) {
        return (uint32_t)json_object_get_int_member(card_obj, "type");
    }

    if (json_object_has_member(card_obj, "data")) {
        JsonObject *data_obj = json_object_get_object_member(card_obj, "data");
        if (data_obj && json_object_has_member(data_obj, "type")) {
            return (uint32_t)json_object_get_int_member(data_obj, "type");
        }
    }

    return 0;
}

static gboolean lookup_card_meta(int card_id, int img_id, char **out_card_name, uint32_t *out_type) {
    JsonObject *card_obj = NULL;

    if (card_id > 0) {
        card_obj = get_card_by_id_offline(card_id);
    }

    if (!card_obj && img_id > 0 && img_id != card_id) {
        card_obj = get_card_by_id_offline(img_id);
    }

    if (!card_obj) {
        *out_card_name = NULL;
        *out_type = 0;
        return FALSE;
    }

    *out_card_name = pick_card_name_from_card(card_obj);
    *out_type = pick_type_from_card(card_obj);
    json_object_unref(card_obj);
    return TRUE;
}

static void add_card_count_entry(GHashTable *map, GPtrArray *array, int card_id, const char *card_name) {
    DeckCardCount *entry = g_hash_table_lookup(map, GINT_TO_POINTER(card_id));
    if (entry) {
        entry->count += 1;
        return;
    }

    entry = g_new0(DeckCardCount, 1);
    entry->card_id = card_id;
    entry->count = 1;
    entry->card_name = g_strdup(card_name ? card_name : "(Unknown)");
    g_hash_table_insert(map, GINT_TO_POINTER(card_id), entry);
    g_ptr_array_add(array, entry);
}

static void collect_main_cards(
    GPtrArray *main_pics,
    int main_count,
    GPtrArray *monster,
    GPtrArray *spell,
    GPtrArray *trap,
    GHashTable *monster_map,
    GHashTable *spell_map,
    GHashTable *trap_map
) {
    if (!main_pics) return;

    int max = MIN(main_count, (int)main_pics->len);
    for (int i = 0; i < max; i++) {
        GtkWidget *slot = GTK_WIDGET(g_ptr_array_index(main_pics, i));
        if (!slot) continue;

        int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(slot), "img_id"));
        int card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(slot), "card_id"));
        if (img_id <= 0 && card_id <= 0) continue;

        int lookup_id = card_id > 0 ? card_id : img_id;
        char *card_name = NULL;
        uint32_t type = 0;
        lookup_card_meta(lookup_id, img_id, &card_name, &type);

        if (!card_name || !*card_name) {
            g_free(card_name);
            card_name = g_strdup_printf("%d", img_id > 0 ? img_id : lookup_id);
        }

        if (type & TYPE_SPELL) {
            add_card_count_entry(spell_map, spell, lookup_id, card_name);
        } else if (type & TYPE_TRAP) {
            add_card_count_entry(trap_map, trap, lookup_id, card_name);
        } else {
            add_card_count_entry(monster_map, monster, lookup_id, card_name);
        }

        g_free(card_name);
    }
}

static void collect_generic_cards(
    GPtrArray *pics,
    int count,
    GPtrArray *target,
    GHashTable *target_map
) {
    if (!pics) return;

    int max = MIN(count, (int)pics->len);
    for (int i = 0; i < max; i++) {
        GtkWidget *slot = GTK_WIDGET(g_ptr_array_index(pics, i));
        if (!slot) continue;

        int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(slot), "img_id"));
        int card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(slot), "card_id"));
        if (img_id <= 0 && card_id <= 0) continue;

        int lookup_id = card_id > 0 ? card_id : img_id;
        char *card_name = NULL;
        uint32_t type = 0;
        lookup_card_meta(lookup_id, img_id, &card_name, &type);
        (void)type;

        if (!card_name || !*card_name) {
            g_free(card_name);
            card_name = g_strdup_printf("%d", img_id > 0 ? img_id : lookup_id);
        }

        add_card_count_entry(target_map, target, lookup_id, card_name);
        g_free(card_name);
    }
}

static void draw_name_cell(cairo_t *cr, const char *text, double x, double baseline_y, double max_width) {
    if (!text) return;

    PangoLayout *layout = pango_cairo_create_layout(cr);
    pango_layout_set_text(layout, text, -1);
    pango_layout_set_width(layout, (int)(max_width * PANGO_SCALE));
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, "Sans");
    pango_font_description_set_size(desc, CARD_NAME_FONT_SIZE * PANGO_SCALE);
    pango_layout_set_font_description(layout, desc);

    int layout_height = 0;
    pango_layout_get_pixel_size(layout, NULL, &layout_height);

    cairo_save(cr);
    cairo_move_to(cr, x, baseline_y - (double)layout_height + 1.0);
    pango_cairo_show_layout(cr, layout);
    cairo_restore(cr);

    pango_font_description_free(desc);
    g_object_unref(layout);
}

static void draw_qty_cell(
    cairo_t *cr,
    int count,
    double cell_left,
    double cell_right,
    double center_y
) {
    char text[8];
    g_snprintf(text, sizeof(text), "%d", count);

    cairo_text_extents_t ext;
    cairo_font_extents_t font_ext;
    cairo_text_extents(cr, text, &ext);
    cairo_font_extents(cr, &font_ext);

    double center_x = (cell_left + cell_right) * 0.5;
    double draw_x = center_x - (ext.x_bearing + ext.width * 0.5);
    double draw_y = center_y + (font_ext.ascent - font_ext.descent) * 0.5;

    cairo_move_to(cr, draw_x, draw_y);
    cairo_show_text(cr, text);
}

static void draw_single_column_entries(
    cairo_t *cr,
    GPtrArray *entries,
    double row_h,
    double qty_center_start_y,
    double name_start_y,
    int rows,
    double name_x,
    double qty_cell_left,
    double qty_cell_right
) {
    const int max_cells = rows;

    int limit = MIN((int)entries->len, max_cells);
    for (int i = 0; i < limit; i++) {
        DeckCardCount *entry = g_ptr_array_index(entries, i);
        if (!entry) continue;

        int row = i;
        double qty_center_y = qty_center_start_y + row_h * row;
        double baseline_y = name_start_y + row_h * row;

        draw_name_cell(cr, entry->card_name, name_x, baseline_y, 140.0);
        draw_qty_cell(cr, entry->count, qty_cell_left, qty_cell_right, qty_center_y);
    }
}

static gboolean format_konami_id_10_digits(const char *input, char out_id[11]) {
    if (!input || !*input) return FALSE;

    int out_index = 0;
    for (const char *p = input; *p && out_index < 10; p++) {
        if (isdigit((unsigned char)*p)) {
            out_id[out_index++] = *p;
        }
    }

    if (out_index != 10) return FALSE;

    out_id[10] = '\0';
    return TRUE;
}

static void draw_spaced_text(cairo_t *cr, const char *text, double x, double y, double letter_spacing) {
    if (!text || !*text) return;

    double current_x = x;
    for (const char *p = text; *p; p++) {
        char ch[2] = {*p, '\0'};

        cairo_text_extents_t ext;
        cairo_text_extents(cr, ch, &ext);

        cairo_move_to(cr, current_x, y);
        cairo_show_text(cr, ch);

        current_x += ext.x_advance + letter_spacing;
    }
}

static gboolean load_and_draw_template(cairo_t *cr, GError **error) {
    GError *load_error = NULL;
    GdkPixbuf *template = gdk_pixbuf_new_from_resource(
        "/com/pai535/YGODeckBuilder/deck_ja_template.png",
        &load_error
    );

    if (!template) {
        g_set_error(error,
                    deck_export_sheet_error_quark(),
                    1,
                    "无法加载卡表模板资源: /com/pai535/YGODeckBuilder/deck_ja_template.png (%s)",
                    load_error ? load_error->message : "unknown");
        g_clear_error(&load_error);
        return FALSE;
    }

    const double page_w = 533.76;
    const double page_h = 810.0;
    int bg_w = gdk_pixbuf_get_width(template);
    int bg_h = gdk_pixbuf_get_height(template);

    cairo_save(cr);
    cairo_scale(cr, page_w / bg_w, page_h / bg_h);
    gdk_cairo_set_source_pixbuf(cr, template, 0, 0);
    cairo_paint(cr);
    cairo_restore(cr);

    g_object_unref(template);
    return TRUE;
}

gboolean generate_deck_sheet_pdf(
    GPtrArray *main_pics, int main_count,
    GPtrArray *extra_pics, int extra_count,
    GPtrArray *side_pics, int side_count,
    const char *konami_id,
    const char *filepath,
    GError **error
) {
    if (!filepath || !*filepath) {
        g_set_error(error, deck_export_sheet_error_quark(), 2, "输出路径无效");
        return FALSE;
    }

    GPtrArray *monster = g_ptr_array_new_with_free_func(deck_card_count_free);
    GPtrArray *spell = g_ptr_array_new_with_free_func(deck_card_count_free);
    GPtrArray *trap = g_ptr_array_new_with_free_func(deck_card_count_free);
    GPtrArray *extra = g_ptr_array_new_with_free_func(deck_card_count_free);
    GPtrArray *side = g_ptr_array_new_with_free_func(deck_card_count_free);

    GHashTable *monster_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *spell_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *trap_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *extra_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    GHashTable *side_map = g_hash_table_new(g_direct_hash, g_direct_equal);

    collect_main_cards(main_pics, main_count, monster, spell, trap, monster_map, spell_map, trap_map);
    collect_generic_cards(extra_pics, extra_count, extra, extra_map);
    collect_generic_cards(side_pics, side_count, side, side_map);

    g_ptr_array_sort(monster, compare_deck_card_count);
    g_ptr_array_sort(spell, compare_deck_card_count);
    g_ptr_array_sort(trap, compare_deck_card_count);
    g_ptr_array_sort(extra, compare_deck_card_count);
    g_ptr_array_sort(side, compare_deck_card_count);

    const double page_w = 533.76;
    const double page_h = 810.0;

    cairo_surface_t *surface = cairo_pdf_surface_create(filepath, page_w, page_h);
    cairo_status_t surface_status = cairo_surface_status(surface);
    if (surface_status != CAIRO_STATUS_SUCCESS) {
        g_set_error(error,
                    deck_export_sheet_error_quark(),
                    3,
                    "创建 PDF 失败: %s",
                    cairo_status_to_string(surface_status));
        cairo_surface_destroy(surface);
        g_hash_table_destroy(monster_map);
        g_hash_table_destroy(spell_map);
        g_hash_table_destroy(trap_map);
        g_hash_table_destroy(extra_map);
        g_hash_table_destroy(side_map);
        g_ptr_array_unref(monster);
        g_ptr_array_unref(spell);
        g_ptr_array_unref(trap);
        g_ptr_array_unref(extra);
        g_ptr_array_unref(side);
        return FALSE;
    }

    cairo_t *cr = cairo_create(surface);
    if (!load_and_draw_template(cr, error)) {
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        g_hash_table_destroy(monster_map);
        g_hash_table_destroy(spell_map);
        g_hash_table_destroy(trap_map);
        g_hash_table_destroy(extra_map);
        g_hash_table_destroy(side_map);
        g_ptr_array_unref(monster);
        g_ptr_array_unref(spell);
        g_ptr_array_unref(trap);
        g_ptr_array_unref(extra);
        g_ptr_array_unref(side);
        return FALSE;
    }

    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 8.8);

    char konami_id_10[11];
    if (format_konami_id_10_digits(konami_id, konami_id_10)) {
        draw_spaced_text(cr, konami_id_10, KONAMI_ID_X, KONAMI_ID_Y, KONAMI_ID_LETTER_SPACING);
    } else if (konami_id && *konami_id) {
        draw_spaced_text(cr, konami_id, KONAMI_ID_X, KONAMI_ID_Y, KONAMI_ID_LETTER_SPACING);
    }

    // 模板布局：
    // - 主卡组区域为三列并排（怪兽/魔法/陷阱各占一列），纵向连续填写
    // - 下方区域为两列并排（额外/副卡组各占一列），纵向连续填写
    draw_single_column_entries(cr, monster, TOP_AREA_ROW_HEIGHT, TOP_QTY_CENTER_START_Y, TOP_NAME_START_Y,    32, MONSTER_NAME_X, 161.3, 187.2);
    draw_single_column_entries(cr, spell,   TOP_AREA_ROW_HEIGHT, TOP_QTY_CENTER_START_Y, TOP_NAME_START_Y,    32, SPELL_NAME_X,   331.7, 357.6);
    draw_single_column_entries(cr, trap,    TOP_AREA_ROW_HEIGHT, TOP_QTY_CENTER_START_Y, TOP_NAME_START_Y,    32, TRAP_NAME_X,    501.6, 527.0);
    draw_single_column_entries(cr, extra,   BOTTOM_AREA_ROW_HEIGHT, BOTTOM_QTY_CENTER_START_Y, BOTTOM_NAME_START_Y, 15, EXTRA_NAME_X, 161.3, 187.2);
    draw_single_column_entries(cr, side,    BOTTOM_AREA_ROW_HEIGHT, BOTTOM_QTY_CENTER_START_Y, BOTTOM_NAME_START_Y, 15, SIDE_NAME_X,  331.7, 357.6);

    // 绘制统计信息
    // 计算各类别卡片总数（包括数量）
    int monster_total = 0;
    for (guint i = 0; i < monster->len; i++) {
        DeckCardCount *entry = g_ptr_array_index(monster, i);
        if (entry) monster_total += entry->count;
    }
    int spell_total = 0;
    for (guint i = 0; i < spell->len; i++) {
        DeckCardCount *entry = g_ptr_array_index(spell, i);
        if (entry) spell_total += entry->count;
    }
    int trap_total = 0;
    for (guint i = 0; i < trap->len; i++) {
        DeckCardCount *entry = g_ptr_array_index(trap, i);
        if (entry) trap_total += entry->count;
    }
    int extra_total = 0;
    for (guint i = 0; i < extra->len; i++) {
        DeckCardCount *entry = g_ptr_array_index(extra, i);
        if (entry) extra_total += entry->count;
    }
    int side_total = 0;
    for (guint i = 0; i < side->len; i++) {
        DeckCardCount *entry = g_ptr_array_index(side, i);
        if (entry) side_total += entry->count;
    }

    int main_total = monster_total + spell_total + trap_total;

    char stat_buf[64];

    // 主卡组总数（显示在怪兽卡区域上方，与左边第一列枚数部分水平对齐）
    g_snprintf(stat_buf, sizeof(stat_buf), "%d", main_total);
    cairo_move_to(cr, MAIN_TOTAL_X, MAIN_TOTAL_Y);
    cairo_show_text(cr, stat_buf);

    // 怪兽总数
    g_snprintf(stat_buf, sizeof(stat_buf), "%d", monster_total);
    cairo_move_to(cr, MONSTER_TOTAL_X, MONSTER_TOTAL_Y);
    cairo_show_text(cr, stat_buf);

    // 魔法总数
    g_snprintf(stat_buf, sizeof(stat_buf), "%d", spell_total);
    cairo_move_to(cr, SPELL_TOTAL_X, SPELL_TOTAL_Y);
    cairo_show_text(cr, stat_buf);

    // 陷阱总数
    g_snprintf(stat_buf, sizeof(stat_buf), "%d", trap_total);
    cairo_move_to(cr, TRAP_TOTAL_X, TRAP_TOTAL_Y);
    cairo_show_text(cr, stat_buf);

    // 额外卡组总数
    g_snprintf(stat_buf, sizeof(stat_buf), "%d", extra_total);
    cairo_move_to(cr, EXTRA_TOTAL_X, EXTRA_TOTAL_Y);
    cairo_show_text(cr, stat_buf);

    // 副卡组总数
    g_snprintf(stat_buf, sizeof(stat_buf), "%d", side_total);
    cairo_move_to(cr, SIDE_TOTAL_X, SIDE_TOTAL_Y);
    cairo_show_text(cr, stat_buf);

    cairo_show_page(cr);
    cairo_destroy(cr);

    cairo_surface_finish(surface);
    cairo_status_t final_status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);

    g_hash_table_destroy(monster_map);
    g_hash_table_destroy(spell_map);
    g_hash_table_destroy(trap_map);
    g_hash_table_destroy(extra_map);
    g_hash_table_destroy(side_map);
    g_ptr_array_unref(monster);
    g_ptr_array_unref(spell);
    g_ptr_array_unref(trap);
    g_ptr_array_unref(extra);
    g_ptr_array_unref(side);

    if (final_status != CAIRO_STATUS_SUCCESS) {
        g_set_error(error,
                    deck_export_sheet_error_quark(),
                    4,
                    "写入 PDF 失败: %s",
                    cairo_status_to_string(final_status));
        return FALSE;
    }

    return TRUE;
}
