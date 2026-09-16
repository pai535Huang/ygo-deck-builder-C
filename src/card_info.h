// card_info.h
#ifndef CARD_INFO_H
#define CARD_INFO_H
#include <stdint.h>
#include <stddef.h>
#include <glib.h>

// 将十进制 type 分解为类别字符串，写入 out，多个类别用逗号分隔
void get_card_types(uint32_t type, char* out, size_t out_size);

// 将十进制 attribute 转换为属性字符串，写入 out
void get_card_attribute(uint32_t attribute, char* out, size_t out_size);

// 将十进制 race 转换为种族字符串，写入 out
void get_card_race(uint32_t race, char* out, size_t out_size);

// 从 level 字段解析等级和灵摆刻度
// level 是 32 位整数：第 0-7 位为等级，第 16-23 位为右刻度，第 24-31 位为左刻度
// 传入 NULL 可以跳过某个输出参数
void parse_level_and_scales(uint32_t level, int* out_level, int* left_scale, int* right_scale);

// 从连接怪兽的 def 字段解析连接箭头，写入 out，多个箭头用逗号分隔
void get_link_markers(uint32_t def_value, char* out, size_t out_size);

// 从魔法类别字符串获取对应的type值
// category: "全部", "通常", "仪式", "速攻", "永续", "装备", "场地"
// 返回值：TYPE_SPELL 与具体类别的组合
uint32_t get_spell_type_from_category(const char* category);

// 从陷阱类别字符串获取对应的type值
// category: "全部", "通常", "永续", "反击"
// 返回值：TYPE_TRAP 与具体类别的组合
uint32_t get_trap_type_from_category(const char* category);

// 从属性字符串获取对应的attribute值
// attribute: "全部", "地", "水", "炎", "风", "光", "暗", "神"
// 返回值：ATTRIBUTE 常量
uint32_t get_attribute_from_string(const char* attribute);

// 从种族字符串获取对应的race值
// race: "全部", "战士", "魔法师", ...
// 返回值：RACE 常量
uint32_t get_race_from_string(const char* race);

// 从字段名字符串获取对应的setcode值（十进制）
// 通过解析strings.conf文件，将字段名映射为十六进制数，再转换为十进制
// field_name: 字段名（如"青眼", "真红眼", "英雄"等）
// 返回值：对应的setcode十进制值，如果未找到返回0
uint64_t get_setcode_from_field_name(const char* field_name);

// 解析 strings.conf 的文本内容，返回字段名对应的 setcode（首个匹配项）；未找到返回 0。
// 纯函数（不读文件），便于单元测试；匹配语义与原来的逐行解析完全一致：
// 仅处理 "!setname 0x... 名称" 行，名称用 strstr 做子串匹配，首个匹配即返回。
uint64_t strings_conf_parse_setcode(const char *content, const char *field_name);

// 带缓存的 setcode 查询：按 (path, mtime, size) 失效，并按字段名记忆化结果。
// strings.conf 约 47KB，逐卡重新读取+解析在 13k 张卡的扫描里约浪费 3.7 秒 CPU，
// 因此首次查询后缓存；文件被更新（重新下载 strings.conf）时自动失效。
// path 为 strings.conf 路径；返回 0 表示未找到。
uint64_t strings_conf_lookup_cached(const char *path, const char *field_name);

// 检查卡片的setcode是否匹配给定的字段名
// card_setcode: 卡片的setcode字段值（十进制）
// field_name: 要匹配的字段名
// 返回值：TRUE表示匹配，FALSE表示不匹配
gboolean match_setcode_with_field(uint64_t card_setcode, const char* field_name);

#endif // CARD_INFO_H
