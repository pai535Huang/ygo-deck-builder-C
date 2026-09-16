// card_info / strings.conf setcode 解析与缓存单元测试（meson test）
// 覆盖：
//  1) 纯解析：注释/空行/畸形行、子串匹配、首个匹配优先、大小写 0X 前缀；
//  2) 缓存：同一文件版本内不再重读磁盘；文件变化（大小/修改时间）后自动失效。
// 第 2 点是本次性能修复的核心——此前每张卡都重新读取并解析 47KB 的 strings.conf。
#include "card_info.h"
#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <utime.h>

static int failures = 0;

#define CHECK_U64(actual, expected, msg) do { \
    if ((actual) != (expected)) { \
        fprintf(stderr, "FAIL: %s: expected 0x%llX, got 0x%llX (line %d)\n", \
                (msg), (unsigned long long)(expected), (unsigned long long)(actual), __LINE__); \
        failures++; \
    } \
} while (0)

static const char *SAMPLE =
    "# 注释行\n"
    "\n"
    "!setname 0x1234 Alpha\n"
    "!setname 0x5678 Beta\n"
    "!setname 0x9ABC Alpha\n"        // 第二个 Alpha：首个匹配必须优先
    "!setname 0xDEF0 2Alpha2\n"      // 子串匹配目标
    "!setname 0XABCD Upper\n"        // 大写 0X 前缀
    "!setname\n"                     // 畸形：仅 1 段
    "!setname 0x1111\n"              // 畸形：仅 2 段
    "!other 0x2222 NotSetname\n";    // 非 setname 指令

static void test_parse_semantics(void) {
    CHECK_U64(strings_conf_parse_setcode(SAMPLE, "Alpha"), 0x1234, "first match wins");
    CHECK_U64(strings_conf_parse_setcode(SAMPLE, "Beta"), 0x5678, "simple match");
    CHECK_U64(strings_conf_parse_setcode(SAMPLE, "Upper"), 0xABCD, "uppercase 0X prefix");
    CHECK_U64(strings_conf_parse_setcode(SAMPLE, "2Alpha"), 0xDEF0, "substring match");
    CHECK_U64(strings_conf_parse_setcode(SAMPLE, "NotSetname"), 0, "only !setname lines count");
    CHECK_U64(strings_conf_parse_setcode(SAMPLE, "Missing"), 0, "unknown field returns 0");
    CHECK_U64(strings_conf_parse_setcode(SAMPLE, ""), 0, "empty field returns 0");
    CHECK_U64(strings_conf_parse_setcode(SAMPLE, NULL), 0, "NULL field returns 0");
    CHECK_U64(strings_conf_parse_setcode(NULL, "Alpha"), 0, "NULL content returns 0");
}

/* 写入内容并把 mtime 固定为 t，使缓存失效条件只取决于 (size, mtime) */
static void write_conf(const char *path, const char *content, time_t t) {
    g_file_set_contents(path, content, -1, NULL);
    struct utimbuf ut = { t, t };
    utime(path, &ut);
}

static void test_cache_reuse_and_invalidation(void) {
    GError *err = NULL;
    gchar *dir = g_dir_make_tmp("ygo-cardinfo-XXXXXX", &err);
    if (!dir) {
        fprintf(stderr, "FAIL: cannot create temp dir: %s\n", err ? err->message : "?");
        if (err) g_error_free(err);
        failures++;
        return;
    }
    gchar *path = g_build_filename(dir, "strings.conf", NULL);
    const time_t T = 1600000000;  // 固定时间戳

    // 版本 1：Alpha -> 0x1111
    write_conf(path, "!setname 0x1111 Alpha\n", T);
    CHECK_U64(strings_conf_lookup_cached(path, "Alpha"), 0x1111, "cache: first lookup");

    // 版本 2：内容变化但大小与 mtime 完全相同（0x1111 -> 0x2222）
    // 缓存命中 ⇒ 返回旧值，证明没有重新读盘（这正是性能修复要达到的效果）
    write_conf(path, "!setname 0x2222 Alpha\n", T);
    CHECK_U64(strings_conf_lookup_cached(path, "Alpha"), 0x1111,
              "cache: same size+mtime must NOT re-read the file");

    // 版本 3：文件大小改变 ⇒ 缓存必须失效并返回新值
    write_conf(path, "!setname 0x3333 Alpha\n# extra\n", T);
    CHECK_U64(strings_conf_lookup_cached(path, "Alpha"), 0x3333,
              "cache: changed size must invalidate");

    // 版本 4：与版本 3 大小相同、只有 mtime 改变 ⇒ 同样必须失效
    struct utimbuf ut = { T + 5, T + 5 };
    g_file_set_contents(path, "!setname 0x4444 Alpha\n# extra\n", -1, NULL);
    utime(path, &ut);
    CHECK_U64(strings_conf_lookup_cached(path, "Alpha"), 0x4444,
              "cache: changed mtime (same size) must invalidate");

    // 不同字段名各自记忆化，且未命中的字段返回 0
    CHECK_U64(strings_conf_lookup_cached(path, "Beta"), 0, "cache: unknown field is 0");

    // 文件不存在时返回 0，不崩溃
    gchar *missing = g_build_filename(dir, "nope.conf", NULL);
    CHECK_U64(strings_conf_lookup_cached(missing, "Alpha"), 0, "cache: missing file is 0");
    CHECK_U64(strings_conf_lookup_cached(NULL, "Alpha"), 0, "cache: NULL path is 0");

    g_remove(path);
    g_remove(missing);
    g_rmdir(dir);
    g_free(missing);
    g_free(path);
    g_free(dir);
}

int main(void) {
    test_parse_semantics();
    test_cache_reuse_and_invalidation();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_card_info: all checks passed\n");
    return 0;
}
