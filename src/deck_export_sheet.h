#ifndef DECK_EXPORT_SHEET_H
#define DECK_EXPORT_SHEET_H

#include <gtk/gtk.h>

/**
 * 生成官方卡表 PDF。
 * - 使用编译时内嵌资源 /com/pai535/YGODeckBuilder/deck_ja_template.png 作为背景模板
 * - 卡名使用离线数据库中的 jp_name
 * - 填写卡名、数量、各区域统计数以及 Konami ID
 */
gboolean generate_deck_sheet_pdf(
    GPtrArray *main_pics, int main_count,
    GPtrArray *extra_pics, int extra_count,
    GPtrArray *side_pics, int side_count,
    const char *konami_id,
    const char *filepath,
    GError **error
);

#endif // DECK_EXPORT_SHEET_H
