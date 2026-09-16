// 单元测试用的 app_path 替身。
// card_info.c 仅在 get_setcode_from_field_name() 的默认路径分支里使用这两个函数；
// 测试直接调用带显式 path 的 strings_conf_lookup_cached()，因此这里只需能链接。
#include "app_path.h"

const char* get_program_directory(void) { return "/nonexistent"; }
gboolean is_portable_mode(void) { return FALSE; }
