#ifndef DECK_URL_H
#define DECK_URL_H

#include <glib.h>
#include <stdbool.h>

/**
 * 将卡组数据编码为YGO-DA协议URL
 * 
 * @param main_cards 主卡组卡片ID数组
 * @param main_count 主卡组卡片数量
 * @param extra_cards 额外卡组卡片ID数组
 * @param extra_count 额外卡组卡片数量
 * @param side_cards 副卡组卡片ID数组
 * @param side_count 副卡组卡片数量
 * @param base_url 基础URL（如 "https://example.com/deck"），如果为NULL则使用默认
 * @return 编码后的完整URL字符串，需要调用者使用g_free()释放；失败返回NULL
 */
char* deck_encode_to_url(
    const int *main_cards, int main_count,
    const int *extra_cards, int extra_count,
    const int *side_cards, int side_count,
    const char *base_url
);

/**
 * 从YGO-DA协议URL解码卡组数据
 * 
 * @param url 卡组URL字符串
 * @param main_cards 输出参数：主卡组卡片ID数组，需要调用者使用g_free()释放
 * @param main_count 输出参数：主卡组卡片数量
 * @param extra_cards 输出参数：额外卡组卡片ID数组，需要调用者使用g_free()释放
 * @param extra_count 输出参数：额外卡组卡片数量
 * @param side_cards 输出参数：副卡组卡片ID数组，需要调用者使用g_free()释放
 * @param side_count 输出参数：副卡组卡片数量
 * @param error 输出参数：错误信息，如果失败会被设置
 * @return 成功返回true，失败返回false
 */
bool deck_decode_from_url(
    const char *url,
    int **main_cards, int *main_count,
    int **extra_cards, int *extra_count,
    int **side_cards, int *side_count,
    GError **error
);

#endif // DECK_URL_H
