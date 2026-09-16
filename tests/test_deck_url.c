// deck_url 编解码单元测试（meson test）
// 覆盖：编码->解码往返、多种类合并、空卡组、协议边界（27 位 ID / 2 位数量）、
// 非法输入（缺少 ygotype、Base64Url 非法字符、数据长度不足）
#include "deck_url.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (line %d)\n", (msg), __LINE__); \
        failures++; \
    } \
} while (0)

static void test_roundtrip_doc_example(void) {
    // 文档示例卡组：单卡多张
    int main_cards[] = {89631139, 89631139, 89631139};
    int extra_cards[] = {44508094, 44508094};
    int side_cards[] = {14087893};

    char *url = deck_encode_to_url(main_cards, 3, extra_cards, 2, side_cards, 1,
                                   "https://example.com/deck");
    CHECK(url != NULL, "encode returns url");
    CHECK(strstr(url, "ygotype=deck") != NULL, "url contains ygotype=deck");
    CHECK(strstr(url, "d=") != NULL, "url contains d=");

    int *m = NULL, *e = NULL, *s = NULL;
    int mc = 0, ec = 0, sc = 0;
    GError *err = NULL;
    CHECK(deck_decode_from_url(url, &m, &mc, &e, &ec, &s, &sc, &err), "decode round-trip");
    CHECK(mc == 3 && ec == 2 && sc == 1, "round-trip counts");
    CHECK(m && e && s, "round-trip arrays allocated");
    if (m && mc == 3) {
        CHECK(m[0] == 89631139 && m[1] == 89631139 && m[2] == 89631139, "main ids");
    }
    if (e && ec == 2) {
        CHECK(e[0] == 44508094 && e[1] == 44508094, "extra ids");
    }
    if (s && sc == 1) {
        CHECK(s[0] == 14087893, "side ids");
    }

    g_free(url);
    g_free(m);
    g_free(e);
    g_free(s);
    g_clear_error(&err);
}

static void test_roundtrip_multi_unique(void) {
    // 多种类卡片：相同 ID 合并、保持出现顺序
    int main_cards[] = {89631139, 74640994, 89631139, 74640994, 89631139};
    int extra_cards[] = {44508094};
    int side_cards[] = {14087893, 97268411};

    char *url = deck_encode_to_url(main_cards, 5, extra_cards, 1, side_cards, 2, NULL);
    CHECK(url != NULL, "encode multi-unique");

    int *m = NULL, *e = NULL, *s = NULL;
    int mc = 0, ec = 0, sc = 0;
    GError *err = NULL;
    CHECK(deck_decode_from_url(url, &m, &mc, &e, &ec, &s, &sc, &err), "decode multi-unique");
    CHECK(mc == 5 && ec == 1 && sc == 2, "multi-unique counts");
    CHECK(m && mc == 5, "main array");
    if (m && mc == 5) {
        // 相同卡片在解码结果中相邻
        CHECK(m[0] == 89631139 && m[1] == 89631139 && m[2] == 89631139, "merged group 1");
        CHECK(m[3] == 74640994 && m[4] == 74640994, "merged group 2");
    }

    g_free(url);
    g_free(m);
    g_free(e);
    g_free(s);
    g_clear_error(&err);
}

static void test_empty_deck(void) {
    // 无 d= 参数：空卡组有效
    int *m = NULL, *e = NULL, *s = NULL;
    int mc = -1, ec = -1, sc = -1;
    GError *err = NULL;
    CHECK(deck_decode_from_url("https://example.com/deck?ygotype=deck&v=1",
                               &m, &mc, &e, &ec, &s, &sc, &err),
          "empty deck decodes");
    CHECK(mc == 0 && ec == 0 && sc == 0, "empty deck counts");

    g_free(m);
    g_free(e);
    g_free(s);
    g_clear_error(&err);
}

static void test_missing_ygotype(void) {
    int *m = NULL, *e = NULL, *s = NULL;
    int mc = 0, ec = 0, sc = 0;
    GError *err = NULL;
    CHECK(!deck_decode_from_url("https://example.com/deck?d=AAAA", &m, &mc, &e, &ec, &s, &sc, &err),
          "missing ygotype rejected");
    CHECK(err != NULL, "error set for missing ygotype");
    g_free(m);
    g_free(e);
    g_free(s);
    g_clear_error(&err);
}

static void test_invalid_base64(void) {
    // '+' 与 '/' 不是 Base64Url 字符
    int *m = NULL, *e = NULL, *s = NULL;
    int mc = 0, ec = 0, sc = 0;
    GError *err = NULL;
    CHECK(!deck_decode_from_url("https://example.com/deck?ygotype=deck&v=1&d=AA+A//",
                                &m, &mc, &e, &ec, &s, &sc, &err),
          "invalid base64url rejected");
    g_free(m);
    g_free(e);
    g_free(s);
    g_clear_error(&err);
}

static void test_truncated_data(void) {
    // 有效 Base64Url 但头部都不完整（少于 16 位）
    int *m = NULL, *e = NULL, *s = NULL;
    int mc = 0, ec = 0, sc = 0;
    GError *err = NULL;
    CHECK(!deck_decode_from_url("https://example.com/deck?ygotype=deck&v=1&d=AA",
                                &m, &mc, &e, &ec, &s, &sc, &err),
          "truncated data rejected");
    g_free(m);
    g_free(e);
    g_free(s);
    g_clear_error(&err);
}

static void test_out_of_range_card_id_skipped(void) {
    // 超出 27 位范围（0-134217727）的 ID（如 9 位先行卡码）编码时跳过，不产生回绕损坏
    int main_cards[] = {89631139, 569814177};
    int extra_cards[] = {0};
    int side_cards[] = {0};

    char *url = deck_encode_to_url(main_cards, 2, extra_cards, 1, side_cards, 1, NULL);
    CHECK(url != NULL, "encode with out-of-range id still succeeds");

    int *m = NULL, *e = NULL, *s = NULL;
    int mc = 0, ec = 0, sc = 0;
    GError *err = NULL;
    CHECK(deck_decode_from_url(url, &m, &mc, &e, &ec, &s, &sc, &err), "decode out-of-range case");
    // 569814177（9 位先行卡码）被跳过，仅剩 1 张主卡组
    CHECK(mc == 1 && m && m[0] == 89631139, "out-of-range id skipped");

    g_free(url);
    g_free(m);
    g_free(e);
    g_free(s);
    g_clear_error(&err);
}

static void test_count_clamped_to_protocol_max(void) {
    // 单卡超过 3 张（非法卡组）按协议上限 3 编码，解码不回绕为 0
    int main_cards[] = {89631139, 89631139, 89631139, 89631139, 89631139};
    int extra_cards[] = {0};
    int side_cards[] = {0};

    char *url = deck_encode_to_url(main_cards, 5, extra_cards, 1, side_cards, 1, NULL);
    CHECK(url != NULL, "encode with 5 copies succeeds");

    int *m = NULL, *e = NULL, *s = NULL;
    int mc = 0, ec = 0, sc = 0;
    GError *err = NULL;
    CHECK(deck_decode_from_url(url, &m, &mc, &e, &ec, &s, &sc, &err), "decode clamped case");
    CHECK(mc == 3, "copies clamped to protocol max 3");

    g_free(url);
    g_free(m);
    g_free(e);
    g_free(s);
    g_clear_error(&err);
}

int main(void) {
    test_roundtrip_doc_example();
    test_roundtrip_multi_unique();
    test_empty_deck();
    test_missing_ygotype();
    test_invalid_base64();
    test_truncated_data();
    test_out_of_range_card_id_skipped();
    test_count_clamped_to_protocol_max();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_deck_url: all checks passed\n");
    return 0;
}
