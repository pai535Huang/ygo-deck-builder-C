// archive_path 路径安全单元测试（meson test）
// 覆盖 Zip Slip / 路径穿越：绝对路径、UNC、盘符、".." 分量（含 Windows 反斜杠分隔）、
// 以及必须放行的正常条目名（含先行卡 YPK 使用的 "./test-release.cdb"）。
#include "archive_path.h"
#include <stdio.h>

static int failures = 0;

#define CHECK_SAFE(name) do { \
    if (!archive_entry_path_is_safe(name)) { \
        fprintf(stderr, "FAIL: expected SAFE: %s (line %d)\n", (name), __LINE__); \
        failures++; \
    } \
} while (0)

#define CHECK_UNSAFE(name) do { \
    if (archive_entry_path_is_safe(name)) { \
        fprintf(stderr, "FAIL: expected UNSAFE: %s (line %d)\n", (name), __LINE__); \
        failures++; \
    } \
} while (0)

static void test_safe_entries(void) {
    // cards.zip / YPK 中真实存在的条目名
    CHECK_SAFE("cards.json");
    CHECK_SAFE("pics/12345.jpg");
    CHECK_SAFE("./test-release.cdb");
    CHECK_SAFE("test-release.cdb");
    CHECK_SAFE("images/1/2/3.png");
    CHECK_SAFE("pics//12345.jpg");      // 重复分隔符
    CHECK_SAFE("..hidden/x.png");       // 分量以 .. 开头但不是 ".."
    CHECK_SAFE("a/..b/c.png");          // 分量包含 .. 但不是 ".."
    CHECK_SAFE("dir./x.png");           // 分量以点结尾
}

static void test_unsafe_entries(void) {
    // NULL 单独判断：直接传给 %s 会产生编译警告
    if (archive_entry_path_is_safe(NULL)) {
        fprintf(stderr, "FAIL: expected UNSAFE: (null) (line %d)\n", __LINE__);
        failures++;
    }
    CHECK_UNSAFE("");
    CHECK_UNSAFE("..");
    CHECK_UNSAFE("../evil.txt");
    CHECK_UNSAFE("pics/../../evil.txt");
    CHECK_UNSAFE("pics/..");
    CHECK_UNSAFE("a/b/../../../x");
    CHECK_UNSAFE("/etc/passwd");
    CHECK_UNSAFE("/");
    CHECK_UNSAFE("//etc/passwd");
    // Windows 风格分隔符同样不应绕过
    CHECK_UNSAFE("..\\evil.txt");
    CHECK_UNSAFE("pics\\..\\..\\evil.txt");
    CHECK_UNSAFE("C:\\Windows\\evil.txt");
    CHECK_UNSAFE("\\\\server\\share\\evil.txt");
}

int main(void) {
    test_safe_entries();
    test_unsafe_entries();

    if (failures > 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("test_archive_path: all checks passed\n");
    return 0;
}
