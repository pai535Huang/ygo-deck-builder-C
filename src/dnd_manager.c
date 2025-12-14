#include "dnd_manager.h"
#include "deck_slot.h"
#include "image_loader.h"
#include "prerelease.h"
#include <string.h>
#include <stdlib.h>

// 外部函数声明
extern void perform_move(SearchUI *ui, const char *from_region, int from_index, const char *to_region, int to_index);
extern void update_count_label(GtkLabel *label, int count);
extern int array_find_first_empty(GPtrArray *arr);
extern void array_shift_right(GPtrArray *arr, int start, int end);

// DnD: drag setup
GdkContentProvider* on_drag_prepare(GtkDragSource *source, double x, double y, gpointer user_data) {
    (void)x; (void)y; (void)user_data;
    GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
    // 判断是否为右栏行拖拽
    const char *drag_kind = (const char*)g_object_get_data(G_OBJECT(pic), "drag_kind");
    if (drag_kind && g_strcmp0(drag_kind, "search_row") == 0) {
        CardPreview *pv = (CardPreview*)g_object_get_data(G_OBJECT(pic), "preview");
        if (!pv || pv->id <= 0) return NULL;
        gboolean is_monster = FALSE, is_extra_type = FALSE;
        if (pv->type > 0) {
            if (pv->type & 0x1) is_monster = TRUE;  // TYPE_MONSTER
            if ((pv->type & 0x40) || (pv->type & 0x2000) ||
                (pv->type & 0x800000) || (pv->type & 0x4000000)) {  // FUSION, SYNCHRO, XYZ, LINK
                is_extra_type = TRUE;
            }
        }
        gboolean is_extra_card = is_monster && is_extra_type;
        // payload 格式: search:<cid>:<id>:<isExtra>:<isPrerelease>
        char *payload = g_strdup_printf("search:%d:%d:%d:%d", pv->cid, pv->id, is_extra_card ? 1 : 0, pv->is_prerelease ? 1 : 0);
        return gdk_content_provider_new_typed(G_TYPE_STRING, payload);
    }
    // 中栏槽位拖拽：仅当槽位已有图像时才允许拖拽
    GdkPixbuf *pb = slot_get_pixbuf(pic);
    if (!pb) return NULL;
    const char *region = (const char*)g_object_get_data(G_OBJECT(pic), "slot_region");
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "slot_index"));
    char *payload = g_strdup_printf("%s:%d", region ? region : "", index);
    return gdk_content_provider_new_typed(G_TYPE_STRING, payload);
}

void on_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data) {
    (void)user_data; (void)drag;
    GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
    // 若为右栏行，使用行内图片作为预览（若存在）
    const char *drag_kind = (const char*)g_object_get_data(G_OBJECT(pic), "drag_kind");
    GdkPixbuf *pb = NULL;
    if (drag_kind && g_strcmp0(drag_kind, "search_row") == 0) {
        GtkWidget *row_child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(pic));
        if (row_child) {
            GtkWidget *hbox = row_child;
            GtkWidget *thumb_stack = gtk_widget_get_first_child(hbox);
            if (thumb_stack && GTK_IS_STACK(thumb_stack)) {
                GtkWidget *picture = gtk_stack_get_child_by_name(GTK_STACK(thumb_stack), "picture");
                if (picture && GTK_IS_DRAWING_AREA(picture)) {
                    pb = slot_get_pixbuf(picture);
                }
            }
        }
    } else {
        pb = slot_get_pixbuf(pic);
    }
    if (!pb) return;
    // 将拖拽预览缩放到 50x79
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(pb, 50, 79, GDK_INTERP_BILINEAR);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    GdkTexture *tex = scaled ? gdk_texture_new_for_pixbuf(scaled) : gdk_texture_new_for_pixbuf(pb);
    G_GNUC_END_IGNORE_DEPRECATIONS
    if (tex) {
        gtk_drag_source_set_icon(source, GDK_PAINTABLE(tex), 25, 39);
        g_object_unref(tex);
    }
    if (scaled) g_object_unref(scaled);
}

// 获取卡片在当前禁限卡表中允许的最大数量
int get_card_limit(SearchUI *ui, int card_id) {
    if (card_id <= 0) return 3;  // 默认3张
    
    GHashTable *forbidden_table = NULL;
    if (ui->forbidden_dropdown) {
        guint selected = gtk_drop_down_get_selected(ui->forbidden_dropdown);
        if (selected == 0 && ui->ocg_forbidden) {
            forbidden_table = ui->ocg_forbidden;
        } else if (selected == 1 && ui->tcg_forbidden) {
            forbidden_table = ui->tcg_forbidden;
        } else if (selected == 2 && ui->sc_forbidden) {
            forbidden_table = ui->sc_forbidden;
        }
    }
    
    if (forbidden_table) {
        char cid_str[32];
        g_snprintf(cid_str, sizeof(cid_str), "%d", card_id);
        const char *status = g_hash_table_lookup(forbidden_table, cid_str);
        
        if (status) {
            if (g_strcmp0(status, "禁止") == 0) return 0;
            if (g_strcmp0(status, "限制") == 0) return 1;
            if (g_strcmp0(status, "准限制") == 0) return 2;
        }
    }
    
    return 3;  // 无限制，默认3张
}

// 统计卡组中某张卡的数量（main+side 或 extra+side）
int count_card_in_deck(SearchUI *ui, int card_id, gboolean is_extra) {
    if (card_id <= 0) return 0;
    
    int count = 0;
    GPtrArray *regions[2];
    int region_counts[2];
    
    if (is_extra) {
        regions[0] = ui->extra_pics;
        regions[1] = ui->side_pics;
        region_counts[0] = ui->extra_idx;
        region_counts[1] = ui->side_idx;
    } else {
        regions[0] = ui->main_pics;
        regions[1] = ui->side_pics;
        region_counts[0] = ui->main_idx;
        region_counts[1] = ui->side_idx;
    }
    
    for (int r = 0; r < 2; r++) {
        for (int i = 0; i < region_counts[r]; i++) {
            GtkWidget *w = GTK_WIDGET(g_ptr_array_index(regions[r], i));
            if (!w) continue;
            
            int stored_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "card_id"));
            if (stored_id == card_id) {
                count++;
            }
        }
    }
    
    return count;
}

gboolean on_drop_accept(GtkDropTarget *target, GdkDrop *drop, gpointer user_data) {
    (void)target; (void)user_data;
    // 仅接受字符串类型
    GdkContentFormats *fmts = gdk_drop_get_formats(drop);
    gboolean ok = gdk_content_formats_match(fmts, gdk_content_formats_new_for_gtype(G_TYPE_STRING));
    return ok;
}

void on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user_data) {
    (void)x; (void)y;
    SearchUI *ui = (SearchUI*)user_data;
    GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
    const char *to_region = (const char*)g_object_get_data(G_OBJECT(pic), "slot_region");
    int to_index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "slot_index"));
    if (!value || !G_VALUE_HOLDS_STRING(value)) return;
    const char *payload = g_value_get_string(value);
    if (!payload) return;
    // 支持两种 payload：
    // 1) "region:index" 来自中栏槽位拖拽
    // 2) "search:<cid>:<id>:<isExtra>:<isPrerelease>" 来自右栏行拖拽
    if (g_str_has_prefix(payload, "search:")) {
        int card_id = 0;  // cid，用于禁限卡检查和统计
        int img_id = 0;   // id，用于图片URL
        int is_extra = 0;
        int is_prerelease = 0;
        // 解析 search:cid:id:isExtra:isPrerelease
        const char *p = payload + 7;
        const char *colon1 = strchr(p, ':');
        if (colon1) {
            card_id = atoi(p);
            const char *colon2 = strchr(colon1 + 1, ':');
            if (colon2) {
                img_id = atoi(colon1 + 1);
                const char *colon3 = strchr(colon2 + 1, ':');
                if (colon3) {
                    is_extra = atoi(colon2 + 1);
                    is_prerelease = atoi(colon3 + 1);
                } else {
                    is_extra = atoi(colon2 + 1);
                }
            }
        }
        if (card_id <= 0 || img_id <= 0) return;
        
        // 检查禁限卡表限制
        int limit = get_card_limit(ui, card_id);
        if (limit == 0) return;  // 禁止卡，不能加入
        
        int current_count = count_card_in_deck(ui, card_id, is_extra ? TRUE : FALSE);
        if (current_count >= limit) return;  // 已达到上限
        
        // 类型限制
        if (g_strcmp0(to_region, "main") == 0 && is_extra) return;
        if (g_strcmp0(to_region, "extra") == 0 && !is_extra) return;
        // 选择目标数组与计数
        GPtrArray *to_arr = NULL; int *to_count = NULL; GtkLabel *to_label = NULL;
        if (g_strcmp0(to_region, "main") == 0) { to_arr = ui->main_pics; to_count = &ui->main_idx; to_label = ui->main_count; }
        else if (g_strcmp0(to_region, "extra") == 0) { to_arr = ui->extra_pics; to_count = &ui->extra_idx; to_label = ui->extra_count; }
        else if (g_strcmp0(to_region, "side") == 0) { to_arr = ui->side_pics; to_count = &ui->side_idx; to_label = ui->side_count; }
        if (!to_arr || !to_count) return;
        if (to_index < 0 || to_index >= (int)to_arr->len) return;
        // 自动放置：若目标与前一个都空，放到第一个空位
        int place_index = to_index;
        GtkWidget *prev = (to_index > 0) ? GTK_WIDGET(g_ptr_array_index(to_arr, to_index - 1)) : NULL;
        GtkWidget *to_chk = GTK_WIDGET(g_ptr_array_index(to_arr, to_index));
        gboolean prev_empty = !prev || slot_get_pixbuf(prev) == NULL;
        gboolean target_empty = slot_get_pixbuf(to_chk) == NULL;
        if (prev_empty && target_empty) {
            int first_empty = array_find_first_empty(to_arr);
            if (first_empty >= 0) place_index = first_empty;
        }
        // 若插入位置在已填范围内，右移并插入
        if (place_index < *to_count) {
            int end_to = *to_count - 1;
            if (end_to >= 0) array_shift_right(to_arr, place_index, end_to);
        }
        GtkWidget *place_w = GTK_WIDGET(g_ptr_array_index(to_arr, place_index));
        // 记录类型标记和卡片ID（用于统计禁限卡数量）
        slot_set_is_extra(place_w, is_extra ? TRUE : FALSE);
        g_object_set_data(G_OBJECT(place_w), "card_id", GINT_TO_POINTER(card_id));
        g_object_set_data(G_OBJECT(place_w), "img_id", GINT_TO_POINTER(img_id));
        
        // 如果是先行卡，从本地加载图片
        if (is_prerelease) {
            gchar *local_path = get_prerelease_card_image_path(img_id);
            if (local_path && g_file_test(local_path, G_FILE_TEST_EXISTS)) {
                GError *error = NULL;
                GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(local_path, &error);
                if (pixbuf) {
                    slot_set_pixbuf(place_w, pixbuf);
                    g_object_unref(pixbuf);
                } else {
                    if (error) {
                        g_warning("加载先行卡图片失败: %s", error->message);
                        g_error_free(error);
                    }
                }
            }
            g_free(local_path);
        } else {
            // 非先行卡，从缓存或在线加载
            // 若缓存中有该卡的缩略图，先立即显示以消除延迟
            if (img_id > 0) {
                GdkPixbuf *cached = get_thumb_from_cache(img_id);
                if (cached) {
                    slot_set_pixbuf(place_w, cached);
                    g_object_unref(cached);
                }
            }
            // 异步加载图片到目标槽位
            char url[128];
            g_snprintf(url, sizeof url, "https://cdn.233.momobako.com/ygoimg/jp/%d.webp", img_id);
            ImageLoadCtx *ctx = g_new0(ImageLoadCtx, 1);
            ctx->stack = NULL;
            ctx->target = GTK_WIDGET(place_w);
            // 防止目标槽位被销毁后异步回调访问
            g_object_add_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
            ctx->scale_to_thumb = TRUE;
            ctx->cache_id = 0;
            ctx->add_to_thumb_cache = FALSE;
            ctx->url = g_strdup(url);
            load_image_async(ui->session, url, ctx);
        }
        if (*to_count < place_index + 1) *to_count = place_index + 1;
        update_count_label(to_label, *to_count);
        return;
    }
    // 原有 region:index 处理
    char from_region[8] = {0};
    int from_index = -1;
    char *colon = strchr(payload, ':');
    if (colon) {
        size_t len = (size_t)(colon - payload);
        if (len >= sizeof(from_region)) len = sizeof(from_region) - 1;
        memcpy(from_region, payload, len);
        from_region[len] = '\0';
        from_index = atoi(colon + 1);
    }
    if (from_index < 0 || from_region[0] == '\0') return;
    perform_move(ui, from_region, from_index, to_region, to_index);
}
