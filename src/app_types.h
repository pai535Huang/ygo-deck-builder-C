#ifndef APP_TYPES_H
#define APP_TYPES_H

#include <gtk/gtk.h>
#include <adwaita.h>
#include <libsoup/soup.h>

// CardPreview 结构：用于在搜索结果行中存储卡片信息
typedef struct {
    int id;   // 数据库ID，用于图片URL
    int cid;  // 卡片ID，用于禁限卡表和卡组统计
    char *cn_name;
    uint32_t type;  // 用于程序内部判断
    char *types;    // 用于UI显示
    char *pdesc;
    char *desc;
    gboolean is_prerelease;  // 是否是先行卡
    
    // data 字段下的所有内容（用于后续功能）
    int64_t ot;         // 卡片所属（OCG/TCG/等）
    int64_t setcode;    // 字段代码
    int64_t atk;        // 攻击力（-1表示无效或?）
    int64_t def;        // 防御力（-1表示无效或?）
    int64_t level;      // 等级/阶级/连接数/刻度
    int64_t race;       // 种族
    int64_t attribute;  // 属性
} CardPreview;

// SearchUI 结构：主应用程序状态和UI组件
typedef struct {
    GtkWidget *entry;
    GtkWidget *button;
    GtkWidget *list;
    SoupSession *session;
    GtkStack *left_stack;
    GtkPicture *left_picture;
    GtkLabel *left_label;
    GtkSpinner *left_spinner;
    // 槽位管理
    GPtrArray *main_pics;
    GPtrArray *extra_pics;
    GPtrArray *side_pics;
    int main_idx;
    int extra_idx;
    int side_idx;
    // 计数标签
    GtkLabel *main_count;
    GtkLabel *extra_count;
    GtkLabel *side_count;
    // 禁限卡表
    GtkDropDown *forbidden_dropdown;
    GHashTable *ocg_forbidden;
    GHashTable *tcg_forbidden;
    GHashTable *sc_forbidden;
    // 过滤选项
    GtkWidget *filter_popover;
    gboolean filter_by_monster;
    gboolean filter_by_spell;
    gboolean filter_by_trap;
    // 搜索结果图片加载队列
    GPtrArray *search_image_queue;
    guint search_image_loader_id;
    // 批量渲染队列
    GPtrArray *pending_results;  // 存储待渲染的JsonObject
    guint batch_render_id;       // idle callback的ID
    // 窗口引用（用于显示对话框）
    AdwApplicationWindow *window;
    // Toast overlay（用于显示通知）
    AdwToastOverlay *toast_overlay;
} SearchUI;

#endif // APP_TYPES_H
