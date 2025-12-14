#include <adwaita.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <gdk/gdk.h>
#include <gio/gio.h>
#include "app_types.h"
#include "startup_update.h"
#include "prerelease.h"
#include "offline_data.h"
#include "card_info.h"
#include "card_sort.h"
#include "card_shuffle.h"
#include "deck_slot.h"
#include "forbidden_list.h"
#include "deck_clear.h"
#include "deck_io.h"
#include "image_loader.h"
#include "dnd_manager.h"
#include "search_filter.h"
#include "deck_url.h"

// 全局变量：缓存最后导出和导入的目录
static char *last_export_directory = NULL;
static char *last_import_directory = NULL;

// 全局变量：是否在搜索结果中显示先行卡（默认显示）
gboolean show_prerelease_cards = TRUE;

// 全局筛选状态（保持默认值）
// FilterState 定义在 search_filter.h 中
static FilterState filter_state = {
    .card_type_selected = 0,
    .monster_type_toggles = {FALSE},
    .link_marker_toggles = {FALSE},
    .spell_type_selected = 0,
    .trap_type_selected = 0,
    .attribute_selected = 0,
    .race_selected = 0,
    .atk_text = NULL,
    .def_text = NULL,
    .level_text = NULL,
    .left_scale_text = NULL,
    .right_scale_text = NULL
};

// 获取当前筛选状态的指针（供search_filter.c使用）
const FilterState* get_current_filter_state(void) {
    return (const FilterState*)&filter_state;
}

// 检查是否有活动的筛选条件
gboolean has_active_filter(void) {
    // 检查卡片类型选择
    if (filter_state.card_type_selected != 0) {
        return TRUE;
    }
    
    // 检查怪兽类别toggles
    for (int i = 0; i < 15; i++) {
        if (filter_state.monster_type_toggles[i]) {
            return TRUE;
        }
    }
    
    // 检查连接箭头toggles
    for (int i = 0; i < 8; i++) {
        if (filter_state.link_marker_toggles[i]) {
            return TRUE;
        }
    }
    
    // 检查魔法/陷阱类别
    if (filter_state.spell_type_selected != 0 || filter_state.trap_type_selected != 0) {
        return TRUE;
    }
    
    // 检查属性和种族
    if (filter_state.attribute_selected != 0 || filter_state.race_selected != 0) {
        return TRUE;
    }
    
    // 检查文本筛选条件
    if ((filter_state.atk_text && filter_state.atk_text[0] != '\0') ||
        (filter_state.def_text && filter_state.def_text[0] != '\0') ||
        (filter_state.level_text && filter_state.level_text[0] != '\0') ||
        (filter_state.left_scale_text && filter_state.left_scale_text[0] != '\0') ||
        (filter_state.right_scale_text && filter_state.right_scale_text[0] != '\0')) {
        return TRUE;
    }
    
    return FALSE;
}

// 配置文件路径
#define CONFIG_DIR ".config/ygo-deck-builder"
#define CONFIG_FILE "settings.conf"

/**
 * 获取禁限卡表文件的完整路径
 * 返回值需要调用者使用 g_free() 释放
 */
static gchar *get_forbidden_list_path(const char *filename) {
    const gchar *config_dir = g_get_user_config_dir();
    if (!config_dir) {
        g_warning("Unable to get user config directory");
        return NULL;
    }
    
    return g_build_filename(config_dir, "ygo-deck-builder", "data", filename, NULL);
}

// 通过 DnD 传递简单字符串 payload，例如 "main:12"

// 前置声明
static void on_export_clicked(GtkButton *btn, gpointer user_data);

void list_clear(GtkListBox *list) {
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list));
    while (child) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(list, child);
        child = next;
    }
}

void draw_pixbuf_scaled(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer user_data) {
    (void)user_data;
    GdkPixbuf *pb = (GdkPixbuf*)g_object_get_data(G_OBJECT(area), "pixbuf");
    if (!pb) return;
    int pw = gdk_pixbuf_get_width(pb);
    int ph = gdk_pixbuf_get_height(pb);
    if (pw <= 0 || ph <= 0) return;
    
    // 获取设备像素比例以支持高DPI显示
    double scale = gtk_widget_get_scale_factor(GTK_WIDGET(area));
    
    // 检查缓存的surface是否存在且尺寸匹配
    cairo_surface_t *cached = (cairo_surface_t*)g_object_get_data(G_OBJECT(area), "cached_surface");
    int cached_w = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "cached_width"));
    int cached_h = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "cached_height"));
    double cached_scale = g_object_get_data(G_OBJECT(area), "cached_scale") ? 
                          *(double*)g_object_get_data(G_OBJECT(area), "cached_scale") : 1.0;
    
    if (cached && cached_w == width && cached_h == height && cached_scale == scale) {
        // 使用缓存的surface,避免重新缩放和绘制
        cairo_set_source_surface(cr, cached, 0, 0);
        // 设置高质量过滤器以保持清晰度
        cairo_pattern_t *pattern = cairo_get_source(cr);
        if (pattern) cairo_pattern_set_filter(pattern, CAIRO_FILTER_BEST);
        cairo_paint(cr);
        return;
    }
    
    double sx = (double)width / (double)pw;
    double sy = (double)height / (double)ph;
    double s = sx < sy ? sx : sy; // contain
    double rw = pw * s;
    double rh = ph * s;
    double tx = (width - rw) / 2.0;
    double ty = (height - rh) / 2.0;

    // 创建高分辨率的缓存surface以支持高DPI
    int surface_w = (int)(width * scale);
    int surface_h = (int)(height * scale);
    cairo_surface_t *new_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, surface_w, surface_h);
    cairo_surface_set_device_scale(new_surface, scale, scale);
    cairo_t *cache_cr = cairo_create(new_surface);
    
    // 清除背景为透明
    cairo_set_operator(cache_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cache_cr);
    cairo_set_operator(cache_cr, CAIRO_OPERATOR_OVER);
    
    cairo_save(cache_cr);
    cairo_translate(cache_cr, tx, ty);
    cairo_scale(cache_cr, s, s);
    gdk_cairo_set_source_pixbuf(cache_cr, pb, 0, 0);
    cairo_pattern_t *pat = cairo_get_source(cache_cr);
    if (pat) cairo_pattern_set_filter(pat, CAIRO_FILTER_BEST);
    cairo_rectangle(cache_cr, 0, 0, pw, ph);
    cairo_fill(cache_cr);
    cairo_restore(cache_cr);
    cairo_destroy(cache_cr);
    
    // 缓存surface和scale
    double *scale_ptr = g_new(double, 1);
    *scale_ptr = scale;
    g_object_set_data_full(G_OBJECT(area), "cached_surface", new_surface, (GDestroyNotify)cairo_surface_destroy);
    g_object_set_data(G_OBJECT(area), "cached_width", GINT_TO_POINTER(width));
    g_object_set_data(G_OBJECT(area), "cached_height", GINT_TO_POINTER(height));
    g_object_set_data_full(G_OBJECT(area), "cached_scale", scale_ptr, g_free);
    
    // 绘制缓存的surface,使用高质量过滤器
    cairo_set_source_surface(cr, new_surface, 0, 0);
    cairo_pattern_t *pattern = cairo_get_source(cr);
    if (pattern) cairo_pattern_set_filter(pattern, CAIRO_FILTER_BEST);
    cairo_paint(cr);
}

// 当 DrawingArea 销毁时清理 pixbuf 数据和缓存的surface，防止 Cairo 上下文问题
void on_drawing_area_destroy(GtkWidget *widget, gpointer user_data) {
    (void)user_data;
    g_object_set_data(G_OBJECT(widget), "pixbuf", NULL);
    g_object_set_data(G_OBJECT(widget), "cached_surface", NULL);
    g_object_set_data(G_OBJECT(widget), "cached_width", NULL);
    g_object_set_data(G_OBJECT(widget), "cached_height", NULL);
    g_object_set_data(G_OBJECT(widget), "cached_scale", NULL);
}

// 槽位点击：删除并前移
static void on_slot_clicked(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)x; (void)y;
    SearchUI *ui = (SearchUI*)user_data;
    GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    // 区分点击与拖拽：比较按下与释放位置（安全存储为 double[2]）
    double *press_xy = (double*)g_object_get_data(G_OBJECT(pic), "press_xy");
    if (press_xy) {
        double px = press_xy[0];
        double py = press_xy[1];
        double dx = fabs(x - px);
        double dy = fabs(y - py);
        const double threshold = 3.0; // 容差像素
        if (dx > threshold || dy > threshold) {
            // 认为是拖拽，释放时不执行删除
            // 清理
            g_object_set_data_full(G_OBJECT(pic), "press_xy", NULL, NULL);
            return;
        }
    }
    const char *region = (const char*)g_object_get_data(G_OBJECT(pic), "slot_region");
    int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "slot_index"));
    if (!region) return;
    if (g_strcmp0(region, "main") == 0) {
        if (index < ui->main_idx) {
            shift_delete_slots(ui->main_pics, &ui->main_idx, index);
            update_count_label(ui->main_count, ui->main_idx);
        }
    } else if (g_strcmp0(region, "extra") == 0) {
        if (index < ui->extra_idx) {
            shift_delete_slots(ui->extra_pics, &ui->extra_idx, index);
            update_count_label(ui->extra_count, ui->extra_idx);
        }
    } else if (g_strcmp0(region, "side") == 0) {
        if (index < ui->side_idx) {
            shift_delete_slots(ui->side_pics, &ui->side_idx, index);
            update_count_label(ui->side_count, ui->side_idx);
        }
    }
    // 清理按下坐标
    g_object_set_data_full(G_OBJECT(pic), "press_xy", NULL, NULL);
}

// 记录按下位置用于区分拖拽
static void on_slot_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)user_data;
    GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    // 安全存储为 double[2]
    double *press_xy = g_new(double, 2);
    press_xy[0] = x;
    press_xy[1] = y;
    g_object_set_data_full(G_OBJECT(pic), "press_xy", press_xy, g_free);
}

static gboolean is_allowed_move(const char *from, const char *to) {
    if (g_strcmp0(from, to) == 0) return TRUE; // intra-region
    if ((g_strcmp0(from, "main") == 0 && g_strcmp0(to, "side") == 0) ||
        (g_strcmp0(from, "side") == 0 && g_strcmp0(to, "main") == 0)) return TRUE;
    if ((g_strcmp0(from, "extra") == 0 && g_strcmp0(to, "side") == 0) ||
        (g_strcmp0(from, "side") == 0 && g_strcmp0(to, "extra") == 0)) return TRUE;
    return FALSE; // disallow main<->extra
}

void perform_move(SearchUI *ui, const char *from_region, int from_index, const char *to_region, int to_index) {
    if (!is_allowed_move(from_region, to_region)) return;
    // Get arrays and counts
    GPtrArray *from_arr = NULL, *to_arr = NULL;
    int *from_count = NULL, *to_count = NULL;
    GtkLabel *from_label = NULL, *to_label = NULL;
    if (g_strcmp0(from_region, "main") == 0) { from_arr = ui->main_pics; from_count = &ui->main_idx; from_label = ui->main_count; }
    else if (g_strcmp0(from_region, "extra") == 0) { from_arr = ui->extra_pics; from_count = &ui->extra_idx; from_label = ui->extra_count; }
    else if (g_strcmp0(from_region, "side") == 0) { from_arr = ui->side_pics; from_count = &ui->side_idx; from_label = ui->side_count; }

    if (g_strcmp0(to_region, "main") == 0) { to_arr = ui->main_pics; to_count = &ui->main_idx; to_label = ui->main_count; }
    else if (g_strcmp0(to_region, "extra") == 0) { to_arr = ui->extra_pics; to_count = &ui->extra_idx; to_label = ui->extra_count; }
    else if (g_strcmp0(to_region, "side") == 0) { to_arr = ui->side_pics; to_count = &ui->side_idx; to_label = ui->side_count; }


    GtkWidget *from_widget = GTK_WIDGET(g_ptr_array_index(from_arr, from_index));
    GdkPixbuf *moving_pb = slot_get_pixbuf(from_widget);
    if (!moving_pb) return;
    GdkPixbuf *moving_pb_ref = g_object_ref(moving_pb);
    gboolean moving_is_extra = slot_get_is_extra(from_widget);
    int moving_card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(from_widget), "card_id"));
    int moving_img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(from_widget), "img_id"));

    // 规则限制：跨区域移动时
    // - 额外怪兽不可进入 main
    // - 非额外卡不可进入 extra
    if (g_strcmp0(from_region, to_region) != 0) {
        if (g_strcmp0(to_region, "main") == 0 && moving_is_extra) {
            g_object_unref(moving_pb_ref);
            return;
        }
        if (g_strcmp0(to_region, "extra") == 0 && !moving_is_extra) {
            g_object_unref(moving_pb_ref);
            return;
        }
    }

    if (from_arr == to_arr) {
        // Intra-region: if dropping onto filled slot, swap; else move to empty
        if (to_index == from_index) return;
        if (to_index < *from_count) {
            GtkWidget *to_widget = GTK_WIDGET(g_ptr_array_index(from_arr, to_index));
            GdkPixbuf *dest_pb = slot_get_pixbuf(to_widget);
            // Take a temporary ref BEFORE overwriting destination to avoid use-after-unref
            GdkPixbuf *dest_pb_ref = dest_pb ? g_object_ref(dest_pb) : NULL;
            // swap pixbuf
            slot_set_pixbuf(to_widget, moving_pb_ref);
            // swap type flags
            gboolean dest_is_extra = slot_get_is_extra(to_widget);
            slot_set_is_extra(to_widget, moving_is_extra);
            // swap card_id and img_id
            int from_card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(from_widget), "card_id"));
            int to_card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(to_widget), "card_id"));
            int from_img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(from_widget), "img_id"));
            int to_img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(to_widget), "img_id"));
            g_object_set_data(G_OBJECT(to_widget), "card_id", GINT_TO_POINTER(from_card_id));
            g_object_set_data(G_OBJECT(to_widget), "img_id", GINT_TO_POINTER(from_img_id));
            if (dest_pb_ref) {
                slot_set_is_extra(from_widget, dest_is_extra);
                slot_set_pixbuf(from_widget, dest_pb_ref);
                g_object_set_data(G_OBJECT(from_widget), "card_id", GINT_TO_POINTER(to_card_id));
                g_object_set_data(G_OBJECT(from_widget), "img_id", GINT_TO_POINTER(to_img_id));
                g_object_unref(dest_pb_ref);
            } else {
                slot_set_is_extra(from_widget, FALSE);
                slot_set_pixbuf(from_widget, NULL);
                g_object_set_data(G_OBJECT(from_widget), "card_id", GINT_TO_POINTER(0));
                g_object_set_data(G_OBJECT(from_widget), "img_id", GINT_TO_POINTER(0));
            }
        } else {
            // moving to empty slot within region
            // Clear original source since we're moving to an empty slot
            slot_set_pixbuf(from_widget, NULL);
            slot_set_is_extra(from_widget, FALSE);
            g_object_set_data(G_OBJECT(from_widget), "card_id", GINT_TO_POINTER(0));
            g_object_set_data(G_OBJECT(from_widget), "img_id", GINT_TO_POINTER(0));
            int place_index = to_index;
            // Feature: if target and its previous are empty, place to first truly empty index (scan)
            if (to_index >= *from_count) {
                GtkWidget *prev = (to_index > 0) ? GTK_WIDGET(g_ptr_array_index(from_arr, to_index - 1)) : NULL;
                GtkWidget *to_w_chk = GTK_WIDGET(g_ptr_array_index(from_arr, to_index));
                gboolean prev_empty = !prev || slot_get_pixbuf(prev) == NULL;
                gboolean target_empty = slot_get_pixbuf(to_w_chk) == NULL;
                if (prev_empty && target_empty) {
                    int first_empty = array_find_first_empty(from_arr);
                    if (first_empty >= 0) {
                        place_index = first_empty;
                    }
                }
            }
            GtkWidget *to_w = GTK_WIDGET(g_ptr_array_index(from_arr, place_index));
            slot_set_pixbuf(to_w, moving_pb_ref);
            slot_set_is_extra(to_w, moving_is_extra);
            g_object_set_data(G_OBJECT(to_w), "card_id", GINT_TO_POINTER(moving_card_id));
            g_object_set_data(G_OBJECT(to_w), "img_id", GINT_TO_POINTER(moving_img_id));
            // update count if we extended or filled a lower empty slot
            if (*from_count < place_index + 1) *from_count = place_index + 1;
        }
    } else {
        // Cross-region: from removes, to inserts
        GtkWidget *to_widget = GTK_WIDGET(g_ptr_array_index(to_arr, to_index));
        GdkPixbuf *dest_pb = slot_get_pixbuf(to_widget);
        // Take a temporary ref BEFORE overwriting destination to avoid use-after-unref
        GdkPixbuf *dest_pb_ref = dest_pb ? g_object_ref(dest_pb) : NULL;
        if (to_index < *to_count && dest_pb) {
            // Destination filled: swap without shifting; counts unchanged
            slot_set_pixbuf(to_widget, moving_pb_ref);
            // swap type flags
            gboolean dest_is_extra = slot_get_is_extra(to_widget);
            slot_set_is_extra(to_widget, moving_is_extra);
            // swap card_id and img_id
            int to_card_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(to_widget), "card_id"));
            int to_img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(to_widget), "img_id"));
            g_object_set_data(G_OBJECT(to_widget), "card_id", GINT_TO_POINTER(moving_card_id));
            g_object_set_data(G_OBJECT(to_widget), "img_id", GINT_TO_POINTER(moving_img_id));
            if (dest_pb_ref) {
                slot_set_is_extra(from_widget, dest_is_extra);
                slot_set_pixbuf(from_widget, dest_pb_ref);
                g_object_set_data(G_OBJECT(from_widget), "card_id", GINT_TO_POINTER(to_card_id));
                g_object_set_data(G_OBJECT(from_widget), "img_id", GINT_TO_POINTER(to_img_id));
                g_object_unref(dest_pb_ref);
            } else {
                slot_set_is_extra(from_widget, FALSE);
                slot_set_pixbuf(from_widget, NULL);
                g_object_set_data(G_OBJECT(from_widget), "card_id", GINT_TO_POINTER(0));
                g_object_set_data(G_OBJECT(from_widget), "img_id", GINT_TO_POINTER(0));
            }
        } else {
            // Destination empty: remove from source, insert into destination, update counts
            int end_from = *from_count - 1;
            if (from_index != end_from) {
                array_shift_left(from_arr, from_index, end_from);
            } else {
            }
            GtkWidget *from_last = GTK_WIDGET(g_ptr_array_index(from_arr, end_from));
            slot_set_pixbuf(from_last, NULL);
            slot_set_is_extra(from_last, FALSE);
            g_object_set_data(G_OBJECT(from_last), "card_id", GINT_TO_POINTER(0));
            g_object_set_data(G_OBJECT(from_last), "img_id", GINT_TO_POINTER(0));
            *from_count = *from_count - 1;

            // Insert into destination
            int place_index = to_index;
            // Feature: if target and its previous are empty, place to first truly empty index (scan)
            {
                GtkWidget *prev = (to_index > 0) ? GTK_WIDGET(g_ptr_array_index(to_arr, to_index - 1)) : NULL;
                GtkWidget *to_w_chk = GTK_WIDGET(g_ptr_array_index(to_arr, to_index));
                gboolean prev_empty = !prev || slot_get_pixbuf(prev) == NULL;
                gboolean target_empty = slot_get_pixbuf(to_w_chk) == NULL;
                if (prev_empty && target_empty) {
                    int first_empty = array_find_first_empty(to_arr);
                    if (first_empty >= 0) {
                        place_index = first_empty;
                    }
                }
            }
            if (place_index < *to_count) {
                int end_to = *to_count - 1;
                if (end_to >= 0) {
                    array_shift_right(to_arr, place_index, end_to);
                }
                GtkWidget *place_w = GTK_WIDGET(g_ptr_array_index(to_arr, place_index));
                slot_set_pixbuf(place_w, moving_pb_ref);
                slot_set_is_extra(place_w, moving_is_extra);
                g_object_set_data(G_OBJECT(place_w), "card_id", GINT_TO_POINTER(moving_card_id));
                g_object_set_data(G_OBJECT(place_w), "img_id", GINT_TO_POINTER(moving_img_id));
            } else {
                // place at first empty slot and increase count
                GtkWidget *place_w = GTK_WIDGET(g_ptr_array_index(to_arr, place_index));
                slot_set_pixbuf(place_w, moving_pb_ref);
                slot_set_is_extra(place_w, moving_is_extra);
                g_object_set_data(G_OBJECT(place_w), "card_id", GINT_TO_POINTER(moving_card_id));
                g_object_set_data(G_OBJECT(place_w), "img_id", GINT_TO_POINTER(moving_img_id));
            }
            if (*to_count < place_index + 1) *to_count = place_index + 1;
        }
    }
    update_count_label(from_label, *from_count);
    update_count_label(to_label, *to_count);
    g_object_unref(moving_pb_ref);
}

void free_card_preview(gpointer data) {
    CardPreview *pv = (CardPreview*)data;
    if (!pv) return;
    g_free(pv->cn_name);
    g_free(pv->types);
    g_free(pv->pdesc);
    g_free(pv->desc);
    g_free(pv);
}

typedef struct {
    SearchUI *ui;
    GtkFileDialog *dialog;
} ExportData;

typedef struct {
    SearchUI *ui;
    GtkFileDialog *dialog;
} ImportData;



// 文件保存对话框回调
static void on_export_file_save_finish(GObject *source, GAsyncResult *result, gpointer user_data) {
    ExportData *export_data = (ExportData*)user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, &error);
    
    if (file) {
        char *path = g_file_get_path(file);
        export_deck_to_ydk(
            export_data->ui->main_pics, export_data->ui->main_idx,
            export_data->ui->extra_pics, export_data->ui->extra_idx,
            export_data->ui->side_pics, export_data->ui->side_idx,
            path
        );
        
        // 保存目录到缓存
        char *dir = g_path_get_dirname(path);
        g_free(last_export_directory);
        last_export_directory = dir;
        
        // 保存到配置文件
        save_io_config(last_export_directory, last_import_directory);
        
        g_free(path);
        g_object_unref(file);
    } else if (error) {
        if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            g_warning("导出失败: %s", error->message);
        }
        g_error_free(error);
    }
    
    g_object_unref(export_data->dialog);
    g_free(export_data);
}

// 文件打开对话框回调
static void on_import_file_open_finish(GObject *source, GAsyncResult *result, gpointer user_data) {
    ImportData *import_data = (ImportData*)user_data;
    GError *error = NULL;
    GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
    
    if (file) {
        char *path = g_file_get_path(file);
        import_deck_from_ydk(
            import_data->ui->main_pics, &import_data->ui->main_idx, import_data->ui->main_count,
            import_data->ui->extra_pics, &import_data->ui->extra_idx, import_data->ui->extra_count,
            import_data->ui->side_pics, &import_data->ui->side_idx, import_data->ui->side_count,
            import_data->ui->session,
            path
        );
        
        // 保存导入目录到缓存
        char *dir = g_path_get_dirname(path);
        g_free(last_import_directory);
        last_import_directory = dir;
        
        // 保存到配置文件
        save_io_config(last_export_directory, last_import_directory);
        
        g_free(path);
        g_object_unref(file);
    } else if (error) {
        if (!g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED)) {
            g_warning("导入失败: %s", error->message);
        }
        g_error_free(error);
    }
    
    g_object_unref(import_data->dialog);
    g_free(import_data);
}

// 导入按钮回调：显示文件打开对话框
static void on_import_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SearchUI *ui = (SearchUI*)user_data;
    
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "导入卡组");
    
    // 设置初始目录
    if (last_import_directory && g_file_test(last_import_directory, G_FILE_TEST_IS_DIR)) {
        GFile *initial_folder = g_file_new_for_path(last_import_directory);
        gtk_file_dialog_set_initial_folder(dialog, initial_folder);
        g_object_unref(initial_folder);
    }
    
    // 设置文件过滤器
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(filter, "*.ydk");
    gtk_file_filter_set_name(filter, "YDK文件 (*.ydk)");
    
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filter);
    
    ImportData *data = g_new0(ImportData, 1);
    data->ui = ui;
    data->dialog = g_object_ref(dialog);
    
    gtk_file_dialog_open(dialog, 
                        NULL, 
                        NULL,
                        on_import_file_open_finish,
                        data);
}

// 重置筛选状态为默认值
static void reset_filter_state(void) {
    filter_state.card_type_selected = 0;
    
    // 重置怪兽类别toggle
    for (int i = 0; i < 14; i++) {
        filter_state.monster_type_toggles[i] = FALSE;
    }
    
    // 重置连接箭头toggle
    for (int i = 0; i < 8; i++) {
        filter_state.link_marker_toggles[i] = FALSE;
    }
    
    filter_state.spell_type_selected = 0;
    filter_state.trap_type_selected = 0;
    filter_state.attribute_selected = 0;
    filter_state.race_selected = 0;
    
    // 释放并重置文本字段
    g_free(filter_state.atk_text);
    g_free(filter_state.def_text);
    g_free(filter_state.level_text);
    g_free(filter_state.left_scale_text);
    g_free(filter_state.right_scale_text);
    
    filter_state.atk_text = NULL;
    filter_state.def_text = NULL;
    filter_state.level_text = NULL;
    filter_state.left_scale_text = NULL;
    filter_state.right_scale_text = NULL;
}

// "恢复默认"按钮回调：重置筛选状态并更新UI控件
static void on_reset_filter_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AdwDialog *dialog = ADW_DIALOG(user_data);
    
    // 重置筛选状态
    reset_filter_state();
    
    // 更新UI控件以反映默认值
    
    // 1. 卡片类型下拉菜单
    AdwComboRow *type_row = g_object_get_data(G_OBJECT(dialog), "type_row");
    if (type_row) {
        adw_combo_row_set_selected(type_row, 0);  // 全部
    }
    
    // 2. 怪兽类别 toggle buttons
    GtkWidget *flow_box = g_object_get_data(G_OBJECT(dialog), "flow_box");
    if (flow_box) {
        GtkWidget *child = gtk_widget_get_first_child(flow_box);
        while (child) {
            GtkWidget *toggle = gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(child));
            if (toggle && GTK_IS_TOGGLE_BUTTON(toggle)) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), FALSE);
            }
            child = gtk_widget_get_next_sibling(child);
        }
    }
    
    // 3. 魔法类别下拉菜单
    AdwComboRow *spell_type_row = g_object_get_data(G_OBJECT(dialog), "spell_type_row");
    if (spell_type_row) {
        adw_combo_row_set_selected(spell_type_row, 0);  // 全部
    }
    
    // 4. 陷阱类别下拉菜单
    AdwComboRow *trap_type_row = g_object_get_data(G_OBJECT(dialog), "trap_type_row");
    if (trap_type_row) {
        adw_combo_row_set_selected(trap_type_row, 0);  // 全部
    }
    
    // 5. 属性下拉菜单
    AdwComboRow *attribute_row = g_object_get_data(G_OBJECT(dialog), "attribute_row");
    if (attribute_row) {
        adw_combo_row_set_selected(attribute_row, 0);  // 全部
    }
    
    // 6. 种族下拉菜单
    AdwComboRow *race_row = g_object_get_data(G_OBJECT(dialog), "race_row");
    if (race_row) {
        adw_combo_row_set_selected(race_row, 0);  // 全部
    }
    
    // 7. 攻击力输入框
    GtkEntry *atk_row = g_object_get_data(G_OBJECT(dialog), "atk_row");
    if (atk_row) {
        gtk_editable_set_text(GTK_EDITABLE(atk_row), "");
    }
    
    // 8. 守备力输入框
    GtkEntry *def_row = g_object_get_data(G_OBJECT(dialog), "def_row");
    if (def_row) {
        gtk_editable_set_text(GTK_EDITABLE(def_row), "");
    }
    
    // 9. 等级输入框
    GtkEntry *level_row = g_object_get_data(G_OBJECT(dialog), "level_row");
    if (level_row) {
        gtk_editable_set_text(GTK_EDITABLE(level_row), "");
    }
    
    // 10. 灵摆左刻度输入框
    GtkEntry *left_scale_entry = g_object_get_data(G_OBJECT(dialog), "left_scale_entry");
    if (left_scale_entry) {
        gtk_editable_set_text(GTK_EDITABLE(left_scale_entry), "");
    }
    
    // 11. 灵摆右刻度输入框
    GtkEntry *right_scale_entry = g_object_get_data(G_OBJECT(dialog), "right_scale_entry");
    if (right_scale_entry) {
        gtk_editable_set_text(GTK_EDITABLE(right_scale_entry), "");
    }
    
    // 12. 连接箭头 toggle buttons
    GtkWidget *link_grid = g_object_get_data(G_OBJECT(dialog), "link_grid");
    if (link_grid) {
        GtkWidget *child = gtk_widget_get_first_child(link_grid);
        while (child) {
            if (GTK_IS_TOGGLE_BUTTON(child)) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(child), FALSE);
            }
            child = gtk_widget_get_next_sibling(child);
        }
    }
}

// 卡片类型选择变化回调：控制怪兽和魔法相关筛选项的显示
static void on_card_type_changed(AdwComboRow *combo_row, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    GtkWidget **groups = (GtkWidget**)user_data;
    GtkWidget *monster_type_group = groups[0];
    GtkWidget *monster_attrs_group = groups[1];
    GtkWidget *stats_group = groups[2];
    GtkWidget *spell_type_group = groups[5];  // 魔法类别组
    GtkWidget *trap_type_group = groups[6];   // 陷阱类别组
    
    guint selected = adw_combo_row_get_selected(combo_row);
    
    // 选中"怪兽"（索引1）时显示怪兽相关选项，否则隐藏
    gboolean show_monster_options = (selected == 1);
    
    gtk_widget_set_visible(monster_type_group, show_monster_options);
    gtk_widget_set_visible(monster_attrs_group, show_monster_options);
    gtk_widget_set_visible(stats_group, show_monster_options);
    // 注意：pendulum_group和link_markers_group的显示由各自的toggle控制，
    // 但当不是怪兽类型时，它们也应该隐藏
    if (!show_monster_options && groups[3]) {
        gtk_widget_set_visible(groups[3], FALSE);  // pendulum_group
    }
    if (!show_monster_options && groups[4]) {
        gtk_widget_set_visible(groups[4], FALSE);  // link_markers_group
    }
    
    // 选中"魔法"（索引2）时显示魔法类别选项，否则隐藏
    gboolean show_spell_options = (selected == 2);
    gtk_widget_set_visible(spell_type_group, show_spell_options);
    
    // 选中"陷阱"（索引3）时显示陷阱类别选项，否则隐藏
    gboolean show_trap_options = (selected == 3);
    gtk_widget_set_visible(trap_type_group, show_trap_options);
}

// 连接toggle状态变化回调：控制连接箭头组的显示
static void on_link_toggle_changed(GtkToggleButton *toggle, gpointer user_data) {
    GtkWidget *link_markers_group = GTK_WIDGET(user_data);
    gboolean active = gtk_toggle_button_get_active(toggle);
    gtk_widget_set_visible(link_markers_group, active);
}

// 保存当前筛选状态的回调（对话框关闭时）
static void on_filter_dialog_closed(AdwDialog *dialog, gpointer user_data) {
    (void)user_data;
    
    // 获取所有控件
    AdwComboRow *type_row = g_object_get_data(G_OBJECT(dialog), "type_row");
    GtkFlowBox *flow_box = g_object_get_data(G_OBJECT(dialog), "flow_box");
    GtkGrid *link_grid = g_object_get_data(G_OBJECT(dialog), "link_grid");
    AdwComboRow *spell_type_row = g_object_get_data(G_OBJECT(dialog), "spell_type_row");
    AdwComboRow *trap_type_row = g_object_get_data(G_OBJECT(dialog), "trap_type_row");
    AdwComboRow *attribute_row = g_object_get_data(G_OBJECT(dialog), "attribute_row");
    AdwComboRow *race_row = g_object_get_data(G_OBJECT(dialog), "race_row");
    AdwEntryRow *atk_row = g_object_get_data(G_OBJECT(dialog), "atk_row");
    AdwEntryRow *def_row = g_object_get_data(G_OBJECT(dialog), "def_row");
    AdwEntryRow *level_row = g_object_get_data(G_OBJECT(dialog), "level_row");
    GtkEntry *left_scale_entry = g_object_get_data(G_OBJECT(dialog), "left_scale_entry");
    GtkEntry *right_scale_entry = g_object_get_data(G_OBJECT(dialog), "right_scale_entry");
    
    // 保存状态
    filter_state.card_type_selected = adw_combo_row_get_selected(type_row);
    filter_state.spell_type_selected = spell_type_row ? adw_combo_row_get_selected(spell_type_row) : 0;
    filter_state.trap_type_selected = trap_type_row ? adw_combo_row_get_selected(trap_type_row) : 0;
    filter_state.attribute_selected = adw_combo_row_get_selected(attribute_row);
    filter_state.race_selected = adw_combo_row_get_selected(race_row);
    
    // 保存怪兽类别toggle状态
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(flow_box));
    int i = 0;
    while (child && i < 15) {
        GtkWidget *toggle = gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(child));
        if (GTK_IS_TOGGLE_BUTTON(toggle)) {
            filter_state.monster_type_toggles[i] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
            i++;
        }
        child = gtk_widget_get_next_sibling(child);
    }
    
    // 保存连接箭头toggle状态（按照grid中的位置顺序：↖ ↑ ↗ ← → ↙ ↓ ↘）
    if (link_grid) {
        const int positions[8][2] = {{0,0}, {1,0}, {2,0}, {0,1}, {2,1}, {0,2}, {1,2}, {2,2}};
        for (int j = 0; j < 8; j++) {
            GtkWidget *toggle = gtk_grid_get_child_at(GTK_GRID(link_grid), positions[j][0], positions[j][1]);
            if (toggle && GTK_IS_TOGGLE_BUTTON(toggle)) {
                filter_state.link_marker_toggles[j] = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(toggle));
            }
        }
    }
    
    // 保存文本状态（释放旧值）
    g_free(filter_state.atk_text);
    g_free(filter_state.def_text);
    g_free(filter_state.level_text);
    g_free(filter_state.left_scale_text);
    g_free(filter_state.right_scale_text);
    
    filter_state.atk_text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(atk_row)));
    filter_state.def_text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(def_row)));
    filter_state.level_text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(level_row)));
    filter_state.left_scale_text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(left_scale_entry)));
    filter_state.right_scale_text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(right_scale_entry)));
}

// 筛选按钮回调：打开筛选选项对话框
static void on_filter_button_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    GtkWindow *parent_window = GTK_WINDOW(user_data);
    
    // 检查离线数据是否存在
    if (!offline_data_exists()) {
        // 离线数据不存在，显示提示对话框
        AdwDialog *alert_dialog = ADW_DIALOG(adw_alert_dialog_new("筛选功能", "仅在离线模式生效"));
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert_dialog), "ok", "确定");
        adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert_dialog), "ok");
        adw_dialog_present(alert_dialog, GTK_WIDGET(parent_window));
        return;
    }
    
    // 创建 AdwPreferencesDialog
    AdwDialog *dialog = ADW_DIALOG(adw_preferences_dialog_new());
    adw_dialog_set_title(dialog, "筛选选项");
    
    // 设置对话框内容高度
    adw_dialog_set_content_height(dialog, 600);
    
    // 创建 PreferencesPage
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());
    adw_preferences_page_set_title(page, "卡片筛选");
    adw_preferences_page_set_icon_name(page, "nautilus-search-filters-symbolic");
    
    // === 恢复默认按钮组 ===
    AdwPreferencesGroup *reset_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    
    // 创建按钮
    GtkWidget *reset_button = gtk_button_new_with_label("恢复默认");
    gtk_widget_set_hexpand(reset_button, TRUE);
    gtk_widget_set_halign(reset_button, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(reset_button, "suggested-action");
    
    // 连接按钮点击信号
    g_signal_connect(reset_button, "clicked", G_CALLBACK(on_reset_filter_clicked), dialog);
    
    // 直接添加到 group
    adw_preferences_group_add(reset_group, reset_button);
    
    adw_preferences_page_add(page, reset_group);
    
    // === 卡片类型组（顶层）===
    AdwPreferencesGroup *type_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(type_group, "卡片类型");
    
    // 卡片类型下拉菜单
    AdwComboRow *type_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(type_row), "类型");
    GtkStringList *type_list = gtk_string_list_new(NULL);
    gtk_string_list_append(type_list, "全部");
    gtk_string_list_append(type_list, "怪兽");
    gtk_string_list_append(type_list, "魔法");
    gtk_string_list_append(type_list, "陷阱");
    adw_combo_row_set_model(type_row, G_LIST_MODEL(type_list));
    adw_combo_row_set_selected(type_row, filter_state.card_type_selected);  // 恢复状态
    adw_preferences_group_add(type_group, GTK_WIDGET(type_row));
    
    // 保存引用以便后续保存状态
    g_object_set_data(G_OBJECT(dialog), "type_row", type_row);
    
    adw_preferences_page_add(page, type_group);
    
    // === 怪兽类别组（仅怪兽）===
    AdwPreferencesGroup *monster_type_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(monster_type_group, "怪兽类别");
    
    // 创建包含 toggle buttons 的 ActionRow
    AdwActionRow *type_buttons_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(type_buttons_row), "类别筛选");
    adw_action_row_set_subtitle(type_buttons_row, "选择要包含的怪兽类别");
    
    // 创建 flow box 容纳 toggle buttons
    GtkWidget *flow_box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow_box), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow_box), 5);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow_box), TRUE);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow_box), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow_box), 6);
    gtk_widget_set_margin_top(flow_box, 12);
    gtk_widget_set_margin_bottom(flow_box, 12);
    gtk_widget_set_margin_start(flow_box, 12);
    gtk_widget_set_margin_end(flow_box, 12);
    
    // 定义怪兽类别
    const char *monster_types[] = {
        "通常", "效果", "仪式", "融合", "同调", "超量", 
        "灵摆", "连接", "调整", "灵魂", "同盟", "二重", "反转", 
        "卡通", "特殊召唤"
    };
    
    // 创建 toggle buttons
    for (size_t i = 0; i < sizeof(monster_types) / sizeof(monster_types[0]); i++) {
        GtkWidget *toggle = gtk_toggle_button_new_with_label(monster_types[i]);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), filter_state.monster_type_toggles[i]);  // 恢复状态
        gtk_flow_box_insert(GTK_FLOW_BOX(flow_box), toggle, -1);
    }
    
    // 保存引用
    g_object_set_data(G_OBJECT(dialog), "flow_box", flow_box);
    
    // 创建一个容器来放置 flow box
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), flow_box);
    
    adw_preferences_group_add(monster_type_group, box);
    
    // 初始隐藏怪兽类别组
    gtk_widget_set_visible(GTK_WIDGET(monster_type_group), FALSE);
    adw_preferences_page_add(page, monster_type_group);
    
    // === 魔法类别组（仅魔法）===
    AdwPreferencesGroup *spell_type_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(spell_type_group, "魔法类别");
    
    // 魔法类别下拉菜单
    AdwComboRow *spell_type_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(spell_type_row), "类别");
    GtkStringList *spell_type_list = gtk_string_list_new(NULL);
    gtk_string_list_append(spell_type_list, "全部");
    gtk_string_list_append(spell_type_list, "通常");
    gtk_string_list_append(spell_type_list, "仪式");
    gtk_string_list_append(spell_type_list, "速攻");
    gtk_string_list_append(spell_type_list, "永续");
    gtk_string_list_append(spell_type_list, "装备");
    gtk_string_list_append(spell_type_list, "场地");
    adw_combo_row_set_model(spell_type_row, G_LIST_MODEL(spell_type_list));
    adw_combo_row_set_selected(spell_type_row, filter_state.spell_type_selected);  // 恢复状态
    adw_preferences_group_add(spell_type_group, GTK_WIDGET(spell_type_row));
    
    // 保存引用
    g_object_set_data(G_OBJECT(dialog), "spell_type_row", spell_type_row);
    
    // 初始隐藏魔法类别组
    gtk_widget_set_visible(GTK_WIDGET(spell_type_group), FALSE);
    adw_preferences_page_add(page, spell_type_group);
    
    // === 陷阱类别组（仅陷阱）===
    AdwPreferencesGroup *trap_type_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(trap_type_group, "陷阱类别");
    
    // 陷阱类别下拉菜单
    AdwComboRow *trap_type_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(trap_type_row), "类别");
    GtkStringList *trap_type_list = gtk_string_list_new(NULL);
    gtk_string_list_append(trap_type_list, "全部");
    gtk_string_list_append(trap_type_list, "通常");
    gtk_string_list_append(trap_type_list, "永续");
    gtk_string_list_append(trap_type_list, "反击");
    adw_combo_row_set_model(trap_type_row, G_LIST_MODEL(trap_type_list));
    adw_combo_row_set_selected(trap_type_row, filter_state.trap_type_selected);  // 恢复状态
    adw_preferences_group_add(trap_type_group, GTK_WIDGET(trap_type_row));
    
    // 保存引用
    g_object_set_data(G_OBJECT(dialog), "trap_type_row", trap_type_row);
    
    // 初始隐藏陷阱类别组
    gtk_widget_set_visible(GTK_WIDGET(trap_type_group), FALSE);
    adw_preferences_page_add(page, trap_type_group);
    
    // === 怪兽属性组（仅怪兽）===
    AdwPreferencesGroup *monster_attrs_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(monster_attrs_group, "怪兽属性");
    
    // 属性下拉菜单
    AdwComboRow *attribute_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(attribute_row), "属性");
    GtkStringList *attribute_list = gtk_string_list_new(NULL);
    gtk_string_list_append(attribute_list, "全部");
    gtk_string_list_append(attribute_list, "地");
    gtk_string_list_append(attribute_list, "水");
    gtk_string_list_append(attribute_list, "炎");
    gtk_string_list_append(attribute_list, "风");
    gtk_string_list_append(attribute_list, "光");
    gtk_string_list_append(attribute_list, "暗");
    gtk_string_list_append(attribute_list, "神");
    adw_combo_row_set_model(attribute_row, G_LIST_MODEL(attribute_list));
    adw_combo_row_set_selected(attribute_row, filter_state.attribute_selected);  // 恢复状态
    adw_preferences_group_add(monster_attrs_group, GTK_WIDGET(attribute_row));
    
    // 保存引用
    g_object_set_data(G_OBJECT(dialog), "attribute_row", attribute_row);
    
    // 种族下拉菜单
    AdwComboRow *race_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(race_row), "种族");
    GtkStringList *race_list = gtk_string_list_new(NULL);
    gtk_string_list_append(race_list, "全部");
    gtk_string_list_append(race_list, "战士");
    gtk_string_list_append(race_list, "魔法师");
    gtk_string_list_append(race_list, "天使");
    gtk_string_list_append(race_list, "恶魔");
    gtk_string_list_append(race_list, "不死");
    gtk_string_list_append(race_list, "机械");
    gtk_string_list_append(race_list, "水");
    gtk_string_list_append(race_list, "炎");
    gtk_string_list_append(race_list, "岩石");
    gtk_string_list_append(race_list, "鸟兽");
    gtk_string_list_append(race_list, "植物");
    gtk_string_list_append(race_list, "昆虫");
    gtk_string_list_append(race_list, "雷");
    gtk_string_list_append(race_list, "龙");
    gtk_string_list_append(race_list, "兽");
    gtk_string_list_append(race_list, "兽战士");
    gtk_string_list_append(race_list, "恐龙");
    gtk_string_list_append(race_list, "鱼");
    gtk_string_list_append(race_list, "海龙");
    gtk_string_list_append(race_list, "爬虫类");
    gtk_string_list_append(race_list, "念动力");
    gtk_string_list_append(race_list, "幻神兽");
    gtk_string_list_append(race_list, "创造神");
    gtk_string_list_append(race_list, "幻龙");
    gtk_string_list_append(race_list, "电子界");
    gtk_string_list_append(race_list, "幻想魔");
    adw_combo_row_set_model(race_row, G_LIST_MODEL(race_list));
    adw_combo_row_set_selected(race_row, filter_state.race_selected);  // 恢复状态
    adw_preferences_group_add(monster_attrs_group, GTK_WIDGET(race_row));
    
    // 保存引用
    g_object_set_data(G_OBJECT(dialog), "race_row", race_row);
    
    // 初始隐藏怪兽属性组
    gtk_widget_set_visible(GTK_WIDGET(monster_attrs_group), FALSE);
    adw_preferences_page_add(page, monster_attrs_group);
    
    // === 数值信息组（仅怪兽）===
    AdwPreferencesGroup *stats_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(stats_group, "数值信息");
    
    // 攻击力输入框
    AdwEntryRow *atk_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(atk_row), "攻击力");
    adw_entry_row_set_input_hints(atk_row, GTK_INPUT_HINT_NO_EMOJI);
    gtk_editable_set_text(GTK_EDITABLE(atk_row), filter_state.atk_text ? filter_state.atk_text : "");  // 恢复状态
    gtk_editable_set_max_width_chars(GTK_EDITABLE(atk_row), 10);
    adw_preferences_group_add(stats_group, GTK_WIDGET(atk_row));
    g_object_set_data(G_OBJECT(dialog), "atk_row", atk_row);
    
    // 守备力输入框
    AdwEntryRow *def_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(def_row), "守备力");
    adw_entry_row_set_input_hints(def_row, GTK_INPUT_HINT_NO_EMOJI);
    gtk_editable_set_text(GTK_EDITABLE(def_row), filter_state.def_text ? filter_state.def_text : "");  // 恢复状态
    gtk_editable_set_max_width_chars(GTK_EDITABLE(def_row), 10);
    adw_preferences_group_add(stats_group, GTK_WIDGET(def_row));
    g_object_set_data(G_OBJECT(dialog), "def_row", def_row);
    
    // 等级输入框
    AdwEntryRow *level_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(level_row), "等级");
    adw_entry_row_set_input_hints(level_row, GTK_INPUT_HINT_NO_EMOJI);
    gtk_editable_set_text(GTK_EDITABLE(level_row), filter_state.level_text ? filter_state.level_text : "");  // 恢复状态
    gtk_editable_set_max_width_chars(GTK_EDITABLE(level_row), 10);
    adw_preferences_group_add(stats_group, GTK_WIDGET(level_row));
    g_object_set_data(G_OBJECT(dialog), "level_row", level_row);
    
    // 初始隐藏数值信息组
    gtk_widget_set_visible(GTK_WIDGET(stats_group), FALSE);
    adw_preferences_page_add(page, stats_group);
    
    // === 灵摆刻度组（仅怪兽）===
    AdwPreferencesGroup *pendulum_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(pendulum_group, "灵摆刻度");
    
    // 创建一个 ActionRow 包含两个刻度输入框
    AdwActionRow *scale_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(scale_row), "灵摆刻度");
    adw_action_row_set_subtitle(scale_row, "左刻度 / 右刻度");
    
    // 左刻度输入框
    GtkWidget *left_scale_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(left_scale_entry), "左");
    gtk_editable_set_text(GTK_EDITABLE(left_scale_entry), filter_state.left_scale_text ? filter_state.left_scale_text : "");  // 恢复状态
    gtk_editable_set_max_width_chars(GTK_EDITABLE(left_scale_entry), 5);
    gtk_widget_set_valign(left_scale_entry, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(left_scale_entry, 60, -1);
    g_object_set_data(G_OBJECT(dialog), "left_scale_entry", left_scale_entry);
    
    // 右刻度输入框
    GtkWidget *right_scale_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(right_scale_entry), "右");
    gtk_editable_set_text(GTK_EDITABLE(right_scale_entry), filter_state.right_scale_text ? filter_state.right_scale_text : "");  // 恢复状态
    gtk_editable_set_max_width_chars(GTK_EDITABLE(right_scale_entry), 5);
    gtk_widget_set_valign(right_scale_entry, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(right_scale_entry, 60, -1);
    g_object_set_data(G_OBJECT(dialog), "right_scale_entry", right_scale_entry);
    
    // 将输入框添加为后缀控件
    adw_action_row_add_suffix(scale_row, left_scale_entry);
    adw_action_row_add_suffix(scale_row, right_scale_entry);
    
    adw_preferences_group_add(pendulum_group, GTK_WIDGET(scale_row));
    
    // 初始隐藏灵摆刻度组
    gtk_widget_set_visible(GTK_WIDGET(pendulum_group), FALSE);
    adw_preferences_page_add(page, pendulum_group);
    
    // === 连接箭头组（仅连接怪兽）===
    AdwPreferencesGroup *link_markers_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(link_markers_group, "连接箭头");
    
    // 创建3x3网格布局（中间位置留空）
    GtkWidget *link_grid = gtk_grid_new();
    gtk_grid_set_row_homogeneous(GTK_GRID(link_grid), TRUE);
    gtk_grid_set_column_homogeneous(GTK_GRID(link_grid), TRUE);
    gtk_grid_set_row_spacing(GTK_GRID(link_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(link_grid), 6);
    gtk_widget_set_margin_top(link_grid, 12);
    gtk_widget_set_margin_bottom(link_grid, 12);
    gtk_widget_set_margin_start(link_grid, 12);
    gtk_widget_set_margin_end(link_grid, 12);
    gtk_widget_set_halign(link_grid, GTK_ALIGN_CENTER);
    
    // 8个箭头按钮（按照grid位置：↖ ↑ ↗ ← → ↙ ↓ ↘）
    const char *arrows[] = {"↖", "↑", "↗", "←", "→", "↙", "↓", "↘"};
    const int positions[8][2] = {{0,0}, {1,0}, {2,0}, {0,1}, {2,1}, {0,2}, {1,2}, {2,2}};
    
    for (int i = 0; i < 8; i++) {
        GtkWidget *toggle = gtk_toggle_button_new_with_label(arrows[i]);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), filter_state.link_marker_toggles[i]);
        gtk_widget_set_size_request(toggle, 50, 50);
        gtk_grid_attach(GTK_GRID(link_grid), toggle, positions[i][0], positions[i][1], 1, 1);
    }
    
    // 保存引用
    g_object_set_data(G_OBJECT(dialog), "link_grid", link_grid);
    
    adw_preferences_group_add(link_markers_group, link_grid);
    
    // 初始隐藏连接箭头组
    gtk_widget_set_visible(GTK_WIDGET(link_markers_group), FALSE);
    adw_preferences_page_add(page, link_markers_group);
    
    // 添加页面到对话框
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), page);
    
    // 连接类型选择变化信号
    GtkWidget **groups = g_new(GtkWidget*, 7);
    groups[0] = GTK_WIDGET(monster_type_group);
    groups[1] = GTK_WIDGET(monster_attrs_group);
    groups[2] = GTK_WIDGET(stats_group);
    groups[3] = GTK_WIDGET(pendulum_group);
    groups[4] = GTK_WIDGET(link_markers_group);
    groups[5] = GTK_WIDGET(spell_type_group);
    groups[6] = GTK_WIDGET(trap_type_group);
    g_signal_connect_data(type_row, "notify::selected", 
                         G_CALLBACK(on_card_type_changed), 
                         groups, 
                         (GClosureNotify)g_free, 
                         0);
    
    // 触发一次类型变化以设置正确的可见性
    on_card_type_changed(type_row, NULL, groups);
    
    // 监听"连接"toggle按钮的状态变化（索引7对应"连接"）
    GtkWidget *link_toggle = NULL;
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(flow_box));
    int i = 0;
    while (child && i <= 7) {
        if (i == 7) {
            link_toggle = gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(child));
            break;
        }
        child = gtk_widget_get_next_sibling(child);
        i++;
    }
    
    if (link_toggle && GTK_IS_TOGGLE_BUTTON(link_toggle)) {
        g_signal_connect(link_toggle, "toggled", G_CALLBACK(on_link_toggle_changed), link_markers_group);
        // 触发一次以设置初始状态
        on_link_toggle_changed(GTK_TOGGLE_BUTTON(link_toggle), link_markers_group);
    }
    
    // 监听"灵摆"toggle按钮的状态变化（索引6对应"灵摆"）
    GtkWidget *pendulum_toggle = NULL;
    child = gtk_widget_get_first_child(GTK_WIDGET(flow_box));
    i = 0;
    while (child && i <= 6) {
        if (i == 6) {
            pendulum_toggle = gtk_flow_box_child_get_child(GTK_FLOW_BOX_CHILD(child));
            break;
        }
        child = gtk_widget_get_next_sibling(child);
        i++;
    }
    
    if (pendulum_toggle && GTK_IS_TOGGLE_BUTTON(pendulum_toggle)) {
        g_signal_connect(pendulum_toggle, "toggled", G_CALLBACK(on_link_toggle_changed), pendulum_group);
        // 触发一次以设置初始状态
        on_link_toggle_changed(GTK_TOGGLE_BUTTON(pendulum_toggle), pendulum_group);
    }
    
    // 连接对话框关闭信号以保存状态
    g_signal_connect(dialog, "closed", G_CALLBACK(on_filter_dialog_closed), NULL);
    
    // 显示对话框
    adw_dialog_present(dialog, GTK_WIDGET(parent_window));
}

// GAction 激活回调：从菜单中选择“从文件...”时触发，复用现有的导入处理逻辑
static void on_action_import_from_file(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    SearchUI *ui = (SearchUI*)user_data;
    // 复用现有的按钮回调实现（传入 NULL 作为 GtkButton）
    on_import_clicked(NULL, ui);
}

// 用于导出URL对话框的数据结构
typedef struct {
    GtkWidget *url_label;
    AdwDialog *dialog;
    AdwToastOverlay *toast_overlay;
} ExportUrlDialogData;

// 用于导入URL对话框的数据结构
typedef struct {
    GtkWidget *url_entry;
    AdwDialog *dialog;
    SearchUI *ui;
} ImportUrlDialogData;

// 辅助函数：为槽位加载卡片图片
static void load_card_image(GtkWidget *slot, int img_id, SoupSession *session) {
    if (img_id <= 0 || !session || !slot) return;
    
    // 检查是否是先行卡
    JsonObject *prerelease_card = find_prerelease_card_by_id(img_id);
    gboolean is_prerelease = (prerelease_card != NULL);
    if (prerelease_card) {
        json_object_unref(prerelease_card);
    }
    
    if (is_prerelease) {
        // 先行卡：从本地加载
        gchar *local_path = get_prerelease_card_image_path(img_id);
        if (local_path && g_file_test(local_path, G_FILE_TEST_EXISTS)) {
            GError *error = NULL;
            GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(local_path, &error);
            if (pixbuf) {
                slot_set_pixbuf(slot, pixbuf);
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
        // 普通卡：从缓存或在线加载
        GdkPixbuf *cached = get_thumb_from_cache(img_id);
        if (!cached) {
            cached = load_from_disk_cache(img_id);
        }
        
        if (cached) {
            // 缓存命中，直接设置
            slot_set_pixbuf(slot, cached);
            if (get_thumb_from_cache(img_id) == NULL) {
                g_object_unref(cached);
            }
        } else {
            // 缓存未命中，异步加载
            char url[128];
            g_snprintf(url, sizeof url, "https://cdn.233.momobako.com/ygoimg/jp/%d.webp", img_id);
            ImageLoadCtx *ctx = g_new0(ImageLoadCtx, 1);
            ctx->stack = NULL;
            ctx->target = slot;
            g_object_add_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
            ctx->scale_to_thumb = TRUE;
            ctx->cache_id = img_id;
            ctx->add_to_thumb_cache = TRUE;
            ctx->url = g_strdup(url);
            load_image_async(session, url, ctx);
        }
    }
}

// 前向声明
static void on_import_url_clicked(GtkButton *btn, gpointer user_data);

// 剪贴板读取完成回调：自动填充URL到输入框
static void on_clipboard_text_ready(GObject *source, GAsyncResult *result, gpointer user_data) {
    GtkWidget *entry = GTK_WIDGET(user_data);
    GError *error = NULL;
    char *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source), result, &error);
    
    if (text && !error) {
        // 检查是否符合YGO-DA协议格式
        // 根据协议文档，判断文本中是否含有"ygotype=deck"参数
        if (strstr(text, "ygotype=deck")) {
            // 从该参数开始往文本前面查找，依次查找是否含有协议头
            const char *param_pos = strstr(text, "ygotype=deck");
            const char *search_start = text;
            const char *url_start = NULL;
            
            // 向前查找协议头
            for (const char *p = param_pos; p >= search_start; p--) {
                if (strncmp(p, "ygo://deck", 10) == 0 || 
                    strncmp(p, "http://", 7) == 0 || 
                    strncmp(p, "https://", 8) == 0) {
                    url_start = p;
                    break;
                }
            }
            
            if (url_start) {
                // 提取URL（到文本末尾或空白字符）
                const char *url_end = url_start;
                while (*url_end && !g_ascii_isspace(*url_end)) {
                    url_end++;
                }
                
                gchar *url = g_strndup(url_start, url_end - url_start);
                gtk_editable_set_text(GTK_EDITABLE(entry), url);
                g_free(url);
            }
        }
        g_free(text);
    } else if (error) {
        g_error_free(error);
    }
}

// GAction 激活回调：从菜单中选择"从URL..."时触发，显示 URL 导入对话框
static void on_action_import_from_url(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    SearchUI *ui = (SearchUI*)user_data;
    
    if (!ui || !ui->window) return;
    
    // 创建 AdwDialog
    AdwDialog *dialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_title(dialog, "从URL导入...");
    adw_dialog_set_content_width(dialog, 400);
    
    // 创建对话框内容容器
    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(content_box, 24);
    gtk_widget_set_margin_end(content_box, 24);
    gtk_widget_set_margin_top(content_box, 24);
    gtk_widget_set_margin_bottom(content_box, 24);

    // 添加更大字号并居中的 Heading 标签
    GtkWidget *heading = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(heading), "<span size='xx-large' weight='bold'>从URL导入...</span>");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.5); // 居中
    gtk_widget_set_halign(heading, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(heading, "heading");
    gtk_box_append(GTK_BOX(content_box), heading);

    // 添加 URL 输入框
    GtkWidget *url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(url_entry),
        "example: http://deck.ourygo.top?ygotype=deck&v=1&d=abcd1234");
    gtk_widget_set_hexpand(url_entry, TRUE);
    gtk_widget_set_margin_top(url_entry, 18); // 统一为18px，与输入框和按钮区一致
    gtk_box_append(GTK_BOX(content_box), url_entry);

    // 居中排列的按钮区，按钮间留空
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 36);
    gtk_widget_set_margin_top(button_box, 18);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);

    GtkWidget *import_btn = gtk_button_new_with_label("导入");
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *cancel_btn = gtk_button_new_with_label("取消");
    gtk_box_append(GTK_BOX(button_box), import_btn);
    gtk_box_append(GTK_BOX(button_box), spacer);
    gtk_box_append(GTK_BOX(button_box), cancel_btn);

    gtk_box_append(GTK_BOX(content_box), button_box);

    // 创建对话框数据并连接导入按钮信号
    ImportUrlDialogData *dialog_data = g_new(ImportUrlDialogData, 1);
    dialog_data->url_entry = url_entry;
    dialog_data->dialog = dialog;
    dialog_data->ui = ui;
    
    g_signal_connect(import_btn, "clicked", G_CALLBACK(on_import_url_clicked), dialog_data);
    g_signal_connect_swapped(cancel_btn, "clicked", G_CALLBACK(adw_dialog_close), dialog);
    
    // 当对话框关闭时释放数据
    g_object_set_data_full(G_OBJECT(dialog), "dialog-data", dialog_data, g_free);

    // 尝试从剪贴板读取URL
    GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_read_text_async(clipboard, NULL, on_clipboard_text_ready, url_entry);

    // 将内容设置为对话框的子控件
    adw_dialog_set_child(dialog, content_box);

    // 显示对话框
    adw_dialog_present(dialog, GTK_WIDGET(ui->window));
}

// GAction 激活回调：从菜单中选择"到文件"时触发，复用现有导出逻辑
static void on_action_export_to_file(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    SearchUI *ui = (SearchUI*)user_data;
    // 复用现有的按钮回调实现（传入 NULL 作为 GtkButton）
    on_export_clicked(NULL, ui);
}

// 复制按钮回调：将URL标签中的文本复制到剪贴板
static void on_copy_url_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ExportUrlDialogData *data = (ExportUrlDialogData*)user_data;
    
    if (!data || !data->url_label) return;
    
    // 获取URL标签的文本
    const char *url_text = gtk_label_get_text(GTK_LABEL(data->url_label));
    
    // 如果文本为空或是占位符，则不复制
    if (!url_text || *url_text == '\0' || g_strcmp0(url_text, "(URL 将在此显示)") == 0) {
        return;
    }
    
    // 获取剪贴板并设置文本
    GdkClipboard *clipboard = gdk_display_get_clipboard(gdk_display_get_default());
    gdk_clipboard_set_text(clipboard, url_text);
    
    g_print("已复制URL到剪贴板: %s\n", url_text);
    
    // 显示Toast通知
    if (data->toast_overlay) {
        AdwToast *toast = adw_toast_new("已复制到剪贴板");
        adw_toast_set_timeout(toast, 2);
        adw_toast_overlay_add_toast(data->toast_overlay, toast);
    }
}

// 导入URL按钮回调：从LURL解码并导入卡组
static void on_import_url_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    ImportUrlDialogData *data = (ImportUrlDialogData*)user_data;
    
    if (!data || !data->url_entry || !data->ui) return;
    
    const char *url_text = gtk_editable_get_text(GTK_EDITABLE(data->url_entry));
    
    if (!url_text || *url_text == '\0') {
        g_warning("请输入URL");
        return;
    }
    
    // 解码URL
    int *main_cards = NULL, *extra_cards = NULL, *side_cards = NULL;
    int main_count = 0, extra_count = 0, side_count = 0;
    GError *error = NULL;
    
    gboolean success = deck_decode_from_url(
        url_text,
        &main_cards, &main_count,
        &extra_cards, &extra_count,
        &side_cards, &side_count,
        &error
    );
    
    if (!success) {
        g_warning("从URL导入失败: %s", error ? error->message : "未知错误");
        
        // 显示错误Toast通知
        SearchUI *ui = data->ui;
        if (ui->toast_overlay) {
            AdwToast *toast = adw_toast_new("URL错误");
            adw_toast_set_timeout(toast, 2);
            adw_toast_overlay_add_toast(ui->toast_overlay, toast);
        }
        
        if (error) g_error_free(error);
        return;
    }
    
    SearchUI *ui = data->ui;
    
    // 清空当前卡组
    clear_all_deck_regions(
        ui->main_pics, &ui->main_idx, ui->main_count,
        ui->extra_pics, &ui->extra_idx, ui->extra_count,
        ui->side_pics, &ui->side_idx, ui->side_count
    );
    
    // 导入主卡组
    for (int i = 0; i < main_count && ui->main_idx < (int)ui->main_pics->len; i++) {
        GtkWidget *slot = GTK_WIDGET(g_ptr_array_index(ui->main_pics, ui->main_idx));
        if (!slot) continue;
        
        int img_id = main_cards[i];
        g_object_set_data(G_OBJECT(slot), "img_id", GINT_TO_POINTER(img_id));
        g_object_set_data(G_OBJECT(slot), "card_id", GINT_TO_POINTER(img_id));
        slot_set_is_extra(slot, FALSE);
        
        // 加载卡片图片
        if (img_id > 0 && ui->session) {
            load_card_image(slot, img_id, ui->session);
        }
        
        ui->main_idx++;
    }
    
    // 导入额外卡组
    for (int i = 0; i < extra_count && ui->extra_idx < (int)ui->extra_pics->len; i++) {
        GtkWidget *slot = GTK_WIDGET(g_ptr_array_index(ui->extra_pics, ui->extra_idx));
        if (!slot) continue;
        
        int img_id = extra_cards[i];
        g_object_set_data(G_OBJECT(slot), "img_id", GINT_TO_POINTER(img_id));
        g_object_set_data(G_OBJECT(slot), "card_id", GINT_TO_POINTER(img_id));
        slot_set_is_extra(slot, TRUE);
        
        // 加载卡片图片
        if (img_id > 0 && ui->session) {
            load_card_image(slot, img_id, ui->session);
        }
        
        ui->extra_idx++;
    }
    
    // 导入副卡组
    for (int i = 0; i < side_count && ui->side_idx < (int)ui->side_pics->len; i++) {
        GtkWidget *slot = GTK_WIDGET(g_ptr_array_index(ui->side_pics, ui->side_idx));
        if (!slot) continue;
        
        int img_id = side_cards[i];
        g_object_set_data(G_OBJECT(slot), "img_id", GINT_TO_POINTER(img_id));
        g_object_set_data(G_OBJECT(slot), "card_id", GINT_TO_POINTER(img_id));
        slot_set_is_extra(slot, FALSE);
        
        // 加载卡片图片
        if (img_id > 0 && ui->session) {
            load_card_image(slot, img_id, ui->session);
        }
        
        ui->side_idx++;
    }
    
    // 更新计数标签
    update_count_label(ui->main_count, ui->main_idx);
    update_count_label(ui->extra_count, ui->extra_idx);
    update_count_label(ui->side_count, ui->side_idx);
    
    // 清理
    g_free(main_cards);
    g_free(extra_cards);
    g_free(side_cards);
    
    // 关闭对话框
    adw_dialog_close(data->dialog);
    
    // 显示Toast通知
    if (ui->toast_overlay) {
        AdwToast *toast = adw_toast_new("从URL导入成功");
        adw_toast_set_timeout(toast, 2);
        adw_toast_overlay_add_toast(ui->toast_overlay, toast);
    }
    
    g_print("从URL导入成功: 主%d 额外%d 副%d\n", main_count, extra_count, side_count);
}

// GAction 激活回调：从菜单中选择"到URL..."时触发，显示一个独立的导出 URL 对话框
static void on_action_export_to_url(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    SearchUI *ui = (SearchUI*)user_data;

    if (!ui || !ui->window) return;

    // 从UI中提取当前卡组数据
    int *main_cards = NULL;
    int *extra_cards = NULL;
    int *side_cards = NULL;
    int main_count = 0;
    int extra_count = 0;
    int side_count = 0;

    // 提取主卡组
    if (ui->main_pics && ui->main_idx > 0) {
        main_cards = g_new(int, ui->main_idx);
        for (int i = 0; i < ui->main_idx; i++) {
            GtkWidget *slot = GTK_WIDGET(g_ptr_array_index(ui->main_pics, i));
            int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(slot), "img_id"));
            if (img_id > 0) {
                main_cards[main_count++] = img_id;
            }
        }
    }

    // 提取额外卡组
    if (ui->extra_pics && ui->extra_idx > 0) {
        extra_cards = g_new(int, ui->extra_idx);
        for (int i = 0; i < ui->extra_idx; i++) {
            GtkWidget *slot = GTK_WIDGET(g_ptr_array_index(ui->extra_pics, i));
            int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(slot), "img_id"));
            if (img_id > 0) {
                extra_cards[extra_count++] = img_id;
            }
        }
    }

    // 提取副卡组
    if (ui->side_pics && ui->side_idx > 0) {
        side_cards = g_new(int, ui->side_idx);
        for (int i = 0; i < ui->side_idx; i++) {
            GtkWidget *slot = GTK_WIDGET(g_ptr_array_index(ui->side_pics, i));
            int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(slot), "img_id"));
            if (img_id > 0) {
                side_cards[side_count++] = img_id;
            }
        }
    }

    // 编码为URL
    char *deck_url = deck_encode_to_url(
        main_cards, main_count,
        extra_cards, extra_count,
        side_cards, side_count,
        "http://deck.ourygo.top"
    );

    // 清理临时数组
    g_free(main_cards);
    g_free(extra_cards);
    g_free(side_cards);

    // 创建对话框
    AdwDialog *dialog = ADW_DIALOG(adw_dialog_new());
    adw_dialog_set_title(dialog, "导出到URL...");
    adw_dialog_set_content_width(dialog, 380);

    GtkWidget *content_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(content_box, 20);
    gtk_widget_set_margin_end(content_box, 20);
    gtk_widget_set_margin_top(content_box, 20);
    gtk_widget_set_margin_bottom(content_box, 20);

    GtkWidget *heading = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(heading), "<span size='large' weight='bold'>导出到URL...</span>");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.5);
    gtk_widget_set_halign(heading, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(heading, "heading");
    gtk_box_append(GTK_BOX(content_box), heading);

    // URL显示容器
    GtkWidget *url_placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(url_placeholder, TRUE);
    gtk_widget_set_margin_top(url_placeholder, 18);
    
    // 显示生成的URL或错误信息
    GtkWidget *url_label;
    if (deck_url) {
        url_label = gtk_label_new(deck_url);
        gtk_label_set_wrap(GTK_LABEL(url_label), TRUE);
        gtk_label_set_wrap_mode(GTK_LABEL(url_label), PANGO_WRAP_CHAR);
        gtk_label_set_selectable(GTK_LABEL(url_label), TRUE);
        // 当对话框关闭时释放URL字符串
        g_object_set_data_full(G_OBJECT(dialog), "deck-url", deck_url, g_free);
    } else {
        url_label = gtk_label_new("(生成URL失败)");
        gtk_widget_add_css_class(url_label, "dim-label");
    }
    
    gtk_label_set_xalign(GTK_LABEL(url_label), 0.5);
    gtk_widget_set_halign(url_label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(url_placeholder), url_label);
    gtk_box_append(GTK_BOX(content_box), url_placeholder);

    // 按钮（居中）
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 36);
    gtk_widget_set_margin_top(button_box, 18);
    gtk_widget_set_halign(button_box, GTK_ALIGN_CENTER);
    GtkWidget *export_btn = gtk_button_new_with_label("复制");
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    GtkWidget *cancel_btn = gtk_button_new_with_label("取消");
    gtk_box_append(GTK_BOX(button_box), export_btn);
    gtk_box_append(GTK_BOX(button_box), spacer);
    gtk_box_append(GTK_BOX(button_box), cancel_btn);
    gtk_box_append(GTK_BOX(content_box), button_box);

    // 创建对话框数据并连接复制按钮信号
    ExportUrlDialogData *dialog_data = g_new(ExportUrlDialogData, 1);
    dialog_data->url_label = url_label;
    dialog_data->dialog = dialog;
    dialog_data->toast_overlay = ui->toast_overlay;
    
    g_signal_connect(export_btn, "clicked", G_CALLBACK(on_copy_url_clicked), dialog_data);
    g_signal_connect_swapped(cancel_btn, "clicked", G_CALLBACK(adw_dialog_close), dialog);
    
    // 当对话框关闭时释放数据
    g_object_set_data_full(G_OBJECT(dialog), "dialog-data", dialog_data, g_free);

    adw_dialog_set_child(dialog, content_box);
    adw_dialog_present(dialog, GTK_WIDGET(ui->window));
}

// 整理按钮回调：对main、extra、side三个区域分别排序
static void on_sort_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SearchUI *ui = (SearchUI*)user_data;
    
    if (!ui) return;
    
    // 对Main区域排序
    if (ui->main_pics && ui->main_idx > 0) {
        sort_deck_region(ui->main_pics, &ui->main_idx, ui->main_count);
    }
    
    // 对Extra区域排序（按融合-同调-超量-连接，同类按level降序）
    if (ui->extra_pics && ui->extra_idx > 0) {
        sort_extra_region(ui->extra_pics, &ui->extra_idx, ui->extra_count);
    }
    
    // 对Side区域排序
    if (ui->side_pics && ui->side_idx > 0) {
        sort_deck_region(ui->side_pics, &ui->side_idx, ui->side_count);
    }
}

// 打乱按钮回调：只打乱Main区域
static void on_shuffle_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SearchUI *ui = (SearchUI*)user_data;
    
    if (!ui) return;
    
    // 只对Main区域打乱
    if (ui->main_pics && ui->main_idx > 0) {
        shuffle_deck_region(ui->main_pics, &ui->main_idx, ui->main_count);
    }
}

// 清空确认对话框的响应回调
static void on_clear_dialog_response(AdwAlertDialog *dialog, const char *response, gpointer user_data) {
    (void)dialog;
    SearchUI *ui = (SearchUI*)user_data;
    
    if (g_strcmp0(response, "clear") == 0) {
        // 用户确认清空
        clear_all_deck_regions(
            ui->main_pics, &ui->main_idx, ui->main_count,
            ui->extra_pics, &ui->extra_idx, ui->extra_count,
            ui->side_pics, &ui->side_idx, ui->side_count
        );
    }
    // 如果是 "cancel"，则不做任何操作
}

// 清空按钮回调：显示确认对话框
static void on_clear_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SearchUI *ui = (SearchUI*)user_data;
    
    if (!ui) return;
    
    // 创建确认对话框
    AdwDialog *dialog = adw_alert_dialog_new("确认清空", "确定要清空所有卡组区域（Main、Extra、Side）吗？此操作无法撤销。");
    
    // 添加取消和确认按钮
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "cancel", "取消");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "clear", "清空");
    
    // 设置清空按钮为破坏性样式（红色）
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "clear", ADW_RESPONSE_DESTRUCTIVE);
    
    // 连接响应信号
    g_signal_connect(dialog, "response", G_CALLBACK(on_clear_dialog_response), ui);
    
    // 显示对话框，需要获取顶层窗口
    GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(btn)));
    adw_dialog_present(dialog, toplevel);
}

// 导出按钮回调：显示文件保存对话框
static void on_export_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SearchUI *ui = (SearchUI*)user_data;
    
    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "导出卡组");
    
    // 设置默认文件名和目录
    if (last_export_directory && g_file_test(last_export_directory, G_FILE_TEST_IS_DIR)) {
        // 使用缓存的目录
        char *full_path = g_build_filename(last_export_directory, "deck.ydk", NULL);
        GFile *initial_file = g_file_new_for_path(full_path);
        gtk_file_dialog_set_initial_file(dialog, initial_file);
        g_object_unref(initial_file);
        g_free(full_path);
    } else {
        // 使用默认文件名（当前目录）
        GFile *initial_file = g_file_new_for_path("deck.ydk");
        gtk_file_dialog_set_initial_file(dialog, initial_file);
        g_object_unref(initial_file);
    }
    
    // 设置文件过滤器
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_add_pattern(filter, "*.ydk");
    gtk_file_filter_set_name(filter, "YDK文件 (*.ydk)");
    
    GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
    g_list_store_append(filters, filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters));
    g_object_unref(filters);
    g_object_unref(filter);
    
    ExportData *data = g_new0(ExportData, 1);
    data->ui = ui;
    data->dialog = g_object_ref(dialog);
    
    gtk_file_dialog_save(dialog, 
                        NULL, 
                        NULL,
                        on_export_file_save_finish,
                        data);
}

char* format_card_text(const CardPreview *pv) {
    GString *s = g_string_new(NULL);
    char *name = pv->cn_name ? g_markup_escape_text(pv->cn_name, -1) : g_strdup("(无名)");
    char *types = pv->types ? g_markup_escape_text(pv->types, -1) : g_strdup("");
    char *pdesc = pv->pdesc ? g_markup_escape_text(pv->pdesc, -1) : g_strdup("");
    char *desc  = pv->desc  ? g_markup_escape_text(pv->desc, -1)  : g_strdup("");
    g_string_append_printf(s, "<b>%s</b>\n", name);
    if (*types) g_string_append_printf(s, "<span size=\"smaller\" color=\"#888\">%s</span>\n\n", types);
    if (*pdesc) g_string_append_printf(s, "%s\n\n", pdesc);
    if (*desc)  g_string_append_printf(s, "%s", desc);
    g_free(name); g_free(types); g_free(pdesc); g_free(desc);
    return g_string_free(s, FALSE);
}

static void show_card_preview(SearchUI *ui, const CardPreview *pv) {
    if (!ui || !pv) return;
    // 文本
    char *markup = format_card_text(pv);
    gtk_label_set_use_markup(ui->left_label, TRUE);
    gtk_label_set_markup(ui->left_label, markup);
    g_free(markup);
    // 图片
    if (pv->id > 0) {
        if (pv->is_prerelease) {
            // 先行卡：从本地加载图片
            gchar *local_path = get_prerelease_card_image_path(pv->id);
            if (local_path && g_file_test(local_path, G_FILE_TEST_EXISTS)) {
                GError *error = NULL;
                GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(local_path, &error);
                if (pixbuf) {
                    GdkTexture *tex = gdk_texture_new_for_pixbuf(pixbuf);
                    if (tex) {
                        gtk_picture_set_paintable(ui->left_picture, GDK_PAINTABLE(tex));
                        g_object_unref(tex);
                    }
                    g_object_unref(pixbuf);
                    gtk_stack_set_visible_child_name(ui->left_stack, "picture");
                } else {
                    if (error) {
                        g_warning("Failed to load prerelease image: %s", error->message);
                        g_error_free(error);
                    }
                }
                g_free(local_path);
            }
        } else {
            // 普通卡：先尝试从磁盘缓存加载
            GdkPixbuf *cached_pixbuf = load_from_disk_cache(pv->id);
            if (cached_pixbuf) {
                // 从缓存加载成功
                GdkTexture *tex = gdk_texture_new_for_pixbuf(cached_pixbuf);
                if (tex) {
                    gtk_picture_set_paintable(ui->left_picture, GDK_PAINTABLE(tex));
                    g_object_unref(tex);
                }
                g_object_unref(cached_pixbuf);
                gtk_stack_set_visible_child_name(ui->left_stack, "picture");
            } else {
                // 缓存不存在，从在线URL加载
                char url[128];
                g_snprintf(url, sizeof url, "https://cdn.233.momobako.com/ygoimg/jp/%d.webp", pv->id);
                gtk_stack_set_visible_child_name(ui->left_stack, "placeholder");
                if (ui->left_spinner) {
                    gtk_widget_set_visible(GTK_WIDGET(ui->left_spinner), TRUE);
                    gtk_spinner_start(ui->left_spinner);
                }
                ImageLoadCtx *ctx = g_new0(ImageLoadCtx, 1);
                ctx->stack = ui->left_stack;
                // 为 stack 增加弱引用，避免销毁后调用
                if (ctx->stack) g_object_add_weak_pointer(G_OBJECT(ctx->stack), (gpointer*)&ctx->stack);
                ctx->target = GTK_WIDGET(ui->left_picture);
                // 防止目标控件销毁后悬空访问
                g_object_add_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
                ctx->scale_to_thumb = FALSE;
                ctx->cache_id = pv->id;  // 设置cache_id以便下载后保存到缓存
                ctx->url = g_strdup(url);
                load_image_async(ui->session, url, ctx);
            }
        }
    }
}

// 右栏行：记录按下位置用于区分拖拽
void on_result_row_pressed(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press; (void)user_data;
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    double *press_xy = g_new(double, 2);
    press_xy[0] = x;
    press_xy[1] = y;
    g_object_set_data_full(G_OBJECT(row), "press_xy", press_xy, g_free);
}

// 右栏行：在 released 中根据位移判定是否为点击；避免拖拽误触发
void on_result_row_released(GtkGestureClick *gesture, int n_press, double x, double y, gpointer user_data) {
    (void)n_press;
    SearchUI *ui = (SearchUI*)user_data;
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
    double *press_xy = (double*)g_object_get_data(G_OBJECT(row), "press_xy");
    if (press_xy) {
        double dx = fabs(x - press_xy[0]);
        double dy = fabs(y - press_xy[1]);
        const double threshold = 3.0;
        if (dx > threshold || dy > threshold) {
            // 拖拽：不进行点击填充
            g_object_set_data_full(G_OBJECT(row), "press_xy", NULL, NULL);
            return;
        }
    }
    g_object_set_data_full(G_OBJECT(row), "press_xy", NULL, NULL);

    CardPreview *pv = (CardPreview*)g_object_get_data(G_OBJECT(row), "preview");
    if (!pv) return;
    
    // 检查禁限卡限制
    if (pv->cid > 0) {
        int limit = get_card_limit(ui, pv->cid);
        if (limit == 0) return;  // 禁止卡，不能加入
    }
    
    gboolean is_monster = FALSE, is_extra_type = FALSE;
    if (pv->type > 0) {
        if (pv->type & 0x1) is_monster = TRUE;  // TYPE_MONSTER
        if ((pv->type & 0x40) || (pv->type & 0x2000) ||
            (pv->type & 0x800000) || (pv->type & 0x4000000)) {  // FUSION, SYNCHRO, XYZ, LINK
            is_extra_type = TRUE;
        }
    }
    enum { REGION_MAIN, REGION_EXTRA, REGION_SIDE } region = REGION_MAIN;
    if (is_monster && is_extra_type) region = REGION_EXTRA;
    
    // 检查当前数量是否已达到限制
    if (pv->cid > 0) {
        int limit = get_card_limit(ui, pv->cid);
        gboolean is_extra_card = is_monster && is_extra_type;
        int current_count = count_card_in_deck(ui, pv->cid, is_extra_card);
        if (current_count >= limit) return;  // 已达到上限
    }
    
    GtkWidget *target_pic = NULL;
    if (region == REGION_MAIN) {
        if (ui->main_idx < (int)ui->main_pics->len) {
            target_pic = GTK_WIDGET(g_ptr_array_index(ui->main_pics, ui->main_idx));
            ui->main_idx++;
            if (ui->main_count) {
                char buf[16];
                g_snprintf(buf, sizeof buf, "(%d)", ui->main_idx);
                gtk_label_set_text(ui->main_count, buf);
            }
        } else if (ui->side_idx < (int)ui->side_pics->len) {
            target_pic = GTK_WIDGET(g_ptr_array_index(ui->side_pics, ui->side_idx));
            ui->side_idx++;
            if (ui->side_count) {
                char buf[16];
                g_snprintf(buf, sizeof buf, "(%d)", ui->side_idx);
                gtk_label_set_text(ui->side_count, buf);
            }
        }
    } else if (region == REGION_EXTRA) {
        if (ui->extra_idx < (int)ui->extra_pics->len) {
            target_pic = GTK_WIDGET(g_ptr_array_index(ui->extra_pics, ui->extra_idx));
            ui->extra_idx++;
            if (ui->extra_count) {
                char buf[16];
                g_snprintf(buf, sizeof buf, "(%d)", ui->extra_idx);
                gtk_label_set_text(ui->extra_count, buf);
            }
        } else if (ui->side_idx < (int)ui->side_pics->len) {
            target_pic = GTK_WIDGET(g_ptr_array_index(ui->side_pics, ui->side_idx));
            ui->side_idx++;
            if (ui->side_count) {
                char buf[16];
                g_snprintf(buf, sizeof buf, "(%d)", ui->side_idx);
                gtk_label_set_text(ui->side_count, buf);
            }
        }
    }
    if (!target_pic) return;
    gboolean is_extra_card = is_monster && is_extra_type;
    slot_set_is_extra(target_pic, is_extra_card);
    // 存储 card_id 用于统计
    if (pv->cid > 0) {
        g_object_set_data(G_OBJECT(target_pic), "card_id", GINT_TO_POINTER(pv->cid));
    }
    // 存储 img_id 用于导出YDK
    if (pv->id > 0) {
        g_object_set_data(G_OBJECT(target_pic), "img_id", GINT_TO_POINTER(pv->id));
    }
    
    // 加载图片
    if (pv->id > 0) {
        // 优先尝试从右栏行中获取已加载的缩略图（零延迟）
        GdkPixbuf *right_pixbuf = NULL;
        if (GTK_IS_LIST_BOX_ROW(row)) {
            GtkWidget *row_child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
            if (row_child) {
                GtkWidget *thumb_stack = gtk_widget_get_first_child(row_child);
                if (thumb_stack && GTK_IS_STACK(thumb_stack)) {
                    GtkWidget *picture = gtk_stack_get_child_by_name(GTK_STACK(thumb_stack), "picture");
                    if (picture && GTK_IS_DRAWING_AREA(picture)) {
                        GdkPixbuf *pb = slot_get_pixbuf(picture);
                        if (pb) {
                            right_pixbuf = g_object_ref(pb);  // 增加引用计数
                        }
                    }
                }
            }
        }
        
        if (right_pixbuf) {
            // 直接复用右栏的缩略图，立即显示（最快）
            slot_set_pixbuf(target_pic, right_pixbuf);
            g_object_unref(right_pixbuf);  // 释放我们增加的引用
        } else if (pv->is_prerelease) {
            // 先行卡：从本地加载
            gchar *local_path = get_prerelease_card_image_path(pv->id);
            if (local_path && g_file_test(local_path, G_FILE_TEST_EXISTS)) {
                GError *error = NULL;
                GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(local_path, &error);
                if (pixbuf) {
                    slot_set_pixbuf(target_pic, pixbuf);
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
            // 尝试从内存缓存加载
            GdkPixbuf *cached = get_thumb_from_cache(pv->id);
            if (cached) {
                slot_set_pixbuf(target_pic, cached);
            } else {
                // 尝试从磁盘缓存加载
                GdkPixbuf *disk_cached = load_from_disk_cache(pv->id);
                if (disk_cached) {
                    slot_set_pixbuf(target_pic, disk_cached);
                    g_object_unref(disk_cached);
                } else {
                    // 缓存都未命中，从在线加载
                    char url[128];
                    g_snprintf(url, sizeof url, "https://cdn.233.momobako.com/ygoimg/jp/%d.webp", pv->id);
                    ImageLoadCtx *ctx = g_new0(ImageLoadCtx, 1);
                    ctx->stack = NULL;
                    ctx->target = GTK_WIDGET(target_pic);
                    // 防止目标控件销毁后悬空访问
                    g_object_add_weak_pointer(G_OBJECT(ctx->target), (gpointer*)&ctx->target);
                    ctx->scale_to_thumb = TRUE;
                    ctx->cache_id = pv->id;  // 启用缓存检查和保存
                    ctx->add_to_thumb_cache = TRUE;  // 下载后保存到缩略图缓存
                    ctx->url = g_strdup(url);
                    load_image_async(ui->session, url, ctx);
                }
            }
        }
    }
}

void on_result_row_enter(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
    (void)x; (void)y;
    SearchUI *ui = (SearchUI*)user_data;
    GtkWidget *row = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    CardPreview *pv = (CardPreview*)g_object_get_data(G_OBJECT(row), "preview");
    if (pv) show_card_preview(ui, pv);
}

// 中栏卡图悬浮事件：异步回调
static void on_slot_card_info_received(GObject *source, GAsyncResult *res, gpointer user_data) {
    SoupSession *session = SOUP_SESSION(source);
    SearchUI *ui = (SearchUI*)user_data;
    GError *err = NULL;
    GInputStream *in = soup_session_send_finish(session, res, &err);
    if (!in) { 
        if (err) g_error_free(err); 
        return; 
    }
    
    GByteArray *ba = g_byte_array_new();
    guint8 bufread[4096];
    gssize n;
    while ((n = g_input_stream_read(in, bufread, sizeof bufread, NULL, NULL)) > 0) {
        g_byte_array_append(ba, bufread, (guint)n);
    }
    g_object_unref(in);
    
    if (ba->len == 0) {
        g_byte_array_unref(ba);
        return;
    }
    
    JsonParser *parser = json_parser_new();
    GError *jerr = NULL;
    if (json_parser_load_from_data(parser, (const char*)ba->data, (gssize)ba->len, &jerr)) {
        JsonNode *root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            
            // 构造 CardPreview 数据
            CardPreview pv = {0};
            
            // 获取 id (用于图片)
            if (json_object_has_member(obj, "id")) {
                pv.id = json_object_get_int_member(obj, "id");
            }
            
            // 获取 cid (用于禁限卡表)
            if (json_object_has_member(obj, "cid")) {
                pv.cid = json_object_get_int_member(obj, "cid");
            }
            
            // 获取type数据
            if (json_object_has_member(obj, "data")) {
                JsonObject *data = json_object_get_object_member(obj, "data");
                if (data && json_object_has_member(data, "type")) {
                    pv.type = (uint32_t)json_object_get_int_member(data, "type");
                }
            }
            
            // 获取文本数据
            if (json_object_has_member(obj, "text")) {
                JsonObject *text = json_object_get_object_member(obj, "text");
                if (text) {
                    // 获取卡名 (从 text.name 获取)
                    if (json_object_has_member(text, "name")) 
                        pv.cn_name = g_strdup(json_object_get_string_member(text, "name"));
                    if (json_object_has_member(text, "types")) 
                        pv.types = g_strdup(json_object_get_string_member(text, "types"));
                    if (json_object_has_member(text, "pdesc")) 
                        pv.pdesc = g_strdup(json_object_get_string_member(text, "pdesc"));
                    if (json_object_has_member(text, "desc")) 
                        pv.desc = g_strdup(json_object_get_string_member(text, "desc"));
                }
            }
            
            // 显示卡片预览
            show_card_preview(ui, &pv);
            
            // 释放临时字符串
            g_free(pv.cn_name);
            g_free(pv.types);
            g_free(pv.pdesc);
            g_free(pv.desc);
        }
    }
    if (jerr) g_error_free(jerr);
    g_object_unref(parser);
    g_byte_array_unref(ba);
}

// 中栏卡图悬浮事件：获取 img_id 并请求卡片信息
static void on_slot_enter(GtkEventControllerMotion *controller, double x, double y, gpointer user_data) {
    (void)x; (void)y;
    SearchUI *ui = (SearchUI*)user_data;
    GtkWidget *pic = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller));
    
    // 获取 img_id
    int img_id = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(pic), "img_id"));
    if (img_id <= 0) return;  // 没有卡片
    
    // 首先尝试从先行卡中查找
    JsonObject *prerelease_card = find_prerelease_card_by_id(img_id);
    if (prerelease_card) {
        // 从先行卡数据构造 CardPreview
        CardPreview pv = {0};
        pv.id = img_id;
        pv.cid = img_id;  // 先行卡的cid与id相同
        pv.is_prerelease = TRUE;
        
        // 获取type数据
        if (json_object_has_member(prerelease_card, "type")) {
            pv.type = (uint32_t)json_object_get_int_member(prerelease_card, "type");
        }
        
        // 获取文本数据
        if (json_object_has_member(prerelease_card, "text")) {
            JsonObject *text = json_object_get_object_member(prerelease_card, "text");
            if (text) {
                if (json_object_has_member(text, "name")) 
                    pv.cn_name = g_strdup(json_object_get_string_member(text, "name"));
                if (json_object_has_member(text, "types")) 
                    pv.types = g_strdup(json_object_get_string_member(text, "types"));
                if (json_object_has_member(text, "pdesc")) 
                    pv.pdesc = g_strdup(json_object_get_string_member(text, "pdesc"));
                if (json_object_has_member(text, "desc")) 
                    pv.desc = g_strdup(json_object_get_string_member(text, "desc"));
            }
        }
        
        // 显示卡片预览
        show_card_preview(ui, &pv);
        
        // 释放临时字符串
        g_free(pv.cn_name);
        g_free(pv.types);
        g_free(pv.pdesc);
        g_free(pv.desc);
        
        json_object_unref(prerelease_card);
        return;
    }
    
    // 如果不是先行卡，则从在线API获取
    // 构造 API URL
    char url[256];
    g_snprintf(url, sizeof url, "https://ygocdb.com/api/v0/card/%d", img_id);
    
    // 发起异步请求
    SoupMessage *msg = soup_message_new("GET", url);
    if (!msg) return;
    
    soup_session_send_async(ui->session, msg, G_PRIORITY_DEFAULT, NULL, on_slot_card_info_received, ui);
    g_object_unref(msg);
}



// 用于存储下载进度对话框和窗口的数据结构
typedef struct {
    GtkWidget *window;
    AdwDialog *progress_dialog;
    AdwToastOverlay *toast_overlay;
} DownloadDialogData;

// 下载先行卡完成后的回调
static gboolean on_download_prerelease_complete(gpointer user_data) {
    DownloadDialogData *data = (DownloadDialogData*)user_data;
    GtkWidget *window = data->window;
    AdwDialog *progress_dialog = data->progress_dialog;
    
    // 关闭进度对话框
    if (progress_dialog) {
        adw_dialog_close(progress_dialog);
    }
    
    // 显示结果对话框
    if (prerelease_data_exists()) {
        AdwDialog *dialog = adw_alert_dialog_new("下载完成", "先行卡数据已成功下载和处理");
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "确定");
        adw_dialog_present(dialog, window);
        g_message("Pre-release cards downloaded and processed successfully");
    } else {
        AdwDialog *dialog = adw_alert_dialog_new("下载失败", "下载或处理先行卡数据时出错");
        adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "确定");
        adw_dialog_present(dialog, window);
        g_warning("Failed to download pre-release cards");
    }
    
    // 释放数据结构
    g_free(data);
    
    return G_SOURCE_REMOVE;
}

// 下载先行卡 GAction 回调
static void on_download_prerelease_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    GtkWidget *window = GTK_WIDGET(user_data);
    
    g_message("Starting pre-release cards download...");
    
    // 显示提示对话框
    AdwDialog *dialog = adw_alert_dialog_new("正在下载", "正在下载先行卡数据，请稍候...");
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "ok", "确定");
    adw_dialog_present(dialog, window);
    
    // 创建数据结构，传递窗口和对话框引用
    DownloadDialogData *data = g_new(DownloadDialogData, 1);
    data->window = window;
    data->progress_dialog = dialog;
    
    // 启动后台下载
    download_prerelease_cards(on_download_prerelease_complete, data);
}

// 下载离线数据完成后的回调
static gboolean on_download_offline_data_complete(gpointer user_data) {
    DownloadDialogData *data = (DownloadDialogData*)user_data;
    AdwToastOverlay *toast_overlay = data->toast_overlay;
    
    // 显示结果
    if (offline_data_exists()) {
        // 使用 toast 显示成功消息
        if (toast_overlay) {
            AdwToast *toast = adw_toast_new("下载成功");
            adw_toast_set_timeout(toast, 2);
            adw_toast_overlay_add_toast(toast_overlay, toast);
        }
        g_message("Offline data downloaded and extracted successfully");
    } else {
        // 使用 toast 显示失败消息
        if (toast_overlay) {
            AdwToast *toast = adw_toast_new("下载失败");
            adw_toast_set_timeout(toast, 2);
            adw_toast_overlay_add_toast(toast_overlay, toast);
        }
        g_warning("Failed to download offline data");
    }
    
    // 释放数据结构
    g_free(data);
    
    return G_SOURCE_REMOVE;
}

// 离线数据 Switch 状态改变回调
static void on_offline_data_switch_changed(GtkSwitch *switch_widget, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    SearchUI *ui = (SearchUI*)user_data;
    
    if (!ui) return;
    
    gboolean active = gtk_switch_get_active(switch_widget);
    
    if (active && !offline_data_exists()) {
        // 从关闭改为开启：下载离线数据
        g_message("Starting offline data download...");
        
        // 创建数据结构，仅传递 toast_overlay 引用
        DownloadDialogData *data = g_new(DownloadDialogData, 1);
        data->window = NULL;
        data->progress_dialog = NULL;
        data->toast_overlay = ui->toast_overlay;
        
        // 启动后台下载
        download_offline_data(on_download_offline_data_complete, data);
        
        // 保存状态到配置文件
        save_offline_data_switch_state(TRUE);
    } else if (!active && offline_data_exists()) {
        // 从开启改为关闭：清理离线数据
        g_message("Clearing offline data...");
        
        // 重置筛选状态为默认值
        reset_filter_state();
        
        if (clear_offline_data()) {
            // 显示清理成功的 toast
            if (ui->toast_overlay) {
                AdwToast *toast = adw_toast_new("已清理离线数据");
                adw_toast_set_timeout(toast, 2);
                adw_toast_overlay_add_toast(ui->toast_overlay, toast);
            }
            g_message("Offline data cleared successfully");
            
            // 保存状态到配置文件
            save_offline_data_switch_state(FALSE);
        } else {
            // 清理失败，恢复开关状态
            gtk_switch_set_active(switch_widget, TRUE);
            if (ui->toast_overlay) {
                AdwToast *toast = adw_toast_new("清理失败");
                adw_toast_set_timeout(toast, 2);
                adw_toast_overlay_add_toast(ui->toast_overlay, toast);
            }
            g_warning("Failed to clear offline data");
        }
    }
}

// 显示先行卡 GAction 回调
static void on_show_prerelease_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action;
    (void)parameter;
    SearchUI *ui = (SearchUI*)user_data;
    
    if (!ui) return;
    
    // 检查先行卡数据是否存在
    if (!prerelease_data_exists()) {
        g_warning("Pre-release data not found. Please download it first.");
        return;
    }
    
    // 清空当前搜索结果
    list_clear(GTK_LIST_BOX(ui->list));
    
    // 清理旧的图片加载队列和定时器
    if (ui->search_image_loader_id > 0) {
        g_source_remove(ui->search_image_loader_id);
        ui->search_image_loader_id = 0;
    }
    if (ui->search_image_queue) {
        g_ptr_array_free(ui->search_image_queue, TRUE);
        ui->search_image_queue = NULL;
    }
    
    // 获取所有先行卡
    JsonArray *all_cards = get_all_prerelease_cards();
    if (!all_cards) {
        g_warning("Failed to load pre-release cards");
        return;
    }
    
    // 显示所有先行卡
    guint len = json_array_get_length(all_cards);
    for (guint i = 0; i < len; i++) {
        JsonObject *card = json_array_get_object_element(all_cards, i);
        if (card) {
            // 添加先行卡标记
            JsonObject *marked_item = json_object_ref(card);
            json_object_set_boolean_member(marked_item, "is_prerelease", TRUE);
            queue_result_for_render(ui, marked_item);
            json_object_unref(marked_item);
        }
    }
    
    json_array_unref(all_cards);
    
    g_message("Displayed %u pre-release cards", len);
}

static GtkWidget* make_section_header(const char *title_text) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *title = gtk_label_new(NULL);
    char *markup = g_strdup_printf("<span size='large'>%s</span>", title_text);
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_widget_add_css_class(title, "heading");
    gtk_widget_set_margin_start(row, 8);
    gtk_widget_set_margin_end(row, 8);
    gtk_box_append(GTK_BOX(row), title);
    // 右侧副标题 (计数)
    GtkWidget *count = gtk_label_new("(0)");
    gtk_label_set_xalign(GTK_LABEL(count), 0.0);
    gtk_widget_set_halign(count, GTK_ALIGN_START);
    gtk_widget_add_css_class(count, "dim-label");
    gtk_box_append(GTK_BOX(row), count);
    // 保存计数标签以便外部获取
    g_object_set_data(G_OBJECT(box), "count_label", count);
    gtk_box_append(GTK_BOX(box), sep);
    gtk_box_append(GTK_BOX(box), row);
    return box;
}

static void
on_activate(GApplication *app, gpointer user_data)
{
    (void)user_data;
    
    // Initialize image cache system
    init_image_cache();

    AdwApplicationWindow *win = ADW_APPLICATION_WINDOW(
        adw_application_window_new(GTK_APPLICATION(app)));

    // 全局样式：为搜索结果缩略图应用固定 68x99 的最小/最大尺寸
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        ".thumb-fixed {\n"
        "  min-width: 68px; min-height: 99px;\n"
        "}\n"
        ".deck-card {\n"
        "  min-width: 50px; min-height: 73px;\n"
        "}\n"
        ;
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);

    gtk_window_set_title(GTK_WINDOW(win), "Yu-Gi-Oh! Deck Builder");
    gtk_window_set_default_size(GTK_WINDOW(win), 1200, 800);

    AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
    GtkWidget *title_label = gtk_label_new(NULL);
    gtk_label_set_use_markup(GTK_LABEL(title_label), TRUE);
    gtk_label_set_markup(GTK_LABEL(title_label), "<b>Yu-Gi-Oh! Deck Builder</b>");
    adw_header_bar_set_title_widget(header, title_label);

    AdwToolbarView *toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(header));

    GtkWidget *col_start = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *col_mid   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *col_end   = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    gtk_widget_set_hexpand(col_start, TRUE);
    gtk_widget_set_vexpand(col_start, TRUE);
    gtk_widget_set_hexpand(col_mid, TRUE);
    gtk_widget_set_vexpand(col_mid, TRUE);
    // 固定右栏宽度为 300：不扩展，并设置宽度请求（与左栏同机制由分割视图控制，但此处也同步标注）
    gtk_widget_set_hexpand(col_end, FALSE);
    gtk_widget_set_vexpand(col_end, TRUE);
    gtk_widget_set_size_request(col_end, 300, -1);
    // 内层：使用 OverlaySplitView（与左栏相同机制），将右栏作为侧栏并固定 300 宽
    AdwOverlaySplitView *inner = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    // 将右栏作为侧栏，主内容为中栏；同时在中/右之间加入显式分隔线
    GtkWidget *right_with_sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *inner_sep = gtk_separator_new(GTK_ORIENTATION_VERTICAL);
    gtk_widget_set_vexpand(inner_sep, TRUE);
    gtk_box_append(GTK_BOX(right_with_sep), inner_sep);
    gtk_box_append(GTK_BOX(right_with_sep), col_end);
    adw_overlay_split_view_set_sidebar(inner, right_with_sep);
    adw_overlay_split_view_set_content(inner, col_mid);
    // 固定右侧边栏宽度为 300
    adw_overlay_split_view_set_min_sidebar_width(inner, 300);
    adw_overlay_split_view_set_max_sidebar_width(inner, 300);
    // 将内层容器方向设为 RTL，使“起始侧”呈现在右侧；同时保持子控件 LTR
    gtk_widget_set_direction(GTK_WIDGET(inner), GTK_TEXT_DIR_RTL);
    gtk_widget_set_direction(col_mid, GTK_TEXT_DIR_LTR);
    gtk_widget_set_direction(col_end, GTK_TEXT_DIR_LTR);
    // 右栏视觉样式：保留 sidebar 类，移除 background 背景绘制
    gtk_widget_add_css_class(col_end, "sidebar");

    // 外层：左栏-（中+右），左栏可收缩
    AdwOverlaySplitView *outer = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar(outer, col_start);
    adw_overlay_split_view_set_content(outer, GTK_WIDGET(inner));
    // 左栏较窄，且可通过属性（或触控手势）隐藏/显示
    adw_overlay_split_view_set_sidebar_width_fraction(outer, 0.22);
    // 固定左栏宽度为 200，使预览图精确按 200x291 呈现
    adw_overlay_split_view_set_min_sidebar_width(outer, 200);
    adw_overlay_split_view_set_max_sidebar_width(outer, 200);
    adw_overlay_split_view_set_enable_show_gesture(outer, TRUE);
    adw_overlay_split_view_set_enable_hide_gesture(outer, TRUE);

    // 头部按钮：切换左侧边栏显示/隐藏
    GtkWidget *sidebar_toggle = gtk_toggle_button_new();
    GtkWidget *sidebar_icon = gtk_image_new_from_icon_name("view-sidebar-symbolic");
    gtk_button_set_child(GTK_BUTTON(sidebar_toggle), sidebar_icon);
    gtk_widget_set_tooltip_text(sidebar_toggle, "显示/隐藏左侧栏");
    // 双向绑定状态，初始值从 outer 的 show-sidebar 同步到按钮
    g_object_bind_property(outer, "show-sidebar",
                           sidebar_toggle, "active",
                           G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    adw_header_bar_pack_start(header, sidebar_toggle);

    /* 应用菜单按钮：放在标题栏的末端（靠近窗口控制按钮，位于其左侧）
     * 使用 Adwaita 图标集中的 open-menu-symbolic */
    GtkWidget *app_menu_button = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(app_menu_button), "open-menu-symbolic");
    gtk_widget_set_tooltip_text(app_menu_button, "应用菜单");
    /* 创建应用菜单，包含先行卡相关菜单项 */
    GMenu *app_menu = g_menu_new();
    g_menu_append(app_menu, "下载先行卡", "win.download-prerelease");
    g_menu_append(app_menu, "显示先行卡", "win.show-prerelease");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(app_menu_button), G_MENU_MODEL(app_menu));
    g_object_unref(app_menu);

    adw_header_bar_pack_end(header, app_menu_button);

    // 左栏预览：图片(目标尺寸) + 文本（紧贴）
    GtkWidget *left_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(left_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(left_stack), 150);
    gtk_widget_set_valign(left_stack, GTK_ALIGN_START);
    gtk_widget_set_vexpand(left_stack, FALSE);
    GtkWidget *left_placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(left_placeholder, 200, 291);
    gtk_widget_set_valign(left_placeholder, GTK_ALIGN_START);
    gtk_widget_set_halign(left_placeholder, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(left_placeholder, 0);
    GtkWidget *left_spinner = gtk_spinner_new();
    gtk_widget_set_valign(left_spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(left_spinner, GTK_ALIGN_CENTER);
    // 初始不显示、不启动，避免未搜索时出现加载动画
    gtk_widget_set_visible(left_spinner, FALSE);
    gtk_box_append(GTK_BOX(left_placeholder), left_spinner);
    gtk_stack_add_named(GTK_STACK(left_stack), left_placeholder, "placeholder");
    GtkWidget *left_picture = gtk_picture_new();
    gtk_widget_set_size_request(left_picture, 200, 291);
    gtk_picture_set_can_shrink(GTK_PICTURE(left_picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(left_picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_halign(left_picture, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(left_picture, GTK_ALIGN_START);
    gtk_widget_set_margin_bottom(left_picture, 0);
    gtk_stack_add_named(GTK_STACK(left_stack), left_picture, "picture");
    gtk_stack_set_visible_child_name(GTK_STACK(left_stack), "placeholder");
    // 与下方文本之间留 10px 间距
    gtk_widget_set_margin_bottom(left_stack, 10);

    GtkWidget *left_text_scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(left_text_scroller, TRUE);
    gtk_widget_set_hexpand(left_text_scroller, TRUE);
    gtk_widget_set_margin_top(left_text_scroller, 0);
    // 性能优化:启用kinetic scrolling
    gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(left_text_scroller), TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(left_text_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    // 顶部对齐容器，防止内容在视口中垂直居中
    GtkWidget *left_text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(left_text_box, GTK_ALIGN_START);
    gtk_widget_set_halign(left_text_box, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(left_text_box, TRUE);
    gtk_widget_set_vexpand(left_text_box, FALSE);
    GtkWidget *left_label = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(left_label), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(left_label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_xalign(GTK_LABEL(left_label), 0.0);
    // 显式设置标签顶对齐
    gtk_label_set_yalign(GTK_LABEL(left_label), 0.0);
    gtk_widget_set_valign(left_label, GTK_ALIGN_START);
    gtk_widget_set_vexpand(left_label, FALSE);
    // 固定文字行宽为 180：在左栏宽 200 下，左右各留 10px 边距
    gtk_widget_set_margin_start(left_label, 10);
    gtk_widget_set_margin_end(left_label, 10);
    gtk_label_set_selectable(GTK_LABEL(left_label), TRUE);
    gtk_box_append(GTK_BOX(left_text_box), left_label);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(left_text_scroller), left_text_box);

    gtk_box_append(GTK_BOX(col_start), left_stack);
    gtk_box_append(GTK_BOX(col_start), left_text_scroller);

    // 中栏：分成上、中、下三个区域
    // 按钮工具栏区域
    GtkWidget *toolbar_section = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_vexpand(toolbar_section, FALSE);
    gtk_widget_set_margin_start(toolbar_section, 8);
    gtk_widget_set_margin_end(toolbar_section, 8);
    gtk_widget_set_margin_top(toolbar_section, 8);
    gtk_widget_set_margin_bottom(toolbar_section, 4);
    
    // 添加五个按钮
    // 将导入按钮改为菜单按钮（GtkMenuButton），菜单项使用 GMenu + GAction
    GtkWidget *import_button = gtk_menu_button_new();
    GtkWidget *import_label = gtk_label_new("导入");
    // 增加左右内边距，使视觉上与普通 GtkButton 保持一致
    gtk_widget_set_margin_start(import_label, 6);
    gtk_widget_set_margin_end(import_label, 6);
    gtk_widget_set_margin_top(import_label, 2);
    gtk_widget_set_margin_bottom(import_label, 2);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(import_button), import_label);
    // 创建 GMenu 并将菜单模型关联到菜单按钮
    GMenu *import_menu = g_menu_new();
    g_menu_append(import_menu, "从文件...", "win.import-from-file");
    // 占位菜单项（UI 审查）：从 URL...，不执行实际功能
    g_menu_append(import_menu, "从URL...", "win.import-from-url");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(import_button), G_MENU_MODEL(import_menu));
    g_object_unref(import_menu);
    GtkWidget *sort_button = gtk_button_new_with_label("整理");
    GtkWidget *shuffle_button = gtk_button_new_with_label("打乱");
    GtkWidget *clear_button = gtk_button_new_with_label("清空");
    GtkWidget *export_button = gtk_menu_button_new();
    GtkWidget *export_label = gtk_label_new("导出");
    gtk_widget_set_margin_start(export_label, 6);
    gtk_widget_set_margin_end(export_label, 6);
    gtk_widget_set_margin_top(export_label, 2);
    gtk_widget_set_margin_bottom(export_label, 2);
    gtk_menu_button_set_child(GTK_MENU_BUTTON(export_button), export_label);
    GMenu *export_menu = g_menu_new();
    g_menu_append(export_menu, "到文件...", "win.export-to-file");
    // 占位菜单项（UI 审查）：到 URL...，不执行实际功能
    g_menu_append(export_menu, "到URL...", "win.export-to-url");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(export_button), G_MENU_MODEL(export_menu));
    g_object_unref(export_menu);
    
    gtk_box_append(GTK_BOX(toolbar_section), import_button);
    gtk_box_append(GTK_BOX(toolbar_section), sort_button);
    gtk_box_append(GTK_BOX(toolbar_section), shuffle_button);
    gtk_box_append(GTK_BOX(toolbar_section), clear_button);
    gtk_box_append(GTK_BOX(toolbar_section), export_button);
    
    // 添加弹性空间，将右侧控件推到右边
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(toolbar_section), spacer);
    
    // 创建离线数据开关
    GtkWidget *offline_data_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(offline_data_box, GTK_ALIGN_CENTER);
    GtkWidget *offline_data_label = gtk_label_new("离线数据");
    gtk_box_append(GTK_BOX(offline_data_box), offline_data_label);
    GtkWidget *offline_data_switch = gtk_switch_new();
    gtk_widget_set_valign(offline_data_switch, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(offline_data_switch, "开启后将下载离线卡片数据");
    // 从配置文件加载开关状态，如果配置中开启但数据不存在则关闭
    gboolean config_state = load_offline_data_switch_state();
    gboolean actual_state = config_state && offline_data_exists();
    gtk_switch_set_active(GTK_SWITCH(offline_data_switch), actual_state);
    // 如果配置状态与实际状态不一致，保存实际状态
    if (config_state != actual_state) {
        save_offline_data_switch_state(actual_state);
    }
    // 如果开关是开启状态，启动时检查更新
    if (actual_state) {
        g_message("Offline data switch is ON, checking for updates...");
        check_offline_data_update(NULL, NULL);
    }
    gtk_box_append(GTK_BOX(offline_data_box), offline_data_switch);
    gtk_box_append(GTK_BOX(toolbar_section), offline_data_box);
    
    // 创建禁限卡表下拉菜单
    GtkWidget *forbidden_dropdown = gtk_drop_down_new_from_strings((const char *[]){"OCG", "TCG", "简体中文", NULL});
    gtk_drop_down_set_selected(GTK_DROP_DOWN(forbidden_dropdown), 0); // 默认选择OCG
    gtk_widget_set_tooltip_text(forbidden_dropdown, "选择禁限卡表");
    gtk_box_append(GTK_BOX(toolbar_section), forbidden_dropdown);
    
    gtk_box_append(GTK_BOX(col_mid), toolbar_section);

    // 工具栏与 Main 区域之间的分隔线
    GtkWidget *toolbar_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(col_mid), toolbar_separator);

    // Main 区域（上）
    GtkWidget *main_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_vexpand(main_section, TRUE);
    GtkWidget *main_title_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(main_title_row, 8);
    gtk_widget_set_margin_end(main_title_row, 8);
    gtk_widget_set_margin_top(main_title_row, 6);
    GtkWidget *main_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(main_title), "<span size='large'>Main</span>");
    gtk_label_set_xalign(GTK_LABEL(main_title), 0.0);
    gtk_widget_add_css_class(main_title, "heading");
    gtk_box_append(GTK_BOX(main_title_row), main_title);
    GtkWidget *main_count = gtk_label_new("(0)");
    gtk_widget_add_css_class(main_count, "dim-label");
    gtk_box_append(GTK_BOX(main_title_row), main_count);
    gtk_box_append(GTK_BOX(main_section), main_title_row);
    // 使用 ScrolledWindow 包装，让内容有自然宽度
    GtkWidget *main_placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_widget_set_vexpand(main_placeholder, TRUE);
    gtk_widget_set_hexpand(main_placeholder, TRUE);
    gtk_widget_set_margin_start(main_placeholder, 8);
    gtk_widget_set_margin_end(main_placeholder, 8);
    gtk_widget_set_margin_top(main_placeholder, 14);
    // Main 示例：4 行，每行 15 张，占位卡槽 50x73（共60）
    {
        GtkWidget *main_grid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
        // 初始化槽位数组
        GPtrArray *main_pics = g_ptr_array_new_with_free_func(g_object_unref);
        for (int r = 0; r < 4; ++r) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
            gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
            gtk_widget_set_hexpand(row, FALSE);
            for (int c = 0; c < 15; ++c) {
                int idx = r * 15 + c + 1;
                char txt[8];
                g_snprintf(txt, sizeof txt, "%d", idx);
                GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
                gtk_widget_add_css_class(slot, "deck-card");
                gtk_widget_set_halign(slot, GTK_ALIGN_START);
                gtk_widget_set_valign(slot, GTK_ALIGN_START);
                GtkWidget *pic = gtk_drawing_area_new();
                gtk_widget_set_size_request(pic, 50, 73);
                gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pic), draw_pixbuf_scaled, NULL, NULL);
                g_signal_connect(pic, "destroy", G_CALLBACK(on_drawing_area_destroy), NULL);
                gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
                gtk_widget_set_valign(pic, GTK_ALIGN_CENTER);
                gtk_widget_set_hexpand(pic, FALSE);
                gtk_widget_set_vexpand(pic, FALSE);
                gtk_box_append(GTK_BOX(slot), pic);
                g_ptr_array_add(main_pics, g_object_ref(pic));
                gtk_box_append(GTK_BOX(row), slot);
            }
            gtk_box_append(GTK_BOX(main_grid), row);
        }
        gtk_box_append(GTK_BOX(main_placeholder), main_grid);
        // 保存到 SearchUI 初始化后
        // 暂存到 main_placeholder 的数据，稍后赋值到 SearchUI
        // 同时为每个槽记录区域与序号
        for (guint i = 0; i < main_pics->len; ++i) {
            GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(main_pics, i));
            g_object_set_data(G_OBJECT(pic), "slot_region", (gpointer)"main");
            g_object_set_data(G_OBJECT(pic), "slot_index", GINT_TO_POINTER((int)i));
        }
        g_object_set_data_full(G_OBJECT(main_placeholder), "slot_main_pics", main_pics, (GDestroyNotify)g_ptr_array_unref);
    }
    gtk_box_append(GTK_BOX(main_section), main_placeholder);
    gtk_box_append(GTK_BOX(col_mid), main_section);

    // Extra 区域（中）- 固定高度容纳两行 50x73 图片
    GtkWidget *extra_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(extra_section, FALSE);
    GtkWidget *extra_header = make_section_header("Extra");
    gtk_widget_set_margin_top(extra_header, 10);
    gtk_box_append(GTK_BOX(extra_section), extra_header);
    // 占位容器：固定高度 73px（单行 50x73）
    GtkWidget *extra_placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(extra_placeholder, TRUE);
    gtk_widget_set_vexpand(extra_placeholder, FALSE);
    gtk_widget_set_size_request(extra_placeholder, -1, 73);
    gtk_widget_set_margin_start(extra_placeholder, 8);
    gtk_widget_set_margin_end(extra_placeholder, 8);
    gtk_widget_set_margin_top(extra_placeholder, 14);
    gtk_widget_set_margin_bottom(extra_placeholder, 4);
    // Extra 示例：单行 15 张，占位卡槽 50x73
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(row, FALSE);
        GPtrArray *extra_pics = g_ptr_array_new_with_free_func(g_object_unref);
        for (int c = 0; c < 15; ++c) {
            char txt[8];
            g_snprintf(txt, sizeof txt, "E%d", c + 1);
            GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_widget_add_css_class(slot, "deck-card");
            gtk_widget_set_halign(slot, GTK_ALIGN_START);
            gtk_widget_set_valign(slot, GTK_ALIGN_START);
            GtkWidget *pic = gtk_drawing_area_new();
            gtk_widget_set_size_request(pic, 50, 73);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pic), draw_pixbuf_scaled, NULL, NULL);
            g_signal_connect(pic, "destroy", G_CALLBACK(on_drawing_area_destroy), NULL);
            gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(pic, GTK_ALIGN_CENTER);
            gtk_widget_set_hexpand(pic, FALSE);
            gtk_widget_set_vexpand(pic, FALSE);
            gtk_box_append(GTK_BOX(slot), pic);
            g_ptr_array_add(extra_pics, g_object_ref(pic));
            gtk_box_append(GTK_BOX(row), slot);
        }
        gtk_box_append(GTK_BOX(extra_placeholder), row);
        for (guint i = 0; i < extra_pics->len; ++i) {
            GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(extra_pics, i));
            g_object_set_data(G_OBJECT(pic), "slot_region", (gpointer)"extra");
            g_object_set_data(G_OBJECT(pic), "slot_index", GINT_TO_POINTER((int)i));
        }
        g_object_set_data_full(G_OBJECT(extra_placeholder), "slot_extra_pics", extra_pics, (GDestroyNotify)g_ptr_array_unref);
    }
    gtk_box_append(GTK_BOX(extra_section), extra_placeholder);
    gtk_box_append(GTK_BOX(col_mid), extra_section);

    // Side 区域（下）- 固定为单行 50x73 图片
    GtkWidget *side_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(side_section, FALSE);
    GtkWidget *side_header = make_section_header("Side");
    gtk_widget_set_margin_top(side_header, 10);
    gtk_box_append(GTK_BOX(side_section), side_header);
    GtkWidget *side_placeholder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(side_placeholder, 8);
    gtk_widget_set_margin_end(side_placeholder, 8);
    gtk_widget_set_margin_top(side_placeholder, 14);
    gtk_widget_set_margin_bottom(side_placeholder, 14);
    gtk_widget_set_vexpand(side_placeholder, FALSE);
    gtk_widget_set_hexpand(side_placeholder, TRUE);
    gtk_widget_set_size_request(side_placeholder, -1, 73);
    // Side 示例：单行 15 张，占位卡槽 50x73
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
        gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(row, FALSE);
        GPtrArray *side_pics = g_ptr_array_new_with_free_func(g_object_unref);
        for (int c = 0; c < 15; ++c) {
            char txt[8];
            g_snprintf(txt, sizeof txt, "S%d", c + 1);
            GtkWidget *slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            gtk_widget_add_css_class(slot, "deck-card");
            gtk_widget_set_halign(slot, GTK_ALIGN_START);
            gtk_widget_set_valign(slot, GTK_ALIGN_START);
            GtkWidget *pic = gtk_drawing_area_new();
            gtk_widget_set_size_request(pic, 50, 73);
            gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(pic), draw_pixbuf_scaled, NULL, NULL);
            g_signal_connect(pic, "destroy", G_CALLBACK(on_drawing_area_destroy), NULL);
            gtk_widget_set_halign(pic, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(pic, GTK_ALIGN_CENTER);
            gtk_widget_set_hexpand(pic, FALSE);
            gtk_widget_set_vexpand(pic, FALSE);
            gtk_box_append(GTK_BOX(slot), pic);
            g_ptr_array_add(side_pics, g_object_ref(pic));
            gtk_box_append(GTK_BOX(row), slot);
        }
        gtk_box_append(GTK_BOX(side_placeholder), row);
        for (guint i = 0; i < side_pics->len; ++i) {
            GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(side_pics, i));
            g_object_set_data(G_OBJECT(pic), "slot_region", (gpointer)"side");
            g_object_set_data(G_OBJECT(pic), "slot_index", GINT_TO_POINTER((int)i));
        }
        g_object_set_data_full(G_OBJECT(side_placeholder), "slot_side_pics", side_pics, (GDestroyNotify)g_ptr_array_unref);
    }
    gtk_box_append(GTK_BOX(side_section), side_placeholder);
    gtk_box_append(GTK_BOX(col_mid), side_section);

    // 右栏搜索UI
    SearchUI *sui = g_new0(SearchUI, 1);
    sui->session = soup_session_new();
    sui->left_stack = GTK_STACK(left_stack);
    sui->left_picture = GTK_PICTURE(left_picture);
    sui->left_label = GTK_LABEL(left_label);
    sui->left_spinner = GTK_SPINNER(left_spinner);
    
    // 初始化图片加载队列
    sui->search_image_queue = NULL;
    sui->search_image_loader_id = 0;
    
    // 初始化批量渲染队列
    sui->pending_results = NULL;
    sui->batch_render_id = 0;
    
    // 保存窗口引用（用于显示对话框）
    sui->window = win;
    
    // 保存下拉菜单引用
    sui->forbidden_dropdown = GTK_DROP_DOWN(forbidden_dropdown);
    
    // 启动后台更新禁限卡表（先下载）
    startup_update_ocg_forbidden();
    startup_update_tcg_forbidden();
    startup_update_sc_forbidden();
    
    // 加载禁限卡表（从配置目录）
    gchar *ocg_path = get_forbidden_list_path("ocg_forbidden.json");
    gchar *tcg_path = get_forbidden_list_path("tcg_forbidden.json");
    gchar *sc_path = get_forbidden_list_path("sc_forbidden.json");
    
    sui->ocg_forbidden = load_forbidden_list(ocg_path ? ocg_path : "");
    sui->tcg_forbidden = load_forbidden_list(tcg_path ? tcg_path : "");
    sui->sc_forbidden = load_forbidden_list(sc_path ? sc_path : "");
    
    g_free(ocg_path);
    g_free(tcg_path);
    g_free(sc_path);
    
    // 连接下拉菜单变化信号
    g_signal_connect(forbidden_dropdown, "notify::selected", G_CALLBACK(on_forbidden_dropdown_changed), sui);
    
    // 连接离线数据开关信号
    g_signal_connect(offline_data_switch, "notify::active", G_CALLBACK(on_offline_data_switch_changed), sui);
    
    // 连接导出及其它按钮信号；导入使用 GMenu + GAction，下面注册 action 并由菜单调用
    // 在窗口上注册名为 "import-from-file" 的 action（路径为 win.import-from-file）
    GSimpleAction *import_action = g_simple_action_new("import-from-file", NULL);
    g_signal_connect(import_action, "activate", G_CALLBACK(on_action_import_from_file), sui);
    g_action_map_add_action(G_ACTION_MAP(win), G_ACTION(import_action));
    g_object_unref(import_action);
    // 注册一个占位 action 用于菜单项 "从URL..."（UI 审查用）
    GSimpleAction *import_url_action = g_simple_action_new("import-from-url", NULL);
    g_signal_connect(import_url_action, "activate", G_CALLBACK(on_action_import_from_url), sui);
    g_action_map_add_action(G_ACTION_MAP(win), G_ACTION(import_url_action));
    g_object_unref(import_url_action);
    g_signal_connect(sort_button, "clicked", G_CALLBACK(on_sort_clicked), sui);
    g_signal_connect(shuffle_button, "clicked", G_CALLBACK(on_shuffle_clicked), sui);
    g_signal_connect(clear_button, "clicked", G_CALLBACK(on_clear_clicked), sui);
    // 注册导出 action（菜单项触发），复用原有导出处理
    GSimpleAction *export_action = g_simple_action_new("export-to-file", NULL);
    g_signal_connect(export_action, "activate", G_CALLBACK(on_action_export_to_file), sui);
    g_action_map_add_action(G_ACTION_MAP(win), G_ACTION(export_action));
    g_object_unref(export_action);
    // 注册一个占位 action 用于菜单项 "到URL..."（UI 审查用）
    GSimpleAction *export_url_action = g_simple_action_new("export-to-url", NULL);
    g_signal_connect(export_url_action, "activate", G_CALLBACK(on_action_export_to_url), sui);
    g_action_map_add_action(G_ACTION_MAP(win), G_ACTION(export_url_action));
    g_object_unref(export_url_action);
    
    GtkWidget *search_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(search_bar, 8);
    gtk_widget_set_margin_end(search_bar, 8);
    gtk_widget_set_margin_top(search_bar, 8);
    gtk_widget_set_margin_bottom(search_bar, 8);
    sui->entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sui->entry), "搜索卡片名称/效果/ID...");
    gtk_widget_set_hexpand(sui->entry, TRUE);
    
    // 使用图标按钮代替文本按钮
    sui->button = gtk_button_new_from_icon_name("edit-find-symbolic");
    gtk_widget_set_tooltip_text(sui->button, "搜索");
    gtk_widget_add_css_class(sui->button, "flat");
    g_signal_connect(sui->button, "clicked", G_CALLBACK(on_search_clicked), sui);
    
    // 为输入框添加回车键支持,触发按钮点击
    g_signal_connect_swapped(sui->entry, "activate", G_CALLBACK(gtk_widget_activate), sui->button);
    gtk_box_append(GTK_BOX(search_bar), sui->entry);
    gtk_box_append(GTK_BOX(search_bar), sui->button);
    
    // 添加筛选选项按钮
    GtkWidget *filter_button = gtk_button_new_from_icon_name("nautilus-search-filters-symbolic");
    gtk_widget_set_tooltip_text(filter_button, "筛选选项");
    gtk_widget_add_css_class(filter_button, "flat");
    g_signal_connect(filter_button, "clicked", G_CALLBACK(on_filter_button_clicked), win);
    
    gtk_box_append(GTK_BOX(search_bar), filter_button);

    GtkWidget *scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_widget_set_hexpand(scroller, TRUE);
    // 性能优化:启用kinetic scrolling以获得更流畅的滚动体验
    gtk_scrolled_window_set_kinetic_scrolling(GTK_SCROLLED_WINDOW(scroller), TRUE);
    // 使用自动滚动策略,只在需要时显示滚动条
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    sui->list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(sui->list), GTK_SELECTION_NONE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), sui->list);

    gtk_box_append(GTK_BOX(col_end), search_bar);
    gtk_box_append(GTK_BOX(col_end), scroller);

    // 绑定槽位数组到 SearchUI
    sui->main_pics = (GPtrArray*)g_object_get_data(G_OBJECT(main_placeholder), "slot_main_pics");
    sui->extra_pics = (GPtrArray*)g_object_get_data(G_OBJECT(extra_placeholder), "slot_extra_pics");
    sui->side_pics = (GPtrArray*)g_object_get_data(G_OBJECT(side_placeholder), "slot_side_pics");
    // 初始化索引
    sui->main_idx = 0;
    sui->extra_idx = 0;
    sui->side_idx = 0;
    // 计数标签绑定
    sui->main_count = GTK_LABEL(main_count);
    sui->extra_count = GTK_LABEL(g_object_get_data(G_OBJECT(extra_header), "count_label"));
    sui->side_count  = GTK_LABEL(g_object_get_data(G_OBJECT(side_header), "count_label"));

    // 为槽位点击事件设置 user_data 指向 SearchUI
    if (sui->main_pics) {
        for (guint i = 0; i < sui->main_pics->len; ++i) {
            GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(sui->main_pics, i));
            GtkGesture *slot_click = gtk_gesture_click_new();
            // 记录按下位置
            g_signal_connect(slot_click, "pressed", G_CALLBACK(on_slot_pressed), sui);
            // 使用 released，结合位移判断触发删除
            g_signal_connect(slot_click, "released", G_CALLBACK(on_slot_clicked), sui);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(slot_click));
            // Drag source
            GtkDragSource *ds = gtk_drag_source_new();
            gtk_drag_source_set_actions(ds, GDK_ACTION_MOVE);
            g_signal_connect(ds, "prepare", G_CALLBACK(on_drag_prepare), NULL);
            g_signal_connect(ds, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(ds));
            // Drop target
            GtkDropTarget *dt = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY | GDK_ACTION_MOVE);
            g_signal_connect(dt, "accept", G_CALLBACK(on_drop_accept), NULL);
            g_signal_connect(dt, "drop", G_CALLBACK(on_drop), sui);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(dt));
            // Motion controller for hover preview
            GtkEventController *motion = GTK_EVENT_CONTROLLER(gtk_event_controller_motion_new());
            g_signal_connect(motion, "enter", G_CALLBACK(on_slot_enter), sui);
            gtk_widget_add_controller(pic, motion);
        }
    }
    if (sui->extra_pics) {
        for (guint i = 0; i < sui->extra_pics->len; ++i) {
            GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(sui->extra_pics, i));
            GtkGesture *slot_click = gtk_gesture_click_new();
            g_signal_connect(slot_click, "pressed", G_CALLBACK(on_slot_pressed), sui);
            g_signal_connect(slot_click, "released", G_CALLBACK(on_slot_clicked), sui);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(slot_click));
            GtkDragSource *ds = gtk_drag_source_new();
            gtk_drag_source_set_actions(ds, GDK_ACTION_MOVE);
            g_signal_connect(ds, "prepare", G_CALLBACK(on_drag_prepare), NULL);
            g_signal_connect(ds, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(ds));
            GtkDropTarget *dt = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY | GDK_ACTION_MOVE);
            g_signal_connect(dt, "accept", G_CALLBACK(on_drop_accept), NULL);
            g_signal_connect(dt, "drop", G_CALLBACK(on_drop), sui);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(dt));
            // Motion controller for hover preview
            GtkEventController *motion = GTK_EVENT_CONTROLLER(gtk_event_controller_motion_new());
            g_signal_connect(motion, "enter", G_CALLBACK(on_slot_enter), sui);
            gtk_widget_add_controller(pic, motion);
        }
    }
    if (sui->side_pics) {
        for (guint i = 0; i < sui->side_pics->len; ++i) {
            GtkWidget *pic = GTK_WIDGET(g_ptr_array_index(sui->side_pics, i));
            GtkGesture *slot_click = gtk_gesture_click_new();
            g_signal_connect(slot_click, "pressed", G_CALLBACK(on_slot_pressed), sui);
            g_signal_connect(slot_click, "released", G_CALLBACK(on_slot_clicked), sui);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(slot_click));
            GtkDragSource *ds = gtk_drag_source_new();
            gtk_drag_source_set_actions(ds, GDK_ACTION_MOVE);
            g_signal_connect(ds, "prepare", G_CALLBACK(on_drag_prepare), NULL);
            g_signal_connect(ds, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(ds));
            GtkDropTarget *dt = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_COPY | GDK_ACTION_MOVE);
            g_signal_connect(dt, "accept", G_CALLBACK(on_drop_accept), NULL);
            g_signal_connect(dt, "drop", G_CALLBACK(on_drop), sui);
            gtk_widget_add_controller(pic, GTK_EVENT_CONTROLLER(dt));
            // Motion controller for hover preview
            GtkEventController *motion = GTK_EVENT_CONTROLLER(gtk_event_controller_motion_new());
            g_signal_connect(motion, "enter", G_CALLBACK(on_slot_enter), sui);
            gtk_widget_add_controller(pic, motion);
        }
    }

    adw_toolbar_view_set_content(toolbar_view, GTK_WIDGET(outer));
    
    // 添加Toast Overlay以支持通知消息
    AdwToastOverlay *toast_overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(toast_overlay, GTK_WIDGET(toolbar_view));
    sui->toast_overlay = toast_overlay;
    
    adw_application_window_set_content(win, GTK_WIDGET(toast_overlay));

    // 注册应用菜单动作
    GSimpleAction *download_action = g_simple_action_new("download-prerelease", NULL);
    g_signal_connect(download_action, "activate", G_CALLBACK(on_download_prerelease_action), win);
    g_action_map_add_action(G_ACTION_MAP(win), G_ACTION(download_action));
    g_object_unref(download_action);
    
    GSimpleAction *show_action = g_simple_action_new("show-prerelease", NULL);
    g_signal_connect(show_action, "activate", G_CALLBACK(on_show_prerelease_action), sui);
    g_action_map_add_action(G_ACTION_MAP(win), G_ACTION(show_action));
    g_object_unref(show_action);

    gtk_window_present(GTK_WINDOW(win));
}

int
main(int argc, char *argv[])
{
    g_autoptr(AdwApplication) app = adw_application_new(
        "com.pai535.YGODeckBuilder", G_APPLICATION_DEFAULT_FLAGS);

    // 初始化 libadwaita 样式管理器
    adw_init();

    // 加载导出目录配置
    load_io_config(&last_export_directory, &last_import_directory);

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
    return g_application_run(G_APPLICATION(app), argc, argv);
}
