#ifndef ARCHIVE_PATH_H
#define ARCHIVE_PATH_H

#include <glib.h>

/**
 * 判断压缩包内的条目名是否可以安全解压到目标目录之内。
 *
 * 拒绝以下条目（Zip Slip / 路径穿越）：
 *  - NULL 或空字符串
 *  - 绝对路径（如 "/etc/passwd"、"\\server\\share"）
 *  - 包含 ".." 路径分量（如 "../x"、"pics/../../x"）
 *
 * 注意：调用方拼接到目标目录前应先做此检查，否则恶意压缩包
 * （cards.zip / YPK 均来自网络下载）可以写到目标目录之外。
 *
 * @param entry_name 压缩包条目名（archive_entry_pathname 的返回值）
 * @return 可以安全解压返回 TRUE，否则返回 FALSE
 */
gboolean archive_entry_path_is_safe(const char *entry_name);

#endif // ARCHIVE_PATH_H
