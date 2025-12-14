#ifndef APP_PATH_H
#define APP_PATH_H

#include <glib.h>

/**
 * 获取程序所在目录
 * @return 程序所在目录的绝对路径，不需要释放
 */
const char* get_program_directory(void);

/**
 * 检查是否为便携模式
 * @return TRUE 如果是便携模式（数据在程序目录），FALSE 如果使用 XDG 目录
 */
gboolean is_portable_mode(void);

#endif // APP_PATH_H
