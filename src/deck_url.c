#include "deck_url.h"
#include <string.h>
#include <stdlib.h>

// 协议字段限制（见 docs/deck-url.md）：
// 卡片 ID 为 27 位二进制，单卡数量为 2 位（1-3）
#define DECK_URL_MAX_CARD_ID 134217727  /* 2^27 - 1 */
#define DECK_URL_MAX_COPIES  3

// Base64Url字符表（使用 - 和 _ 替换标准Base64的 + 和 /）
static const char base64url_chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// 用于解码的反向映射表
static int base64url_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

// 卡片信息结构（用于排序和计数）
typedef struct {
    int card_id;
    int count;
} CardInfo;

// 统计卡片，返回不同种类的卡片信息（保持原始顺序）
// 协议要求：相同的卡片排在一起，但整体保持YDK文件中的出现顺序
static CardInfo* count_unique_cards(const int *cards, int count, int *unique_count) {
    if (count == 0) {
        *unique_count = 0;
        return NULL;
    }
    
    // 按照原始顺序扫描，统计每种卡片的数量
    // 使用数组存储已经遇到的卡片，保持顺序
    CardInfo *infos = g_new(CardInfo, count);  // 最多count种不同的卡
    int uniq = 0;
    
    for (int i = 0; i < count; i++) {
        int card_id = cards[i];

        // 超出 27 位范围的 ID（负数或 9 位先行卡码等）写入时会回绕为无关卡片，
        // 跳过并警告，避免导出静默损坏的卡组
        if (card_id <= 0 || card_id > DECK_URL_MAX_CARD_ID) {
            g_warning("卡片 ID %d 超出 URL 协议 27 位范围，已跳过", card_id);
            continue;
        }

        // 查找这个卡片是否已经记录过
        int found = -1;
        for (int j = 0; j < uniq; j++) {
            if (infos[j].card_id == card_id) {
                found = j;
                break;
            }
        }
        
        if (found >= 0) {
            // 已经存在，增加计数
            infos[found].count++;
        } else {
            // 新卡片，添加到列表
            infos[uniq].card_id = card_id;
            infos[uniq].count = 1;
            uniq++;
        }
    }
    
    // 调整数组大小到实际需要的大小
    CardInfo *result = g_new(CardInfo, uniq);
    memcpy(result, infos, uniq * sizeof(CardInfo));
    g_free(infos);
    
    *unique_count = uniq;
    return result;
}

// 将位数组转换为Base64Url字符串
static char* bits_to_base64url(const bool *bits, int bit_count) {
    // 为了与其他平台兼容，需要将数据填充到字节边界（8的倍数）
    // 然后再转换为Base64URL
    int padded_bit_count = ((bit_count + 7) / 8) * 8;  // 向上取整到8的倍数
    
    // 计算需要多少个6位组（Base64每个字符代表6位）
    int sextet_count = (padded_bit_count + 5) / 6;
    char *result = g_new(char, sextet_count + 1);
    
    for (int i = 0; i < sextet_count; i++) {
        int value = 0;
        for (int j = 0; j < 6; j++) {
            int bit_idx = i * 6 + j;
            // 超出原始数据的位自动为0（填充位）
            if (bit_idx < bit_count && bits[bit_idx]) {
                value |= (1 << (5 - j));
            }
        }
        result[i] = base64url_chars[value];
    }
    
    result[sextet_count] = '\0';
    return result;
}

// 将整数转换为指定位数的二进制位数组
static void int_to_bits(int value, bool *bits, int start_pos, int bit_count) {
    for (int i = 0; i < bit_count; i++) {
        bits[start_pos + i] = (value >> (bit_count - 1 - i)) & 1;
    }
}

// 单卡数量字段为 2 位，超出按协议上限 3 编码（非法卡组导出时避免回绕）
static int deck_url_clamp_count(int count) {
    if (count > DECK_URL_MAX_COPIES) {
        g_warning("单卡数量 %d 超出协议上限 %d，按 %d 编码", count, DECK_URL_MAX_COPIES, DECK_URL_MAX_COPIES);
        return DECK_URL_MAX_COPIES;
    }
    return count;
}

char* deck_encode_to_url(
    const int *main_cards, int main_count,
    const int *extra_cards, int extra_count,
    const int *side_cards, int side_count,
    const char *base_url)
{
    // 统计每个区域的不同种类卡片
    int main_unique, extra_unique, side_unique;
    CardInfo *main_infos = count_unique_cards(main_cards, main_count, &main_unique);
    CardInfo *extra_infos = count_unique_cards(extra_cards, extra_count, &extra_unique);
    CardInfo *side_infos = count_unique_cards(side_cards, side_count, &side_unique);
    
    // 检查种类数量是否超出限制
    if (main_unique > 255 || extra_unique > 15 || side_unique > 15) {
        g_free(main_infos);
        g_free(extra_infos);
        g_free(side_infos);
        g_warning("卡组种类数量超出限制");
        return NULL;
    }
    
    // 计算总位数：16位头部 + 29位*总种类数
    int total_unique = main_unique + extra_unique + side_unique;
    int total_bits = 16 + 29 * total_unique;
    bool *bits = g_new0(bool, total_bits);
    
    // 写入16位头部：8位主卡组种类数 + 4位额外种类数 + 4位副卡组种类数
    int_to_bits(main_unique, bits, 0, 8);
    int_to_bits(extra_unique, bits, 8, 4);
    int_to_bits(side_unique, bits, 12, 4);
    
    int bit_pos = 16;
    
    // 写入主卡组卡片信息
    for (int i = 0; i < main_unique; i++) {
        int_to_bits(deck_url_clamp_count(main_infos[i].count), bits, bit_pos, 2);
        int_to_bits(main_infos[i].card_id, bits, bit_pos + 2, 27);
        bit_pos += 29;
    }

    // 写入额外卡组卡片信息
    for (int i = 0; i < extra_unique; i++) {
        int_to_bits(deck_url_clamp_count(extra_infos[i].count), bits, bit_pos, 2);
        int_to_bits(extra_infos[i].card_id, bits, bit_pos + 2, 27);
        bit_pos += 29;
    }

    // 写入副卡组卡片信息
    for (int i = 0; i < side_unique; i++) {
        int_to_bits(deck_url_clamp_count(side_infos[i].count), bits, bit_pos, 2);
        int_to_bits(side_infos[i].card_id, bits, bit_pos + 2, 27);
        bit_pos += 29;
    }
    
    // 转换为Base64Url
    char *base64_data = bits_to_base64url(bits, total_bits);
    
    // 构造完整URL
    const char *url_base = base_url ? base_url : "https://example.com/deck";
    char *full_url = g_strdup_printf("%s?ygotype=deck&v=1&d=%s", url_base, base64_data);
    
    // 清理
    g_free(bits);
    g_free(base64_data);
    g_free(main_infos);
    g_free(extra_infos);
    g_free(side_infos);
    
    return full_url;
}

// 从Base64Url解码为位数组
static bool* base64url_to_bits(const char *base64_str, int *bit_count) {
    int len = strlen(base64_str);
    if (len == 0) {
        *bit_count = 0;
        return NULL;
    }
    
    // 每个Base64字符代表6位
    *bit_count = len * 6;
    bool *bits = g_new0(bool, *bit_count);
    
    for (int i = 0; i < len; i++) {
        int value = base64url_decode_char(base64_str[i]);
        if (value < 0) {
            g_free(bits);
            return NULL;
        }
        
        for (int j = 0; j < 6; j++) {
            bits[i * 6 + j] = (value >> (5 - j)) & 1;
        }
    }
    
    return bits;
}

// 从位数组中读取整数
static int bits_to_int(const bool *bits, int start_pos, int bit_count) {
    int value = 0;
    for (int i = 0; i < bit_count; i++) {
        if (bits[start_pos + i]) {
            value |= (1 << (bit_count - 1 - i));
        }
    }
    return value;
}

bool deck_decode_from_url(
    const char *url,
    int **main_cards, int *main_count,
    int **extra_cards, int *extra_count,
    int **side_cards, int *side_count,
    GError **error)
{
    *main_cards = NULL;
    *extra_cards = NULL;
    *side_cards = NULL;
    *main_count = *extra_count = *side_count = 0;
    
    g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_ENCODED_QUERY, NULL) : NULL;
    if (!uri || !g_uri_get_host(uri) || !*g_uri_get_host(uri) ||
        (g_ascii_strcasecmp(g_uri_get_scheme(uri), "http") != 0 &&
         g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") != 0) ||
        !g_uri_get_query(uri)) {
        g_set_error_literal(error, g_quark_from_static_string("deck-url"), 1,
                            "无效的卡组 URL");
        return false;
    }

    GUriParamsIter params;
    g_uri_params_iter_init(&params, g_uri_get_query(uri), -1, "&", G_URI_PARAMS_NONE);
    g_autofree char *deck_type = NULL;
    g_autofree char *version = NULL;
    g_autofree char *base64_data = NULL;
    g_autoptr(GError) param_error = NULL;
    for (;;) {
        g_autofree char *name = NULL;
        g_autofree char *value = NULL;
        if (!g_uri_params_iter_next(&params, &name, &value, &param_error)) break;

        char **field = NULL;
        if (g_str_equal(name, "ygotype")) field = &deck_type;
        else if (g_str_equal(name, "v")) field = &version;
        else if (g_str_equal(name, "d")) field = &base64_data;
        if (!field) continue;
        if (*field) {
            g_set_error(error, g_quark_from_static_string("deck-url"), 1,
                        "重复的卡组 URL 参数: %s", name);
            return false;
        }
        *field = g_steal_pointer(&value);
    }
    if (param_error || g_strcmp0(deck_type, "deck") != 0 ||
        g_strcmp0(version, "1") != 0) {
        g_set_error_literal(error, g_quark_from_static_string("deck-url"), 1,
                            "无效的卡组类型或协议版本");
        return false;
    }

    // A v1 link without d is the existing representation of an empty deck.
    if (!base64_data) {
        *main_cards = g_new(int, 1);
        *extra_cards = g_new(int, 1);
        *side_cards = g_new(int, 1);
        return true;
    }

    const int max_bits = 16 + 29 * (255 + 15 + 15);
    const gsize max_encoded_length = (((max_bits + 7) / 8) * 8 + 5) / 6;
    if (strlen(base64_data) > max_encoded_length) {
        g_set_error_literal(error, g_quark_from_static_string("deck-url"), 2,
                            "卡组 URL 数据超出长度限制");
        return false;
    }
    int bit_count;
    g_autofree bool *bits = base64url_to_bits(base64_data, &bit_count);

    if (!bits || bit_count < 16) {
        g_set_error(error, g_quark_from_string("deck-url"), 2,
                   "Base64Url解码失败或数据不完整");
        return false;
    }
    
    // 读取16位头部
    int main_unique = bits_to_int(bits, 0, 8);
    int extra_unique = bits_to_int(bits, 8, 4);
    int side_unique = bits_to_int(bits, 12, 4);
    
    // 检查数据长度是否足够
    int required_bits = 16 + 29 * (main_unique + extra_unique + side_unique);
    if (bit_count < required_bits) {
        g_set_error(error, g_quark_from_string("deck-url"), 3,
                   "数据长度不足，期望至少%d位，实际%d位", required_bits, bit_count);
        return false;
    }
    
    const int padded_bits = ((required_bits + 7) / 8) * 8;
    if (bit_count > ((padded_bits + 5) / 6) * 6 ||
        bits_to_int(bits, required_bits, bit_count - required_bits) != 0) {
        g_set_error_literal(error, g_quark_from_static_string("deck-url"), 3,
                            "卡组数据包含无效的尾部位");
        return false;
    }

    for (int pos = 16; pos < required_bits; pos += 29) {
        if (bits_to_int(bits, pos, 2) == 0 || bits_to_int(bits, pos + 2, 27) == 0) {
            g_set_error_literal(error, g_quark_from_static_string("deck-url"), 3,
                                "卡组数据包含无效的卡片编号或张数");
            return false;
        }
    }

    int bit_pos = 16;
    
    // 解码主卡组
    *main_count = 0;
    for (int i = 0; i < main_unique; i++) {
        int count = bits_to_int(bits, bit_pos, 2);
        *main_count += count;
        bit_pos += 29;
    }
    *main_cards = g_new(int, *main_count > 0 ? *main_count : 1);
    
    bit_pos = 16;
    int card_idx = 0;
    for (int i = 0; i < main_unique; i++) {
        int count = bits_to_int(bits, bit_pos, 2);
        int card_id = bits_to_int(bits, bit_pos + 2, 27);
        for (int j = 0; j < count; j++) {
            (*main_cards)[card_idx++] = card_id;
        }
        bit_pos += 29;
    }
    
    // 解码额外卡组
    *extra_count = 0;
    for (int i = 0; i < extra_unique; i++) {
        int count = bits_to_int(bits, bit_pos, 2);
        *extra_count += count;
        bit_pos += 29;
    }
    *extra_cards = g_new(int, *extra_count > 0 ? *extra_count : 1);
    
    bit_pos = 16 + 29 * main_unique;
    card_idx = 0;
    for (int i = 0; i < extra_unique; i++) {
        int count = bits_to_int(bits, bit_pos, 2);
        int card_id = bits_to_int(bits, bit_pos + 2, 27);
        for (int j = 0; j < count; j++) {
            (*extra_cards)[card_idx++] = card_id;
        }
        bit_pos += 29;
    }
    
    // 解码副卡组
    *side_count = 0;
    for (int i = 0; i < side_unique; i++) {
        int count = bits_to_int(bits, bit_pos, 2);
        *side_count += count;
        bit_pos += 29;
    }
    *side_cards = g_new(int, *side_count > 0 ? *side_count : 1);
    
    bit_pos = 16 + 29 * (main_unique + extra_unique);
    card_idx = 0;
    for (int i = 0; i < side_unique; i++) {
        int count = bits_to_int(bits, bit_pos, 2);
        int card_id = bits_to_int(bits, bit_pos + 2, 27);
        for (int j = 0; j < count; j++) {
            (*side_cards)[card_idx++] = card_id;
        }
        bit_pos += 29;
    }
    
    return true;
}
