#ifndef SEARCH_FILTER_H
#define SEARCH_FILTER_H

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include "app_types.h"

// 搜索结果图片加载数据结构
typedef struct {
    int img_id;
    gboolean is_prerelease;
} SearchImageToLoad;

// 搜索与过滤相关回调
void on_search_clicked(GtkButton *btn, gpointer user_data);
void on_monster_filter_toggled(GtkCheckButton *btn, gpointer user_data);
void on_spell_filter_toggled(GtkCheckButton *btn, gpointer user_data);
void on_trap_filter_toggled(GtkCheckButton *btn, gpointer user_data);

// 搜索结果处理
void add_result_row(SearchUI *ui, JsonObject *obj);
void search_response_cb(GObject *source, GAsyncResult *res, gpointer user_data);

// 图片加载队列管理
gboolean search_load_next_image(gpointer user_data);

// 批量渲染管理
void queue_result_for_render(SearchUI *ui, JsonObject *obj);
gboolean batch_render_results(gpointer user_data);
void add_result_row_immediate(SearchUI *ui, JsonObject *obj);

// 禁限卡表变化回调
void on_forbidden_dropdown_changed(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data);

// 筛选状态结构体（与main.c中的FilterState对应）
typedef struct {
    guint card_type_selected;  // 卡片类型选择 (0=全部, 1=怪兽, 2=魔法, 3=陷阱)
    gboolean monster_type_toggles[15];  // 15个怪兽类别toggle状态
    gboolean link_marker_toggles[8];  // 8个连接箭头toggle状态
    guint spell_type_selected;  // 魔法类别选择 (0=全部, 1=通常, 2=仪式, 3=速攻, 4=永续, 5=装备, 6=场地)
    guint trap_type_selected;  // 陷阱类别选择
    guint attribute_selected;  // 属性选择
    guint race_selected;  // 种族选择
    gchar *atk_text;  // 攻击力文本
    gchar *def_text;  // 守备力文本
    gchar *level_text;  // 等级文本
    gchar *left_scale_text;  // 左刻度文本
    gchar *right_scale_text;  // 右刻度文本
    gchar *field_text;  // 卡片字段文本
} FilterState;

// 应用筛选条件到搜索结果
// 返回值：TRUE 表示卡片通过筛选，FALSE 表示应该被过滤掉
gboolean apply_filter(JsonObject *card, const FilterState *filter_state);

// 获取当前筛选状态（从main.c中）
const FilterState* get_current_filter_state(void);

// 检查是否有活动的筛选条件
gboolean has_active_filter(void);

#endif // SEARCH_FILTER_H
