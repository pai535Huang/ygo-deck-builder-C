#include "archive_path.h"

#include <string.h>

gboolean archive_entry_path_is_safe(const char *entry_name) {
    if (!entry_name || !*entry_name) return FALSE;

    // 绝对路径：POSIX 的 "/x"，或 Windows 的 UNC "\\server\\x"
    if (g_path_is_absolute(entry_name)) return FALSE;
    if (entry_name[0] == '\\') return FALSE;
    // Windows 盘符绝对路径 "C:\x"
    if (g_ascii_isalpha(entry_name[0]) && entry_name[1] == ':') return FALSE;

    // 逐分量检查 ".."：'/' 与 '\' 都当作分隔符，避免 Windows 风格的穿越
    const char *p = entry_name;
    while (*p) {
        const char *end = p;
        while (*end && *end != '/' && *end != '\\') end++;

        if (end - p == 2 && p[0] == '.' && p[1] == '.') return FALSE;

        if (*end == '\0') break;
        p = end + 1;
    }

    return TRUE;
}
