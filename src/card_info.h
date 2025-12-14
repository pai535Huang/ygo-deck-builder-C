// card_info.h
#ifndef CARD_INFO_H
#define CARD_INFO_H
#include <stdint.h>
#include <stddef.h>

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

#endif // CARD_INFO_H
