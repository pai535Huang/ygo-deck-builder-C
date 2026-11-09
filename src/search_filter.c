#include "search_filter.h"
#include "ui_language.h"
#include "card_language.h"
#include "image_loader.h"
#include "prerelease.h"
#include "offline_data.h"
#include "alt_art.h"
#include "deck_io.h"
#include "dnd_manager.h"
#include "deck_slot.h"
#include "card_info.h"
#include "forbidden_list.h"
#include "json_utils.h"
#include <string.h>

// 搜索结果上限：保护右侧列表和图片队列，避免一次返回过多卡片拖慢 UI。
#define SEARCH_MAX_RESULTS 500
#define UNKNOWN_CARD_STAT (-2)

// 全局变量：是否在搜索结果中显示先行卡（默认显示）
extern gboolean show_prerelease_cards;

// 外部函数声明
extern void list_clear(GtkListBox *list);
extern void draw_pixbuf_scaled(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data);
extern void on_drawing_area_destroy(GtkWidget *widget, gpointer user_data);
extern void free_card_preview(gpointer data);
extern void on_result_row_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
extern void on_result_row_released(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data);
extern void on_result_row_enter(GtkEventControllerMotion *controller, double x, double y, gpointer user_data);
extern void update_genesys_score(SearchUI *ui);  // GENESYS 总分更新
extern void apply_overlay_to_widget(GtkWidget *w, SearchUI *ui);  // 设置卡图角标
extern void update_deck_overlays(SearchUI *ui);  // 批量更新卡组角标

typedef struct {
    SearchUI *ui;
    guint64 generation;
} SearchRequestCtx;

static void load_search_thumb(SearchUI *ui, GtkWidget *picture, GtkStack *stack,
                              int img_id, gboolean prerelease) {
    g_autofree char *local_path = prerelease ? get_prerelease_card_image_path(img_id) : NULL;
    load_card_picture_async(ui->session, picture, stack, img_id, TRUE, prerelease, local_path);
}

static gboolean art_variants_contains(GArray *variants, int art_id) {
    if (!variants) return FALSE;
    for (guint i = 0; i < variants->len; i++) {
        if (g_array_index(variants, int, i) == art_id) return TRUE;
    }
    return FALSE;
}

// 找到搜索结果行里的缩略图栈与绘图区
static gboolean get_search_row_picture(GtkWidget *row_box, GtkStack **out_stack, GtkWidget **out_picture) {
    if (!row_box || !out_stack || !out_picture) return FALSE;
    GtkWidget *list_row = gtk_widget_get_ancestor(row_box, GTK_TYPE_LIST_BOX_ROW);
    if (!list_row) return FALSE;
    GtkWidget *child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(list_row));
    if (!child) return FALSE;
    GtkWidget *stack = gtk_widget_get_first_child(child);
    if (!stack || !GTK_IS_STACK(stack)) return FALSE;
    GtkWidget *picture = gtk_stack_get_child_by_name(GTK_STACK(stack), "picture");
    if (!picture || !GTK_IS_DRAWING_AREA(picture)) return FALSE;
    *out_stack = GTK_STACK(stack);
    *out_picture = picture;
    return TRUE;
}

static void update_art_buttons_recursive(GtkWidget *widget, GArray *variants, int selected_art) {
    if (!widget || !variants) return;
    for (GtkWidget *child = gtk_widget_get_first_child(widget);
         child; child = gtk_widget_get_next_sibling(child)) {
        gpointer index_data = g_object_get_data(G_OBJECT(child), "art_index");
        if (index_data) {
            int index = GPOINTER_TO_INT(index_data) - 1;
            int art_id = 0;
            if (index >= 0 && index < (int)variants->len) {
                art_id = g_array_index(variants, int, index);
            }
            if (art_id == selected_art) {
                gtk_widget_add_css_class(child, "art-choice-selected");
            } else {
                gtk_widget_remove_css_class(child, "art-choice-selected");
            }
        }
        update_art_buttons_recursive(child, variants, selected_art);
    }
}

static void update_art_menu_highlight(GtkWidget *row_box, int selected_art) {
    GtkWidget *menu_button = (GtkWidget *)g_object_get_data(G_OBJECT(row_box), "art_menu_button");
    if (!menu_button || !GTK_IS_MENU_BUTTON(menu_button)) return;
    GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(menu_button)));
    if (!popover) return;
    GtkWidget *child = gtk_popover_get_child(GTK_POPOVER(popover));
    if (!child) return;
    GArray *variants = (GArray *)g_object_get_data(G_OBJECT(row_box), "art_variants");
    update_art_buttons_recursive(child, variants, selected_art);
}

static void select_search_row_art(GtkWidget *row_box, int art_index) {
    if (!row_box) return;
    GArray *variants = (GArray *)g_object_get_data(G_OBJECT(row_box), "art_variants");
    if (!variants || art_index < 0 || art_index >= (int)variants->len) return;

    int base_id = g_array_index(variants, int, 0);
    int art_id = g_array_index(variants, int, art_index);
    if (base_id <= 0 || art_id <= 0) return;

    GtkWidget *list_row = gtk_widget_get_ancestor(row_box, GTK_TYPE_LIST_BOX_ROW);
    if (!list_row) return;
    CardPreview *pv = (CardPreview *)g_object_get_data(G_OBJECT(list_row), "preview");
    if (pv) pv->id = art_id;

    alt_art_set_selected(base_id, art_id);
    update_art_menu_highlight(row_box, art_id);

    SearchUI *ui = (SearchUI *)g_object_get_data(G_OBJECT(row_box), "search_ui");
    GtkStack *stack = NULL;
    GtkWidget *picture = NULL;
    if (!get_search_row_picture(row_box, &stack, &picture)) return;

    g_object_set_data(G_OBJECT(picture), "art_expected_id", GINT_TO_POINTER(art_id));
    slot_set_pixbuf(picture, NULL);
    g_object_set_data(G_OBJECT(picture), "img_id", NULL);
    g_object_set_data(G_OBJECT(picture), "pending_img_id", NULL);
    gtk_stack_set_visible_child_name(stack, "placeholder");
    if (ui) load_search_thumb(ui, picture, stack, art_id, pv && pv->is_prerelease);
}

static void on_art_choice_clicked(GtkButton *button, gpointer user_data) {
    (void)user_data;
    GtkWidget *row_box = (GtkWidget *)g_object_get_data(G_OBJECT(button), "art_row");
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "art_index")) - 1;
    select_search_row_art(row_box, index);

    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
}

static void load_art_choice_thumb(SearchUI *ui, GtkWidget *picture, GtkStack *stack, int art_id) {
    if (!picture || art_id <= 0) return;
    g_object_set_data(G_OBJECT(picture), "art_expected_id", GINT_TO_POINTER(art_id));
    if (ui) load_search_thumb(ui, picture, stack, art_id, FALSE);
}

static void on_art_choice_map(GtkWidget *stack, gpointer user_data) {
    (void)user_data;
    if (!stack || !GTK_IS_STACK(stack)) return;
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(stack), "art_thumb_loaded"))) return;
    g_object_set_data(G_OBJECT(stack), "art_thumb_loaded", GINT_TO_POINTER(1));

    SearchUI *ui = (SearchUI *)g_object_get_data(G_OBJECT(stack), "art_choice_ui");
    int art_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(stack), "art_choice_id"));
    GtkWidget *picture = gtk_stack_get_child_by_name(GTK_STACK(stack), "picture");
    if (picture && GTK_IS_DRAWING_AREA(picture)) {
        load_art_choice_thumb(ui, picture, GTK_STACK(stack), art_id);
    }
}

static GtkWidget *create_art_menu_button(GtkWidget *row_box, GArray *variants, int selected_art) {
    if (!variants || variants->len <= 1) return NULL;

    SearchUI *ui = (SearchUI *)g_object_get_data(G_OBJECT(row_box), "search_ui");

    GtkWidget *button = gtk_menu_button_new();
    ui_language_bind_text(G_OBJECT(button), "label", "异画", "Artwork");
    gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(button), FALSE);
    ui_language_bind_text(G_OBJECT(button), "tooltip-text", "切换异画", "Choose Artwork");
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(button, "flat");
    gtk_widget_add_css_class(button, "art-menu-button");

    GtkWidget *popover = gtk_popover_new();
    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    int rows = (variants->len + 2) / 3;
    int height = MIN(rows * 114 + 12, 420);
    gtk_widget_set_size_request(scroller, 280, height);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_widget_set_margin_start(grid, 6);
    gtk_widget_set_margin_end(grid, 6);
    gtk_widget_set_margin_top(grid, 6);
    gtk_widget_set_margin_bottom(grid, 6);

    for (guint i = 0; i < variants->len; i++) {
        int art_id = g_array_index(variants, int, i);

        GtkWidget *item = gtk_button_new();
        gtk_widget_add_css_class(item, "flat");
        gtk_widget_add_css_class(item, "art-choice");
        if (art_id == selected_art) {
            gtk_widget_add_css_class(item, "art-choice-selected");
        }
        g_autofree char *chinese_tip = i == 0 ? g_strdup_printf("原画 (%d)", art_id)
                                               : g_strdup_printf("异画 %u (%d)", i, art_id);
        g_autofree char *english_tip = i == 0 ? g_strdup_printf("Original Artwork (%d)", art_id)
                                               : g_strdup_printf("Alternate Artwork %u (%d)", i, art_id);
        ui_language_bind_text(G_OBJECT(item), "tooltip-text", chinese_tip, english_tip);

        GtkWidget *stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
        gtk_stack_set_transition_duration(GTK_STACK(stack), 120);
        gtk_widget_set_size_request(stack, 68, 99);

        GtkWidget *placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_size_request(placeholder, 68, 99);
        gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
        gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
        GtkWidget *spinner = gtk_spinner_new();
        gtk_widget_set_visible(spinner, FALSE);
        g_object_set_data(G_OBJECT(stack), "image-spinner", spinner);
        gtk_box_append(GTK_BOX(placeholder), spinner);
        gtk_stack_add_named(GTK_STACK(stack), placeholder, "placeholder");

        GtkWidget *picture = gtk_drawing_area_new();
        gtk_widget_set_size_request(picture, 68, 99);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(picture), draw_pixbuf_scaled, NULL, NULL);
        g_signal_connect(picture, "destroy", G_CALLBACK(on_drawing_area_destroy), NULL);
        gtk_stack_add_named(GTK_STACK(stack), picture, "picture");
        gtk_stack_set_visible_child_name(GTK_STACK(stack), "placeholder");
        gtk_button_set_child(GTK_BUTTON(item), stack);

        // 缩略图延迟到弹出菜单真正显示时再加载，避免搜索结果里每张异画卡都预下载一堆图。
        // 注意挂在 stack 上：初始显示的是 placeholder 页，picture 页尚未 map，
        // 若把 map 信号挂在 picture 上，缩略图永远不会开始加载。
        g_object_set_data(G_OBJECT(stack), "art_choice_ui", ui);
        g_object_set_data(G_OBJECT(stack), "art_choice_id", GINT_TO_POINTER(art_id));
        g_signal_connect(stack, "map", G_CALLBACK(on_art_choice_map), NULL);

        g_object_set_data(G_OBJECT(item), "art_row", row_box);
        // +1 避免索引 0 被 g_object_set_data 当作 NULL 删除
        g_object_set_data(G_OBJECT(item), "art_index", GINT_TO_POINTER((int)i + 1));
        g_signal_connect(item, "clicked", G_CALLBACK(on_art_choice_clicked), NULL);

        gtk_grid_attach(GTK_GRID(grid), item, (int)(i % 3), (int)(i / 3), 1, 1);
    }

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), grid);
    gtk_popover_set_child(GTK_POPOVER(popover), scroller);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(button), popover);
    g_object_set_data(G_OBJECT(row_box), "art_menu_button", button);
    return button;
}

static gboolean load_visible_search_thumbs(gpointer data) {
    SearchUI *ui = data;
    GtkWidget *list = ui->list;
    ui->search_image_source = 0;

    double page_height = gtk_adjustment_get_page_size(ui->search_vadjustment);
    if (page_height <= 0) return G_SOURCE_REMOVE;

    // Prefetch one row on each side of the viewport.
    double scroll_y = gtk_adjustment_get_value(ui->search_vadjustment) -
                      gtk_widget_get_margin_top(list);
    double first_y = MAX(0, scroll_y - SLOT_THUMB_H);
    double last_y = scroll_y + page_height + SLOT_THUMB_H;
    GtkWidget *row = GTK_WIDGET(gtk_list_box_get_row_at_y(GTK_LIST_BOX(list), first_y));
    for (; row; row = gtk_widget_get_next_sibling(row)) {
        graphene_rect_t bounds;
        if (gtk_widget_get_height(row) <= 0 || !gtk_widget_compute_bounds(row, list, &bounds))
            break;
        if (bounds.origin.y >= last_y) break;

        GtkStack *stack;
        GtkWidget *picture;
        GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
        if (!get_search_row_picture(box, &stack, &picture)) continue;
        int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(picture), "pending_img_id"));
        if (img_id <= 0) continue;

        g_object_set_data(G_OBJECT(picture), "pending_img_id", NULL);
        const CardPreview *card = g_object_get_data(G_OBJECT(row), "preview");
        load_search_thumb(ui, picture, stack, img_id, card && card->is_prerelease);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_search_thumbs(GtkAdjustment *adjustment, gpointer data) {
    (void)adjustment;
    SearchUI *ui = data;
    if (ui->search_vadjustment && !ui->search_image_source)
        ui->search_image_source = g_idle_add(load_visible_search_thumbs, ui);
}

static gboolean schedule_search_thumbs_after_frame(GtkWidget *list, GdkFrameClock *clock,
                                                   gpointer data) {
    (void)list;
    (void)clock;
    SearchUI *ui = data;
    ui->search_image_tick = 0;
    schedule_search_thumbs(NULL, ui);
    return G_SOURCE_REMOVE;
}

static void on_search_list_destroy(GtkWidget *list, gpointer data) {
    SearchUI *ui = data;
    if (ui->search_image_tick) {
        gtk_widget_remove_tick_callback(list, ui->search_image_tick);
        ui->search_image_tick = 0;
    }
    g_signal_handlers_disconnect_by_func(ui->search_vadjustment, schedule_search_thumbs, ui);
    g_clear_handle_id(&ui->search_image_source, g_source_remove);
    g_clear_object(&ui->search_vadjustment);
    g_clear_handle_id(&ui->batch_render_id, g_source_remove);
    g_clear_pointer(&ui->pending_results, g_ptr_array_unref);
    ui->list = NULL;
}

void search_ui_watch_viewport(SearchUI *ui, GtkAdjustment *adjustment) {
    ui->search_vadjustment = g_object_ref(adjustment);
    g_signal_connect(adjustment, "changed", G_CALLBACK(schedule_search_thumbs), ui);
    g_signal_connect(adjustment, "value-changed", G_CALLBACK(schedule_search_thumbs), ui);
    g_signal_connect(ui->list, "destroy", G_CALLBACK(on_search_list_destroy), ui);
}

// 批量渲染回调：每次处理一批结果（10个）
gboolean batch_render_results(gpointer user_data) {
    SearchUI *ui = (SearchUI*)user_data;
    
    if (!ui || !ui->pending_results ||
        ui->pending_results_pos >= ui->pending_results->len) {
        if (ui) {
            ui->batch_render_id = 0;
        }
        return G_SOURCE_REMOVE;
    }
    
    // 控制单次 idle 的工作量，避免大量结果持续占住主线程。
    const int BATCH_SIZE = 20;
    const gint64 deadline_us = g_get_monotonic_time() + 8000;
    int processed = 0;
    
    while (processed < BATCH_SIZE &&
           ui->pending_results &&
           ui->pending_results_pos < ui->pending_results->len) {
        JsonObject *obj = g_ptr_array_index(ui->pending_results, ui->pending_results_pos++);
        
        if (obj) {
            add_result_row_immediate(ui, obj);
        }
        processed++;

        if (processed >= 4 && g_get_monotonic_time() >= deadline_us) {
            break;
        }
    }
    
    if (ui->search_vadjustment && !ui->search_image_tick)
        ui->search_image_tick = gtk_widget_add_tick_callback(ui->list,
            schedule_search_thumbs_after_frame, ui, NULL);
    if (!ui->pending_results || ui->pending_results_pos >= ui->pending_results->len) {
        g_clear_pointer(&ui->pending_results, g_ptr_array_unref);
        ui->pending_results_pos = 0;
        ui->batch_render_id = 0;
        return G_SOURCE_REMOVE;
    }
    
    return G_SOURCE_CONTINUE;
}

static void on_alt_art_ready(gpointer user_data) {
    SearchUI *ui = (SearchUI *)user_data;
    if (!ui || !ui->list || !ui->entry || !ui->button) return;
    // 用户还没有搜索结果时不打扰；有结果则原地刷新，补上异画按钮
    if (!gtk_widget_get_first_child(ui->list)) return;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(ui->entry));
    if ((!q || *q == '\0') && !has_active_filter()) return;
    if (ui->toast_overlay) {
        AdwToast *toast = adw_toast_new(TR("异画数据已就绪，已刷新搜索结果", "Alternate artwork data loaded and search results refreshed"));
        adw_toast_set_timeout(toast, 3);
        adw_toast_overlay_add_toast(ui->toast_overlay, toast);
    }
    on_search_clicked(GTK_BUTTON(ui->button), ui);
}

void search_ui_watch_alt_art(SearchUI *ui) {
    if (!ui) return;
    alt_art_set_ready_callback(on_alt_art_ready, ui);
}

// 将结果加入渲染队列（延迟渲染）
void queue_result_for_render(SearchUI *ui, JsonObject *obj) {
    if (!ui || !ui->list) return;
    
    // 初始化队列
    if (!ui->pending_results) {
        ui->pending_results = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
        ui->pending_results_pos = 0;
    }
    
    // 增加引用计数并加入队列
    g_ptr_array_add(ui->pending_results, json_object_ref(obj));
    
    // 启动批量渲染定时器
    if (ui->batch_render_id == 0) {
        ui->batch_render_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, batch_render_results, ui, NULL);
    }
}

typedef struct {
    GtkLabel *title;
    GtkLabel *subtitle;
    GtkLabel *status;
    GtkWidget *status_box;
    gboolean is_change;
    char *raw_status;
    gboolean points;
    gboolean show_status_tooltip;
} ResultLanguageLabels;

static void free_result_language_labels(gpointer data) {
    ResultLanguageLabels *labels = data;
    g_free(labels->raw_status);
    g_free(labels);
}

static void refresh_result_row_language(GtkWidget *row) {
    ResultLanguageLabels *labels = g_object_get_data(G_OBJECT(row), "result-language");
    const CardPreview *card = g_object_get_data(G_OBJECT(row), "preview");
    if (!labels || !card)
        return;

    const char *name = card_name_for_language(card->original_name, card->en_name);
    gtk_label_set_text(labels->title, name ? name : TR("(无名)", "(Unnamed)"));
    g_autofree char *metadata = card_display_metadata(card->types);
    gtk_label_set_text(labels->subtitle, metadata ? metadata : "");
    gtk_label_set_wrap(labels->subtitle, ui_is_english());
    gtk_label_set_lines(labels->subtitle, ui_is_english() ? 3 : -1);

    if (labels->status && labels->raw_status) {
        g_autofree char *status = labels->points
            ? g_strdup_printf(TR("%s分", "%s pts"), labels->raw_status)
            : card_display_metadata(labels->raw_status);
        g_autofree char *markup = g_markup_printf_escaped("<b>[%s]</b>", status);
        gtk_label_set_markup(labels->status, markup);
        if (labels->show_status_tooltip)
            gtk_widget_set_tooltip_text(GTK_WIDGET(labels->status), status);
    }
}

static void refresh_result_rule(SearchUI *ui, GtkWidget *row) {
    ResultLanguageLabels *labels = g_object_get_data(G_OBJECT(row), "result-language");
    const CardPreview *card = g_object_get_data(G_OBJECT(row), "preview");
    if (!labels || !card || labels->is_change || !ui->forbidden_dropdown) return;

    const char *status = NULL;
    const char *css = NULL;
    labels->points = FALSE;
    if (gtk_drop_down_get_selected(ui->forbidden_dropdown) == 4) {
        if (get_card_limit(ui, card->cid, card->type) == 0) {
            status = "禁止";
            css = "error";
        } else if (card->en_name && ui->genesys_forbidden) {
            status = g_hash_table_lookup(ui->genesys_forbidden, card->en_name);
            labels->points = status && *status;
            css = "accent";
        }
    } else {
        switch (get_card_limit(ui, card->cid, card->type)) {
        case 0: status = "禁止"; css = "error"; break;
        case 1: status = "限制"; css = "warning"; break;
        case 2: status = "准限制"; css = "accent"; break;
        }
    }

    g_free(labels->raw_status);
    labels->raw_status = g_strdup(status);
    GtkWidget *label = GTK_WIDGET(labels->status);
    gtk_widget_remove_css_class(label, "error");
    gtk_widget_remove_css_class(label, "warning");
    gtk_widget_remove_css_class(label, "accent");
    if (css) gtk_widget_add_css_class(label, css);
    gtk_widget_set_visible(label, status && *status);

    GtkStack *stack;
    GtkWidget *picture;
    GtkWidget *box = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    if (get_search_row_picture(box, &stack, &picture))
        apply_overlay_to_widget(picture, ui);

    gboolean visible = FALSE;
    for (GtkWidget *child = gtk_widget_get_first_child(labels->status_box); child;
         child = gtk_widget_get_next_sibling(child))
        visible |= gtk_widget_get_visible(child);
    gtk_widget_set_visible(labels->status_box, visible);
}

void refresh_search_result_rules(SearchUI *ui) {
    if (!ui->list) return;
    for (GtkWidget *row = gtk_widget_get_first_child(ui->list); row;
         row = gtk_widget_get_next_sibling(row)) {
        refresh_result_rule(ui, row);
        refresh_result_row_language(row);
    }
}

void refresh_search_result_language(SearchUI *ui) {
    for (GtkWidget *row = gtk_widget_get_first_child(ui->list); row;
         row = gtk_widget_get_next_sibling(row)) {
        refresh_result_row_language(row);
    }
}

// 立即渲染单个结果行（原 add_result_row 函数）
void add_result_row_immediate(SearchUI *ui, JsonObject *obj) {
    const char *english_name = card_english_name(obj);
    
    int id = 0;
    if (json_object_has_member(obj, "id")) id = json_object_get_int_member(obj, "id");
    
    // 获取cid用于禁限卡表查询
    int cid = 0;
    if (json_object_has_member(obj, "cid")) {
        cid = json_object_get_int_member(obj, "cid");
    }

    // 异画：id 是本体密码，artid 是用户直接搜索异画密码时 ygocdb 返回的版本。
    // 默认版本优先取用户上次选择，其次取 artid，最后回退本体。
    int artid = 0;
    if (json_object_has_member(obj, "artid")) {
        artid = json_object_get_int_member(obj, "artid");
    }
    GArray *art_variants = NULL;
    int selected_art = id;
    if (id > 0) {
        art_variants = alt_art_get_variants(id);
        if (artid > 0) {
            // 用户直接搜索异画密码：即使本地还没有异画列表，也按 artid 显示/加入
            selected_art = artid;
        } else if (art_variants && art_variants->len > 1) {
            int remembered = alt_art_get_selected(id);
            if (art_variants_contains(art_variants, remembered)) {
                selected_art = remembered;
            }
        }
        if (!art_variants || art_variants->len <= 1) {
            if (art_variants) g_array_unref(art_variants);
            art_variants = NULL;
        }
    }

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(row, 6);
    gtk_widget_set_margin_bottom(row, 6);
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);

    // 缩略图栈：占位与图片（强制 68x99 尺寸，避免单条结果时放大）
    GtkWidget *thumb_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(thumb_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(thumb_stack), 150);
    gtk_widget_set_size_request(thumb_stack, 68, 99);
    gtk_widget_set_halign(thumb_stack, GTK_ALIGN_START);
    gtk_widget_set_valign(thumb_stack, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(thumb_stack, FALSE);
    gtk_widget_set_vexpand(thumb_stack, FALSE);
    gtk_widget_add_css_class(thumb_stack, "thumb-fixed");
    // 占位框 68x99，居中Spinner
    GtkWidget *placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(placeholder, 68, 99);
    gtk_widget_set_valign(placeholder, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(placeholder, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(placeholder, "thumb-fixed");
    GtkWidget *spinner = gtk_spinner_new();
    gtk_widget_set_visible(spinner, FALSE);
    g_object_set_data(G_OBJECT(thumb_stack), "image-spinner", spinner);
    gtk_box_append(GTK_BOX(placeholder), spinner);
    gtk_stack_add_named(GTK_STACK(thumb_stack), placeholder, "placeholder");
    // 图片控件：使用 GtkDrawingArea + Cairo 高质量缩放绘制
    GtkWidget *picture = gtk_drawing_area_new();
    gtk_widget_set_size_request(picture, 68, 99);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(picture), draw_pixbuf_scaled, NULL, NULL);
    g_signal_connect(picture, "destroy", G_CALLBACK(on_drawing_area_destroy), NULL);
    gtk_widget_set_halign(picture, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(picture, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(picture, FALSE);
    gtk_widget_set_vexpand(picture, FALSE);
    gtk_widget_add_css_class(picture, "thumb-fixed");
    gtk_stack_add_named(GTK_STACK(thumb_stack), picture, "picture");
    gtk_stack_set_visible_child_name(GTK_STACK(thumb_stack), "placeholder");
    // 将 cid 和英文卡名存储到图片控件，以便设置禁限角标
    g_object_set_data(G_OBJECT(picture), "card_id", GINT_TO_POINTER(cid));
    // 当前应显示的版本；异步图片回调据此丢弃旧异画
    g_object_set_data(G_OBJECT(picture), "art_expected_id", GINT_TO_POINTER(selected_art));
    slot_set_en_name(picture, english_name);
    // 检查是否是先行卡（优先通过ID位数判断，9位数=先行卡）
    gboolean is_prerelease = (id > 0 && is_prerelease_id(id));
    
    // 如果不是9位数，检查JSON中的标记（向后兼容）
    if (!is_prerelease && json_object_has_member(obj, "is_prerelease")) {
        is_prerelease = json_object_get_boolean_member(obj, "is_prerelease");
    }
    
    if (selected_art > 0)
        g_object_set_data(G_OBJECT(picture), "pending_img_id", GINT_TO_POINTER(selected_art));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_hexpand(vbox, TRUE);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 20);
    gtk_widget_add_css_class(title, "heading");

    GtkWidget *subtitle = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(subtitle), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(subtitle), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(subtitle), 20);
    gtk_label_set_wrap_mode(GTK_LABEL(subtitle), PANGO_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(subtitle, "dim-label");

    gtk_box_append(GTK_BOX(vbox), title);
    gtk_box_append(GTK_BOX(vbox), subtitle);
    ResultLanguageLabels *labels = g_new0(ResultLanguageLabels, 1);
    labels->title = GTK_LABEL(title);
    labels->subtitle = GTK_LABEL(subtitle);

    // 禁限状态和“异画”按钮放同一排：禁限在左，异画在右。
    // 没有异画时这一排只有禁限，保证所有卡片的禁限状态纵向对齐。
    GtkWidget *status_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(status_box, GTK_ALIGN_START);
    gtk_widget_set_valign(status_box, GTK_ALIGN_CENTER);

    // 有异画时创建菜单按钮，稍后与禁限状态一起加入 status_box
    GtkWidget *art_button = NULL;
    if (art_variants && art_variants->len > 1) {
        g_object_set_data(G_OBJECT(row), "search_ui", ui);
        g_object_set_data_full(G_OBJECT(row), "art_variants", art_variants,
                               (GDestroyNotify)g_array_unref);
        art_button = create_art_menu_button(row, art_variants, selected_art);
        art_variants = NULL;
    }
    
    GtkWidget *status_label = gtk_label_new(NULL);
    labels->status = GTK_LABEL(status_label);
    labels->status_box = status_box;
    gtk_label_set_xalign(labels->status, 0);
    gtk_label_set_ellipsize(labels->status, PANGO_ELLIPSIZE_END);
    gtk_widget_set_visible(status_label, FALSE);
    gtk_box_append(GTK_BOX(status_box), status_label);

    if (json_object_has_member(obj, "forbidden_change")) {
        const char *change = json_object_get_string_member(obj, "forbidden_change");
        if (change && *change) {
            labels->is_change = TRUE;
            labels->raw_status = g_strdup(change);
            labels->show_status_tooltip = TRUE;
            const char *arrow = strstr(change, "→");
            if (arrow) {
                const char *next = arrow + strlen("→");
                if (g_str_has_prefix(next, "禁止")) gtk_widget_add_css_class(status_label, "error");
                else if (g_str_has_prefix(next, "限制")) gtk_widget_add_css_class(status_label, "warning");
                else if (g_str_has_prefix(next, "准限制")) gtk_widget_add_css_class(status_label, "accent");
            }
            gtk_widget_set_visible(status_label, TRUE);
        }
    }

    if (art_button) {
        gtk_box_append(GTK_BOX(status_box), art_button);
    }
    gtk_widget_set_visible(status_box, labels->is_change || art_button != NULL);
    gtk_box_append(GTK_BOX(vbox), status_box);
    if (labels->is_change) apply_overlay_to_widget(picture, ui);

    gtk_box_append(GTK_BOX(row), thumb_stack);
    gtk_box_append(GTK_BOX(row), vbox);

    GtkWidget *list_row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(list_row), row);
    // 绑定悬停事件
    GtkEventController *motion = GTK_EVENT_CONTROLLER(gtk_event_controller_motion_new());
    g_signal_connect(motion, "enter", G_CALLBACK(on_result_row_enter), ui);
    gtk_widget_add_controller(list_row, motion);
    // 绑定点击事件（与中栏一致：pressed 记录、released 判定点击）
    GtkGesture *click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 0);  // 0 表示接受所有按钮（左键、右键等）
    g_signal_connect(click, "pressed", G_CALLBACK(on_result_row_pressed), ui);
    g_signal_connect(click, "released", G_CALLBACK(on_result_row_released), ui);
    gtk_widget_add_controller(list_row, GTK_EVENT_CONTROLLER(click));
    // 存储预览数据
    CardPreview *pv = g_new0(CardPreview, 1);
    pv->id = selected_art;
    pv->cid = cid;
    pv->original_name = g_strdup(card_original_name(obj));
    pv->sheet_name = g_strdup(card_deck_sheet_name(obj));
    // 英文卡名（GENESYS 分值查找用）
    pv->en_name = g_strdup(english_name);
    pv->is_prerelease = is_prerelease;  // 设置是否是先行卡
    
    // 获取类型和其他 data 字段：先行卡的字段在顶层，普通卡的在 data 对象中
    if (is_prerelease) {
        // 先行卡的数据字段直接在顶层
        pv->type = json_object_has_member(obj, "type") ? (uint32_t)json_object_get_int_member(obj, "type") : 0;
        pv->ot = json_object_has_member(obj, "ot") ? json_object_get_int_member(obj, "ot") : -1;
        pv->setcode = json_object_has_member(obj, "setcode") ? json_object_get_int_member(obj, "setcode") : -1;
        pv->atk = json_object_has_member(obj, "atk") ? json_object_get_int_member(obj, "atk") : -1;
        pv->def = json_object_has_member(obj, "def") ? json_object_get_int_member(obj, "def") : -1;
        pv->level = json_object_has_member(obj, "level") ? json_object_get_int_member(obj, "level") : -1;
        pv->race = json_object_has_member(obj, "race") ? json_object_get_int_member(obj, "race") : -1;
        pv->attribute = json_object_has_member(obj, "attribute") ? json_object_get_int_member(obj, "attribute") : -1;
    } else if (json_object_has_member(obj, "data")) {
        // 普通卡的数据字段在 data 对象中
        JsonObject *data = json_object_get_object_member(obj, "data");
        if (data) {
            // 提取所有 data 字段
            if (json_object_has_member(data, "type")) {
                pv->type = (uint32_t)json_object_get_int_member(data, "type");
            }
            pv->ot = json_object_has_member(data, "ot") ? json_object_get_int_member(data, "ot") : -1;
            pv->setcode = json_object_has_member(data, "setcode") ? json_object_get_int_member(data, "setcode") : -1;
            pv->atk = json_object_has_member(data, "atk") ? json_object_get_int_member(data, "atk") : -1;
            pv->def = json_object_has_member(data, "def") ? json_object_get_int_member(data, "def") : -1;
            pv->level = json_object_has_member(data, "level") ? json_object_get_int_member(data, "level") : -1;
            pv->race = json_object_has_member(data, "race") ? json_object_get_int_member(data, "race") : -1;
            pv->attribute = json_object_has_member(data, "attribute") ? json_object_get_int_member(data, "attribute") : -1;
        }
    }
    
    if (json_object_has_member(obj, "text")) {
        JsonObject *text = json_object_get_object_member(obj, "text");
        if (text) {
            if (json_object_has_member(text, "types")) pv->types = g_strdup(json_object_get_string_member(text, "types"));
            if (json_object_has_member(text, "pdesc")) pv->pdesc = g_strdup(json_object_get_string_member(text, "pdesc"));
            if (json_object_has_member(text, "desc"))  pv->desc  = g_strdup(json_object_get_string_member(text, "desc"));
        }
    }
    g_object_set_data_full(G_OBJECT(list_row), "preview", pv, free_card_preview);
    g_object_set_data_full(G_OBJECT(list_row), "result-language", labels, free_result_language_labels);
    refresh_result_rule(ui, list_row);
    refresh_result_row_language(list_row);
    // 右栏行支持拖拽到中栏：提供字符串 payload "search:<id>:<isExtra>"
    GtkDragSource *ds = gtk_drag_source_new();
    // 与中栏一致，采用 MOVE 动作，确保目标接受
    gtk_drag_source_set_actions(ds, GDK_ACTION_MOVE);
    g_signal_connect(ds, "prepare", G_CALLBACK(on_drag_prepare), NULL);
    g_signal_connect(ds, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
    // 在行上存储一个标记供 prepare 使用
    g_object_set_data(G_OBJECT(list_row), "drag_kind", (gpointer)"search_row");
    // 为拖拽预览使用行内缩略图的 pixbuf（若可用）
    gtk_widget_add_controller(list_row, GTK_EVENT_CONTROLLER(ds));
    gtk_list_box_append(GTK_LIST_BOX(ui->list), list_row);
}

void search_response_cb(GObject *source, GAsyncResult *res, gpointer user_data) {
    SoupSession *session = SOUP_SESSION(source);
    SearchRequestCtx *request = (SearchRequestCtx*)user_data;
    SearchUI *ui = request ? request->ui : NULL;
    GError *err = NULL;
    GBytes *response_body = soup_session_send_and_read_finish(session, res, &err);
    
    // 如果请求被取消（例如新搜索开始），直接返回
    if (err && g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_error_free(err);
        g_free(request);
        return;
    }
    
    if (!response_body) {
        if (err) g_error_free(err); 
        g_free(request);
        return; 
    }

    if (!ui || !request || request->generation != ui->search_generation) {
        if (err) g_error_free(err);
        g_bytes_unref(response_body);
        g_free(request);
        return;
    }

    gsize body_size = 0;
    const guint8 *body_data = g_bytes_get_data(response_body, &body_size);

    JsonParser *parser = json_parser_new();
    GError *jerr = NULL;
    if (body_data && body_size > 0 && json_parser_load_from_data(parser, (const char*)body_data, (gssize)body_size, &jerr)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_TYPE(root) == JSON_NODE_OBJECT) {
            JsonObject *robj = json_node_get_object(root);
            if (json_object_has_member(robj, "result")) {
                JsonArray *arr = json_object_get_array_member(robj, "result");
                guint nitems = json_array_get_length(arr);
                guint queued = 0;
                for (guint i = 0; i < nitems && queued < SEARCH_MAX_RESULTS; ++i) {
                    JsonObject *item = json_array_get_object_element(arr, i);
                    if (item) {
                        queue_result_for_render(ui, item);
                        queued++;
                    }
                }
                if (nitems > SEARCH_MAX_RESULTS && ui && ui->toast_overlay) {
                    AdwToast *toast = adw_toast_new(TR("搜索结果过多，已限制为 500 条。请缩小搜索范围。", "Showing up to 500 results. Narrow your search for more specific results."));
                    adw_toast_set_timeout(toast, 3);
                    adw_toast_overlay_add_toast(ui->toast_overlay, toast);
                }
            }
        }
    }
    if (jerr) g_error_free(jerr);
    g_object_unref(parser);
    g_bytes_unref(response_body);
    g_free(request);
}

void on_forbidden_dropdown_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)dropdown;
    (void)pspec;
    SearchUI *ui = (SearchUI*)user_data;
    
    // 更新 GENESYS 总分显示
    update_genesys_score(ui);
    // 更新卡组槽位的限制角标
    update_deck_overlays(ui);

    refresh_search_result_rules(ui);
}

// 过滤选项回调函数
void on_monster_filter_toggled(GtkCheckButton *btn, gpointer user_data) {
    SearchUI *ui = (SearchUI*)user_data;
    ui->filter_by_monster = gtk_check_button_get_active(btn);
}

void on_spell_filter_toggled(GtkCheckButton *btn, gpointer user_data) {
    SearchUI *ui = (SearchUI*)user_data;
    ui->filter_by_spell = gtk_check_button_get_active(btn);
}

void on_trap_filter_toggled(GtkCheckButton *btn, gpointer user_data) {
    SearchUI *ui = (SearchUI*)user_data;
    ui->filter_by_trap = gtk_check_button_get_active(btn);
}

// 离线搜索后台任务：cards.json 全量扫描与匹配在工作线程执行，
// 完成后回到主线程渲染，避免上万张卡的遍历阻塞 UI。
typedef struct {
    guint64 generation;          // 派发时的搜索代次，过期结果直接丢弃
    guint64 result_count_before; // 派发前已入队的结果数（先行卡部分）
    guint remaining;             // 本次扫描最多接受的结果数
    gchar *query;                // strdup 的查询词（原指针指向 entry 内部文本）
    gboolean search_all;
    FilterState *filter;         // 主线程筛选状态快照（字符串深拷贝）
    GPtrArray *results;          // 匹配到的 JsonObject*（引用计数持有，跨线程安全）
} OfflineSearchTask;

// 复制筛选状态：工作线程扫描期间主线程可能修改或释放原状态
static FilterState *filter_state_snapshot(const FilterState *src) {
    if (!src) return NULL;
    FilterState *snap = g_new0(FilterState, 1);
    *snap = *src;
    snap->atk_text = g_strdup(src->atk_text);
    snap->def_text = g_strdup(src->def_text);
    snap->level_text = g_strdup(src->level_text);
    snap->left_scale_text = g_strdup(src->left_scale_text);
    snap->right_scale_text = g_strdup(src->right_scale_text);
    snap->field_text = g_strdup(src->field_text);
    return snap;
}

static void offline_search_task_free(OfflineSearchTask *t) {
    if (!t) return;
    g_free(t->query);
    if (t->filter) {
        g_free(t->filter->atk_text);
        g_free(t->filter->def_text);
        g_free(t->filter->level_text);
        g_free(t->filter->left_scale_text);
        g_free(t->filter->right_scale_text);
        g_free(t->filter->field_text);
        g_free(t->filter);
    }
    if (t->results) g_ptr_array_unref(t->results);
    g_free(t);
}

// 工作线程收集回调：只做过滤与引用计数，禁止触碰任何 UI
static gboolean offline_task_collect_cb(JsonObject *item, gpointer user_data) {
    OfflineSearchTask *t = (OfflineSearchTask*)user_data;
    if (!item || !t || !t->filter) return FALSE;
    if (!apply_filter(item, t->filter)) return FALSE;
    g_ptr_array_add(t->results, json_object_ref(item));
    return TRUE;
}

static void offline_search_thread(GTask *task, gpointer source_object, gpointer task_data, GCancellable *cancellable) {
    (void)source_object;
    (void)cancellable;
    OfflineSearchTask *t = (OfflineSearchTask*)task_data;
    if (t) {
        offline_foreach_card(t->query, t->search_all, offline_task_collect_cb, t, t->remaining);
    }
    g_task_return_boolean(task, TRUE);
}

static void offline_search_finished(GObject *source, GAsyncResult *res, gpointer user_data) {
    (void)source;
    SearchUI *ui = (SearchUI*)user_data;
    OfflineSearchTask *t = (OfflineSearchTask*)g_task_get_task_data(G_TASK(res));
    GError *err = NULL;
    g_task_propagate_boolean(G_TASK(res), &err);
    if (err) g_error_free(err);
    // t 由 task_data 的 destroy notify 释放，此处只使用

    // 过期搜索（派发后用户又触发了新搜索）：丢弃结果
    if (!ui || !t || t->generation != ui->search_generation) {
        return;
    }

    for (guint i = 0; i < t->results->len; i++) {
        JsonObject *item = (JsonObject*)g_ptr_array_index(t->results, i);
        queue_result_for_render(ui, item);
    }

    if (t->result_count_before + t->results->len >= SEARCH_MAX_RESULTS && ui->toast_overlay) {
        g_message("Reached maximum result limit (%u), stopping search", SEARCH_MAX_RESULTS);
        AdwToast *toast = adw_toast_new(TR("搜索结果过多，已限制为 500 条。请缩小搜索范围。", "Showing up to 500 results. Narrow your search for more specific results."));
        adw_toast_set_timeout(toast, 3);
        adw_toast_overlay_add_toast(ui->toast_overlay, toast);
    }
}

static gboolean parse_filter_number(const char *text, gboolean allow_unknown, gint64 *value) {
    if (allow_unknown && g_str_equal(text, "?")) {
        *value = UNKNOWN_CARD_STAT;
        return TRUE;
    }

    return g_ascii_string_to_signed(text, 10, 0, G_MAXINT, value, NULL);
}

static gboolean matches_filter_number(const char *text, gint64 card_value, gboolean allow_unknown) {
    gint64 required;
    return !text || !*text ||
           (parse_filter_number(text, allow_unknown, &required) && card_value == required);
}

static gboolean has_invalid_filter_number(const FilterState *filter) {
    if (filter->card_type_selected != 1) {
        return FALSE;
    }

    gboolean pendulum_selected = filter->monster_type_toggles[MONSTER_FILTER_PENDULUM];
    const char *numbers[] = {
        filter->atk_text, filter->def_text, filter->level_text,
        pendulum_selected ? filter->left_scale_text : NULL,
        pendulum_selected ? filter->right_scale_text : NULL,
    };
    for (guint i = 0; i < G_N_ELEMENTS(numbers); i++) {
        gint64 value;
        if (numbers[i] && *numbers[i] && !parse_filter_number(numbers[i], i < 2, &value)) {
            return TRUE;
        }
    }
    return FALSE;
}

void on_search_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SearchUI *ui = (SearchUI*)user_data;
    const char *q = gtk_editable_get_text(GTK_EDITABLE(ui->entry));
    
    const FilterState *filter = get_current_filter_state();
    gboolean filter_active = has_active_filter();
    gboolean has_query = q && *q;
    if (!has_query && !filter_active) {
        return;
    }

    gboolean offline_enabled = load_offline_data_switch_state() && offline_data_is_ready();
    if (filter_active && !offline_enabled) {
        if (ui->toast_overlay) {
            adw_toast_overlay_add_toast(ui->toast_overlay, adw_toast_new(TR("使用筛选前请启用并下载离线数据。", "Enable and download offline data to use filters.")));
        }
        return;
    }
    if (has_invalid_filter_number(filter)) {
        if (ui->toast_overlay) {
            adw_toast_overlay_add_toast(ui->toast_overlay, adw_toast_new(TR("数值筛选请输入非负整数。攻击力和守备力也支持 ?。", "Enter non-negative integers. ATK and DEF also accept ?.")));
        }
        return;
    }

    ui->search_generation++;
    const guint64 search_generation = ui->search_generation;

    // 停止批量渲染定时器
    if (ui->batch_render_id > 0) {
        g_source_remove(ui->batch_render_id);
        ui->batch_render_id = 0;
    }
    
    // 注意：移除 g_main_context_iteration 调用，因为它可能导致重入问题
    // 我们依赖于弱引用和 row_invalid 标记来防止访问已销毁的对象
    
    // 清理待渲染队列
    GPtrArray *old_pending = ui->pending_results;
    ui->pending_results = NULL;
    ui->pending_results_pos = 0;
    if (old_pending) {
        g_ptr_array_free(old_pending, TRUE);
    }
    
    // 在清空UI列表之前，标记所有现有行为"无效"
    // 这样即使有待处理的事件回调，它们也会检测到标记并跳过处理
    GtkWidget *list_child = gtk_widget_get_first_child(ui->list);
    while (list_child) {
        if (GTK_IS_LIST_BOX_ROW(list_child)) {
            g_object_set_data(G_OBJECT(list_child), "row_invalid", GINT_TO_POINTER(1));
        }
        list_child = gtk_widget_get_next_sibling(list_child);
    }
    
    // 最后清空UI列表（这会销毁widget并触发弱引用清理）
    list_clear(GTK_LIST_BOX(ui->list));

    gboolean search_all = !has_query;
    
    // 设置最大结果数限制，避免UI卡死
    guint result_count = 0;

    // 如果启用了先行卡显示，先添加先行卡搜索结果
    if (show_prerelease_cards && result_count < SEARCH_MAX_RESULTS) {
        JsonArray *prerelease_results = search_all ? get_all_prerelease_cards() : search_prerelease_cards(q);
        if (prerelease_results) {
            guint len = json_array_get_length(prerelease_results);
            for (guint i = 0; i < len && result_count < SEARCH_MAX_RESULTS; i++) {
                JsonObject *item = json_array_get_object_element(prerelease_results, i);
                if (item) {
                    // 应用筛选条件
                    if (apply_filter(item, filter)) {
                        // 添加一个标记表示这是先行卡。卡片对象来自共享解析缓存，
                        // 直接写入会永久污染缓存，故先深拷贝再标记。
                        // 注意必须用 json_node_deep_copy：json_node_copy 只是浅拷贝
                        // （对 object 仅增加引用计数），仍会写进共享缓存。
                        JsonNode *marked_node = json_node_deep_copy(json_array_get_element(prerelease_results, i));
                        JsonObject *marked_item = marked_node ? json_node_get_object(marked_node) : NULL;
                        if (marked_item) {
                            json_object_set_boolean_member(marked_item, "is_prerelease", TRUE);
                            queue_result_for_render(ui, marked_item);
                        }
                        if (marked_node) json_node_free(marked_node);
                        result_count++;
                    }
                }
            }
            json_array_unref(prerelease_results);
            
            // 如果先行卡就达到最大结果数，显示警告
            if (result_count >= SEARCH_MAX_RESULTS) {
                g_message("Reached maximum result limit (%u) with prerelease cards", SEARCH_MAX_RESULTS);
                if (ui->toast_overlay) {
                    AdwToast *toast = adw_toast_new(TR("搜索结果过多，已限制为 500 条。请缩小搜索范围。", "Showing up to 500 results. Narrow your search for more specific results."));
                    adw_toast_set_timeout(toast, 3);
                    adw_toast_overlay_add_toast(ui->toast_overlay, toast);
                }
            }
        }
    }

    if (offline_enabled && result_count < SEARCH_MAX_RESULTS) {
        // 离线数据搜索：全量扫描移入后台线程，完成后回主线程渲染（达到上限即停止）
        g_message("Searching in offline data...");

        OfflineSearchTask *t = g_new0(OfflineSearchTask, 1);
        t->generation = search_generation;
        t->result_count_before = result_count;
        t->remaining = (SEARCH_MAX_RESULTS > result_count) ? (SEARCH_MAX_RESULTS - result_count) : 0;
        t->query = g_strdup(q ? q : "");
        t->search_all = search_all;
        t->filter = filter_state_snapshot(filter);
        t->results = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);

        GTask *task = g_task_new(NULL, NULL, offline_search_finished, ui);
        g_task_set_task_data(task, t, (GDestroyNotify)offline_search_task_free);
        g_task_run_in_thread(task, offline_search_thread);
        g_object_unref(task);
    } else if (!search_all && result_count < SEARCH_MAX_RESULTS) {
        // 仅在有搜索关键词时进行在线API搜索
        // 如果搜索框为空但有筛选条件，不进行在线搜索
        char *escaped = g_uri_escape_string(q, NULL, TRUE);
        char *url = g_strdup_printf("https://ygocdb.com/api/v0/?search=%s", escaped);
        g_free(escaped);

        SoupMessage *msg = soup_message_new("GET", url);
        g_free(url);
        if (!msg) return;
        // 搜索请求不使用 cancellable，让请求自然完成
        // 回调中会检查是否应该处理结果
        SearchRequestCtx *request = g_new0(SearchRequestCtx, 1);
        request->ui = ui;
        request->generation = search_generation;
        soup_session_send_and_read_async(ui->session, msg, G_PRIORITY_DEFAULT, NULL, search_response_cb, request);
        g_object_unref(msg);  // soup_session_send_and_read_async 会内部增加引用
    }
}
static gint64 get_filter_card_int(JsonObject *stats, const char *field, gint64 fallback) {
    JsonNode *node = stats ? json_object_get_member(stats, field) : NULL;
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) {
        return fallback;
    }
    return json_node_get_int(node);
}

// 应用筛选条件到搜索结果
// 返回值：TRUE 表示卡片通过筛选，FALSE 表示应该被过滤掉
gboolean apply_filter(JsonObject *card, const FilterState *filter_state) {
    if (!card || !filter_state) {
        return TRUE;
    }
    
    // 先行卡的数值在卡片对象上，离线卡的数值在data对象中。
    JsonObject *stats = card;
    if (!json_object_has_member(card, "type")) {
        JsonNode *data = json_object_get_member(card, "data");
        stats = data && JSON_NODE_HOLDS_OBJECT(data) ? json_node_get_object(data) : NULL;
    }

    // 首先检查字段筛选（不依赖卡片类型选择）
    if (filter_state->field_text && filter_state->field_text[0] != '\0') {
        // 获取卡片的setcode字段
        gint64 card_setcode = get_filter_card_int(stats, "setcode", 0);
        
        // 使用card_info中的函数检查字段匹配
        if (!match_setcode_with_field(card_setcode, filter_state->field_text)) {
            return FALSE;
        }
    }
    
    // 如果卡片类型选择是"全部"，不进行类型筛选
    if (filter_state->card_type_selected == 0) {
        return TRUE;
    }
    
    gint64 card_type = get_filter_card_int(stats, "type", 0);
    if (!card_type) {
        return FALSE;
    }

    // 怪兽卡筛选
    if (filter_state->card_type_selected == 1) {
        // 首先检查是否是怪兽卡
        if (!(card_type & 0x1)) {  // TYPE_MONSTER = 0x1
            return FALSE;
        }
        
        gboolean is_link_monster = (card_type & 0x4000000) != 0;  // TYPE_LINK

        // 检查是否有任何怪兽类别被选中
        gboolean any_toggle_active = FALSE;
        for (int i = 0; i < 15; i++) {
            if (filter_state->monster_type_toggles[i]) {
                any_toggle_active = TRUE;
                break;
            }
        }
        
        // 如果有类别被选中，检查类别匹配
        if (any_toggle_active) {
            // 怪兽类别对应的type位（按照UI中的顺序）
            // "通常", "效果", "仪式", "融合", "同调", "超量", "灵摆", "连接", "调整",
            // "灵魂", "同盟", "二重", "反转", "卡通", "特殊召唤"
            const uint32_t type_flags[15] = {
                0x10,       // 通常 TYPE_NORMAL
                0x20,       // 效果 TYPE_EFFECT
                0x80,       // 仪式 TYPE_RITUAL
                0x40,       // 融合 TYPE_FUSION
                0x2000,     // 同调 TYPE_SYNCHRO
                0x800000,   // 超量 TYPE_XYZ
                0x1000000,  // 灵摆 TYPE_PENDULUM
                0x4000000,  // 连接 TYPE_LINK
                0x1000,     // 调整 TYPE_TUNER
                0x200,      // 灵魂 TYPE_SPIRIT
                0x400,      // 同盟 TYPE_UNION
                0x800,      // 二重 TYPE_DUAL
                0x200000,   // 反转 TYPE_FLIP
                0x400000,   // 卡通 TYPE_TOON
                0x2000000   // 特殊召唤 TYPE_SPSUMMON
            };
            
            // 构建目标type：所有选中的类别的OR组合
            uint32_t required_types = 0x1;  // 必须是怪兽
            for (int i = 0; i < 15; i++) {
                if (filter_state->monster_type_toggles[i]) {
                    required_types |= type_flags[i];
                }
            }
            
            // 检查卡片是否包含所有选中的类别
            gboolean match = ((card_type & required_types) == required_types);
            
            // 如果不匹配类别，直接返回
            if (!match) {
                return FALSE;
            }
        }
        
        // 检查连接箭头筛选（仅对连接怪兽有效）
        // 检查是否有连接箭头被选中
        gboolean any_link_marker_active = FALSE;
        for (int i = 0; i < 8; i++) {
            if (filter_state->link_marker_toggles[i]) {
                any_link_marker_active = TRUE;
                break;
            }
        }
        
        // 如果有连接箭头被选中，且卡片是连接怪兽，则检查箭头匹配
        if (filter_state->monster_type_toggles[MONSTER_FILTER_LINK] && any_link_marker_active && is_link_monster) {
            // 连接箭头顺序：↖ ↑ ↗ ← → ↙ ↓ ↘
            const uint32_t link_marker_flags[8] = {
                0x040,  // ↖ LINK_MARKER_TOP_LEFT
                0x080,  // ↑ LINK_MARKER_TOP
                0x100,  // ↗ LINK_MARKER_TOP_RIGHT
                0x008,  // ← LINK_MARKER_LEFT
                0x020,  // → LINK_MARKER_RIGHT
                0x001,  // ↙ LINK_MARKER_BOTTOM_LEFT
                0x002,  // ↓ LINK_MARKER_BOTTOM
                0x004   // ↘ LINK_MARKER_BOTTOM_RIGHT
            };
            
            // 构建所需的连接箭头值
            uint32_t required_markers = 0;
            for (int i = 0; i < 8; i++) {
                if (filter_state->link_marker_toggles[i]) {
                    required_markers |= link_marker_flags[i];
                }
            }
            
            // 获取卡片的def字段（连接怪兽的def存储连接箭头）
            gint64 card_def = get_filter_card_int(stats, "def", 0);
            
            // 检查连接箭头是否完全匹配
            // 注意：这里不能直接 return，否则会跳过后续其它筛选条件（属性/种族/攻防/等级等）
            gboolean link_match = ((card_def & required_markers) == required_markers);
            if (!link_match) {
                return FALSE;
            }
        }
        
        // 检查属性筛选
        if (filter_state->attribute_selected != 0) {
            // 属性列表：全部(0), 地(1), 水(2), 炎(3), 风(4), 光(5), 暗(6), 神(7)
            const char *attributes[] = {
                "全部", "地", "水", "炎", "风", "光", "暗", "神"
            };
            
            const char *selected_attribute = NULL;
            if (filter_state->attribute_selected < 8) {
                selected_attribute = attributes[filter_state->attribute_selected];
            } else {
                selected_attribute = "全部";
            }
            
            uint32_t required_attribute = get_attribute_from_string(selected_attribute);
            
            if (required_attribute != 0) {
                // 获取卡片的attribute字段
                gint64 card_attribute = get_filter_card_int(stats, "attribute", 0);
                
                // 检查属性是否匹配
                if (card_attribute != required_attribute) {
                    return FALSE;
                }
            }
        }
        
        // 检查种族筛选
        if (filter_state->race_selected != 0) {
            // 种族列表按UI顺序
            const char *races[] = {
                "全部", "战士", "魔法师", "天使", "恶魔", "不死", "机械",
                "水", "炎", "岩石", "鸟兽", "植物", "昆虫", "雷", "龙", "兽",
                "兽战士", "恐龙", "鱼", "海龙", "爬虫类", "念动力",
                "幻神兽", "创造神", "幻龙", "电子界", "幻想魔"
            };
            
            const char *selected_race = NULL;
            if (filter_state->race_selected < 27) {
                selected_race = races[filter_state->race_selected];
            } else {
                selected_race = "全部";
            }
            
            uint32_t required_race = get_race_from_string(selected_race);
            
            if (required_race != 0) {
                // 获取卡片的race字段
                gint64 card_race = get_filter_card_int(stats, "race", 0);
                
                // 检查种族是否匹配
                if (card_race != required_race) {
                    return FALSE;
                }
            }
        }
        
        if (filter_state->atk_text && *filter_state->atk_text &&
            !matches_filter_number(filter_state->atk_text, get_filter_card_int(stats, "atk", -1), TRUE)) {
            return FALSE;
        }

        // 连接怪兽没有守备力，def字段存储的是连接箭头。
        if (filter_state->def_text && *filter_state->def_text &&
            (is_link_monster ||
             !matches_filter_number(filter_state->def_text, get_filter_card_int(stats, "def", -1), TRUE))) {
            return FALSE;
        }

        gboolean has_level_filter = filter_state->level_text && *filter_state->level_text;
        gboolean has_scale_filter = filter_state->monster_type_toggles[MONSTER_FILTER_PENDULUM] &&
            ((filter_state->left_scale_text && *filter_state->left_scale_text) ||
             (filter_state->right_scale_text && *filter_state->right_scale_text));

        if (has_level_filter || has_scale_filter) {
            // level字段也用于存储连接值和超量阶级。
            if (has_level_filter && (is_link_monster || (card_type & 0x800000))) {  // TYPE_XYZ
                return FALSE;
            }

            gint64 packed_level = get_filter_card_int(stats, "level", G_MININT64);
            if (packed_level == G_MININT64) {
                return FALSE;
            }

            int level, left_scale, right_scale;
            parse_level_and_scales((uint32_t)packed_level, &level, &left_scale, &right_scale);
            if (!matches_filter_number(filter_state->level_text, level, FALSE)) {
                return FALSE;
            }
            if (has_scale_filter &&
                (!matches_filter_number(filter_state->left_scale_text, left_scale, FALSE) ||
                 !matches_filter_number(filter_state->right_scale_text, right_scale, FALSE))) {
                return FALSE;
            }
        }

        return TRUE;
    }
    
    // 魔法卡筛选
    if (filter_state->card_type_selected == 2) {
        // 获取魔法类别对应的字符串
        const char *spell_categories[] = {
            "全部", "通常", "仪式", "速攻", "永续", "装备", "场地"
        };
        
        const char *selected_category = NULL;
        if (filter_state->spell_type_selected < 7) {
            selected_category = spell_categories[filter_state->spell_type_selected];
        } else {
            selected_category = "全部";
        }
        
        // 获取目标type值
        guint32 target_type = get_spell_type_from_category(selected_category);
        
        // “通常”要求没有任何额外的类别位。
        if (filter_state->spell_type_selected == 1) {
            return card_type == target_type;
        }
        return (card_type & target_type) == target_type;
    }
    
    // 陷阱卡筛选
    if (filter_state->card_type_selected == 3) {
        // 获取陷阱类别对应的字符串
        const char *trap_categories[] = {
            "全部", "通常", "永续", "反击"
        };
        
        const char *selected_category = NULL;
        if (filter_state->trap_type_selected < 4) {
            selected_category = trap_categories[filter_state->trap_type_selected];
        } else {
            selected_category = "全部";
        }
        
        // 获取目标type值
        guint32 target_type = get_trap_type_from_category(selected_category);
        
        // “通常”要求没有任何额外的类别位。
        if (filter_state->trap_type_selected == 1) {
            return card_type == target_type;
        }
        return (card_type & target_type) == target_type;
    }
    
    // 其他类型暂不实现
    return TRUE;
}
