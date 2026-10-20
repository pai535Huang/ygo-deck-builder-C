#ifndef PREFS_H
#define PREFS_H

#include <glib.h>

/**
 * 读取 settings.conf 中的布尔配置。
 * 配置文件或键不存在时返回 default_value。
 * @param group 配置组名，例如 "Preferences"
 * @param key 键名
 * @param default_value 缺省值
 */
gboolean prefs_get_boolean(const char *group, const char *key, gboolean default_value);

/**
 * 写入 settings.conf 中的布尔配置，保留文件中已有的其它组和键。
 * @param group 配置组名，例如 "Preferences"
 * @param key 键名
 * @param value 要保存的值
 * @return TRUE 保存成功，FALSE 失败
 */
gboolean prefs_set_boolean(const char *group, const char *key, gboolean value);

#endif // PREFS_H
