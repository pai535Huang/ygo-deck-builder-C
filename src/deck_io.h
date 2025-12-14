#ifndef DECK_IO_H
#define DECK_IO_H

#include <gtk/gtk.h>
#include <libsoup/soup.h>

/**
 * 导出卡组为YDK文件
 * @param main_pics 主卡组槽位数组
 * @param main_count 主卡组数量
 * @param extra_pics 额外卡组槽位数组
 * @param extra_count 额外卡组数量
 * @param side_pics 副卡组槽位数组
 * @param side_count 副卡组数量
 * @param filepath 要保存的文件路径
 */
void export_deck_to_ydk(
    GPtrArray *main_pics, int main_count,
    GPtrArray *extra_pics, int extra_count,
    GPtrArray *side_pics, int side_count,
    const char *filepath
);

/**
 * 从YDK文件导入卡组
 * @param main_pics 主卡组槽位数组
 * @param main_idx 主卡组索引指针
 * @param main_count 主卡组计数标签
 * @param extra_pics 额外卡组槽位数组
 * @param extra_idx 额外卡组索引指针
 * @param extra_count 额外卡组计数标签
 * @param side_pics 副卡组槽位数组
 * @param side_idx 副卡组索引指针
 * @param side_count 副卡组计数标签
 * @param session libsoup会话（用于加载图片）
 * @param filepath 要读取的文件路径
 * @return 成功返回TRUE，失败返回FALSE
 */
gboolean import_deck_from_ydk(
    GPtrArray *main_pics, int *main_idx, GtkLabel *main_count,
    GPtrArray *extra_pics, int *extra_idx, GtkLabel *extra_count,
    GPtrArray *side_pics, int *side_idx, GtkLabel *side_count,
    SoupSession *session,
    const char *filepath
);

/**
 * 加载导入导出目录配置
 * @param last_export_dir 存储最后导出目录的指针
 * @param last_import_dir 存储最后导入目录的指针
 */
void load_io_config(char **last_export_dir, char **last_import_dir);

/**
 * 保存导入导出目录配置
 * @param last_export_dir 最后导出目录
 * @param last_import_dir 最后导入目录
 */
void save_io_config(const char *last_export_dir, const char *last_import_dir);

/**
 * 加载离线数据开关状态
 * @return TRUE 如果开关应该开启，FALSE 如果应该关闭
 */
gboolean load_offline_data_switch_state(void);

/**
 * 保存离线数据开关状态
 * @param enabled TRUE 如果开关开启，FALSE 如果关闭
 */
void save_offline_data_switch_state(gboolean enabled);

#endif // DECK_IO_H
