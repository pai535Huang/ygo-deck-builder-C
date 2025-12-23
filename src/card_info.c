// card_info.c
// 用于分解卡片类别信息
#include "card_info.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>

// constant.lua TYPE 部分参考
#define TYPE_MONSTER      0x1
#define TYPE_SPELL        0x2
#define TYPE_TRAP         0x4
#define TYPE_NORMAL       0x10
#define TYPE_EFFECT       0x20
#define TYPE_FUSION       0x40
#define TYPE_RITUAL       0x80
#define TYPE_TRAPMONSTER  0x100
#define TYPE_SPIRIT       0x200
#define TYPE_UNION        0x400
#define TYPE_DUAL         0x800
#define TYPE_TUNER        0x1000
#define TYPE_SYNCHRO      0x2000
#define TYPE_TOKEN        0x4000
#define TYPE_QUICKPLAY    0x10000
#define TYPE_CONTINUOUS   0x20000
#define TYPE_EQUIP        0x40000
#define TYPE_FIELD        0x80000
#define TYPE_COUNTER      0x100000
#define TYPE_FLIP         0x200000
#define TYPE_TOON         0x400000
#define TYPE_XYZ          0x800000
#define TYPE_PENDULUM     0x1000000
#define TYPE_SPSUMMON     0x2000000
#define TYPE_LINK         0x4000000

// constant.lua ATTRIBUTE 部分参考
#define ATTRIBUTE_EARTH   0x01
#define ATTRIBUTE_WATER   0x02
#define ATTRIBUTE_FIRE    0x04
#define ATTRIBUTE_WIND    0x08
#define ATTRIBUTE_LIGHT   0x10
#define ATTRIBUTE_DARK    0x20
#define ATTRIBUTE_DIVINE  0x40

// constant.lua RACE 部分参考
#define RACE_WARRIOR        0x1
#define RACE_SPELLCASTER    0x2
#define RACE_FAIRY          0x4
#define RACE_FIEND          0x8
#define RACE_ZOMBIE         0x10
#define RACE_MACHINE        0x20
#define RACE_AQUA           0x40
#define RACE_PYRO           0x80
#define RACE_ROCK           0x100
#define RACE_WINDBEAST      0x200
#define RACE_PLANT          0x400
#define RACE_INSECT         0x800
#define RACE_THUNDER        0x1000
#define RACE_DRAGON         0x2000
#define RACE_BEAST          0x4000
#define RACE_BEASTWARRIOR   0x8000
#define RACE_DINOSAUR       0x10000
#define RACE_FISH           0x20000
#define RACE_SEASERPENT     0x40000
#define RACE_REPTILE        0x80000
#define RACE_PSYCHO         0x100000
#define RACE_DIVINE         0x200000
#define RACE_CREATORGOD     0x400000
#define RACE_WYRM           0x800000
#define RACE_CYBERSE        0x1000000
#define RACE_ILLUSION       0x2000000

// constant.lua LINK_MARKER 部分参考
#define LINK_MARKER_BOTTOM_LEFT   0x001
#define LINK_MARKER_BOTTOM        0x002
#define LINK_MARKER_BOTTOM_RIGHT  0x004
#define LINK_MARKER_LEFT          0x008
#define LINK_MARKER_RIGHT         0x020
#define LINK_MARKER_TOP_LEFT      0x040
#define LINK_MARKER_TOP           0x080
#define LINK_MARKER_TOP_RIGHT     0x100

// 返回类别字符串，多个类别用逗号分隔
void get_card_types(uint32_t type, char* out, size_t out_size) {
    out[0] = '\0';
    int first = 1;
    struct {
        uint32_t value;
        const char* name;
    } type_map[] = {
        {TYPE_MONSTER, "怪兽"},
        {TYPE_SPELL, "魔法"},
        {TYPE_TRAP, "陷阱"},
        {TYPE_NORMAL, "通常"},
        {TYPE_EFFECT, "效果"},
        {TYPE_FUSION, "融合"},
        {TYPE_RITUAL, "仪式"},
        {TYPE_TRAPMONSTER, "陷阱怪兽"},
        {TYPE_SPIRIT, "灵魂"},
        {TYPE_UNION, "同盟"},
        {TYPE_DUAL, "二重"},
        {TYPE_TUNER, "调整"},
        {TYPE_SYNCHRO, "同调"},
        {TYPE_TOKEN, "衍生物"},
        {TYPE_QUICKPLAY, "速攻"},
        {TYPE_CONTINUOUS, "永续"},
        {TYPE_EQUIP, "装备"},
        {TYPE_FIELD, "场地"},
        {TYPE_COUNTER, "反击"},
        {TYPE_FLIP, "反转"},
        {TYPE_TOON, "卡通"},
        {TYPE_XYZ, "超量"},
        {TYPE_PENDULUM, "灵摆"},
        {TYPE_SPSUMMON, "特殊召唤"},
        {TYPE_LINK, "连接"}
    };
    size_t n_types = sizeof(type_map) / sizeof(type_map[0]);
    for (size_t i = 0; i < n_types; ++i) {
        if (type & type_map[i].value) {
            if (!first) strncat(out, ",", out_size - strlen(out) - 1);
            strncat(out, type_map[i].name, out_size - strlen(out) - 1);
            first = 0;
        }
    }
}

// 返回属性字符串
void get_card_attribute(uint32_t attribute, char* out, size_t out_size) {
    out[0] = '\0';
    struct {
        uint32_t value;
        const char* name;
    } attribute_map[] = {
        {ATTRIBUTE_EARTH, "地"},
        {ATTRIBUTE_WATER, "水"},
        {ATTRIBUTE_FIRE, "炎"},
        {ATTRIBUTE_WIND, "风"},
        {ATTRIBUTE_LIGHT, "光"},
        {ATTRIBUTE_DARK, "暗"},
        {ATTRIBUTE_DIVINE, "神"}
    };
    size_t n_attributes = sizeof(attribute_map) / sizeof(attribute_map[0]);
    for (size_t i = 0; i < n_attributes; ++i) {
        if (attribute == attribute_map[i].value) {
            strncpy(out, attribute_map[i].name, out_size - 1);
            out[out_size - 1] = '\0';
            return;
        }
    }
    // 如果没有匹配，返回空字符串
}

// 返回种族字符串
void get_card_race(uint32_t race, char* out, size_t out_size) {
    out[0] = '\0';
    struct {
        uint32_t value;
        const char* name;
    } race_map[] = {
        {RACE_WARRIOR, "战士"},
        {RACE_SPELLCASTER, "魔法师"},
        {RACE_FAIRY, "天使"},
        {RACE_FIEND, "恶魔"},
        {RACE_ZOMBIE, "不死"},
        {RACE_MACHINE, "机械"},
        {RACE_AQUA, "水"},
        {RACE_PYRO, "炎"},
        {RACE_ROCK, "岩石"},
        {RACE_WINDBEAST, "鸟兽"},
        {RACE_PLANT, "植物"},
        {RACE_INSECT, "昆虫"},
        {RACE_THUNDER, "雷"},
        {RACE_DRAGON, "龙"},
        {RACE_BEAST, "兽"},
        {RACE_BEASTWARRIOR, "兽战士"},
        {RACE_DINOSAUR, "恐龙"},
        {RACE_FISH, "鱼"},
        {RACE_SEASERPENT, "海龙"},
        {RACE_REPTILE, "爬虫类"},
        {RACE_PSYCHO, "念动力"},
        {RACE_DIVINE, "幻神兽"},
        {RACE_CREATORGOD, "创造神"},
        {RACE_WYRM, "幻龙"},
        {RACE_CYBERSE, "电子界"},
        {RACE_ILLUSION, "幻想魔"}
    };
    size_t n_races = sizeof(race_map) / sizeof(race_map[0]);
    for (size_t i = 0; i < n_races; ++i) {
        if (race == race_map[i].value) {
            strncpy(out, race_map[i].name, out_size - 1);
            out[out_size - 1] = '\0';
            return;
        }
    }
    // 如果没有匹配，返回空字符串
}

// 从 level 字段解析等级和灵摆刻度
// level 是 32 位整数：第 0-7 位为等级，第 16-23 位为右刻度，第 24-31 位为左刻度
void parse_level_and_scales(uint32_t level, int* out_level, int* left_scale, int* right_scale) {
    if (out_level) {
        *out_level = level & 0xFF;  // 第 0-7 位
    }
    if (right_scale) {
        *right_scale = (level >> 16) & 0xFF;  // 第 16-23 位
    }
    if (left_scale) {
        *left_scale = (level >> 24) & 0xFF;  // 第 24-31 位
    }
}

// 从连接怪兽的 def 字段解析连接箭头
// 返回箭头描述字符串，多个箭头用逗号分隔
void get_link_markers(uint32_t def_value, char* out, size_t out_size) {
    out[0] = '\0';
    int first = 1;
    struct {
        uint32_t value;
        const char* name;
    } marker_map[] = {
        {LINK_MARKER_TOP_LEFT, "↖"},
        {LINK_MARKER_TOP, "↑"},
        {LINK_MARKER_TOP_RIGHT, "↗"},
        {LINK_MARKER_LEFT, "←"},
        {LINK_MARKER_RIGHT, "→"},
        {LINK_MARKER_BOTTOM_LEFT, "↙"},
        {LINK_MARKER_BOTTOM, "↓"},
        {LINK_MARKER_BOTTOM_RIGHT, "↘"}
    };
    size_t n_markers = sizeof(marker_map) / sizeof(marker_map[0]);
    for (size_t i = 0; i < n_markers; ++i) {
        if (def_value & marker_map[i].value) {
            if (!first) strncat(out, ",", out_size - strlen(out) - 1);
            strncat(out, marker_map[i].name, out_size - strlen(out) - 1);
            first = 0;
        }
    }
}

// 从魔法类别字符串获取对应的type值
// 返回值：TYPE_SPELL 与具体类别的组合
// category: "全部"返回 TYPE_SPELL
//          "通常"返回 TYPE_SPELL
//          "仪式"返回 TYPE_SPELL | TYPE_RITUAL
//          "速攻"返回 TYPE_SPELL | TYPE_QUICKPLAY
//          "永续"返回 TYPE_SPELL | TYPE_CONTINUOUS
//          "装备"返回 TYPE_SPELL | TYPE_EQUIP
//          "场地"返回 TYPE_SPELL | TYPE_FIELD
uint32_t get_spell_type_from_category(const char* category) {
    if (!category) return TYPE_SPELL;
    
    if (strcmp(category, "全部") == 0) {
        return TYPE_SPELL;
    } else if (strcmp(category, "通常") == 0) {
        return TYPE_SPELL;
    } else if (strcmp(category, "仪式") == 0) {
        return TYPE_SPELL | TYPE_RITUAL;
    } else if (strcmp(category, "速攻") == 0) {
        return TYPE_SPELL | TYPE_QUICKPLAY;
    } else if (strcmp(category, "永续") == 0) {
        return TYPE_SPELL | TYPE_CONTINUOUS;
    } else if (strcmp(category, "装备") == 0) {
        return TYPE_SPELL | TYPE_EQUIP;
    } else if (strcmp(category, "场地") == 0) {
        return TYPE_SPELL | TYPE_FIELD;
    }
    
    return TYPE_SPELL;
}

// 从陷阱类别字符串获取对应的type值
// 返回值：TYPE_TRAP 与具体类别的组合
// category: "全部"返回 TYPE_TRAP
//          "通常"返回 TYPE_TRAP
//          "永续"返回 TYPE_TRAP | TYPE_CONTINUOUS
//          "反击"返回 TYPE_TRAP | TYPE_COUNTER
uint32_t get_trap_type_from_category(const char* category) {
    if (!category) return TYPE_TRAP;
    
    if (strcmp(category, "全部") == 0) {
        return TYPE_TRAP;
    } else if (strcmp(category, "通常") == 0) {
        return TYPE_TRAP;
    } else if (strcmp(category, "永续") == 0) {
        return TYPE_TRAP | TYPE_CONTINUOUS;
    } else if (strcmp(category, "反击") == 0) {
        return TYPE_TRAP | TYPE_COUNTER;
    }
    
    return TYPE_TRAP;
}

// 从属性字符串获取对应的attribute值
uint32_t get_attribute_from_string(const char* attribute) {
    if (!attribute) return 0;
    
    if (strcmp(attribute, "全部") == 0) {
        return 0;  // 0 表示不筛选
    } else if (strcmp(attribute, "地") == 0) {
        return ATTRIBUTE_EARTH;   // 0x01
    } else if (strcmp(attribute, "水") == 0) {
        return ATTRIBUTE_WATER;   // 0x02
    } else if (strcmp(attribute, "炎") == 0) {
        return ATTRIBUTE_FIRE;    // 0x04
    } else if (strcmp(attribute, "风") == 0) {
        return ATTRIBUTE_WIND;    // 0x08
    } else if (strcmp(attribute, "光") == 0) {
        return ATTRIBUTE_LIGHT;   // 0x10
    } else if (strcmp(attribute, "暗") == 0) {
        return ATTRIBUTE_DARK;    // 0x20
    } else if (strcmp(attribute, "神") == 0) {
        return ATTRIBUTE_DIVINE;  // 0x40
    }
    
    return 0;
}

// 从种族字符串获取对应的race值
uint32_t get_race_from_string(const char* race) {
    if (!race) return 0;
    
    if (strcmp(race, "全部") == 0) {
        return 0;  // 0 表示不筛选
    } else if (strcmp(race, "战士") == 0) {
        return RACE_WARRIOR;        // 0x1
    } else if (strcmp(race, "魔法师") == 0) {
        return RACE_SPELLCASTER;    // 0x2
    } else if (strcmp(race, "天使") == 0) {
        return RACE_FAIRY;          // 0x4
    } else if (strcmp(race, "恶魔") == 0) {
        return RACE_FIEND;          // 0x8
    } else if (strcmp(race, "不死") == 0) {
        return RACE_ZOMBIE;         // 0x10
    } else if (strcmp(race, "机械") == 0) {
        return RACE_MACHINE;        // 0x20
    } else if (strcmp(race, "水") == 0) {
        return RACE_AQUA;           // 0x40
    } else if (strcmp(race, "炎") == 0) {
        return RACE_PYRO;           // 0x80
    } else if (strcmp(race, "岩石") == 0) {
        return RACE_ROCK;           // 0x100
    } else if (strcmp(race, "鸟兽") == 0) {
        return RACE_WINDBEAST;      // 0x200
    } else if (strcmp(race, "植物") == 0) {
        return RACE_PLANT;          // 0x400
    } else if (strcmp(race, "昆虫") == 0) {
        return RACE_INSECT;         // 0x800
    } else if (strcmp(race, "雷") == 0) {
        return RACE_THUNDER;        // 0x1000
    } else if (strcmp(race, "龙") == 0) {
        return RACE_DRAGON;         // 0x2000
    } else if (strcmp(race, "兽") == 0) {
        return RACE_BEAST;          // 0x4000
    } else if (strcmp(race, "兽战士") == 0) {
        return RACE_BEASTWARRIOR;   // 0x8000
    } else if (strcmp(race, "恐龙") == 0) {
        return RACE_DINOSAUR;       // 0x10000
    } else if (strcmp(race, "鱼") == 0) {
        return RACE_FISH;           // 0x20000
    } else if (strcmp(race, "海龙") == 0) {
        return RACE_SEASERPENT;     // 0x40000
    } else if (strcmp(race, "爬虫类") == 0) {
        return RACE_REPTILE;        // 0x80000
    } else if (strcmp(race, "念动力") == 0) {
        return RACE_PSYCHO;         // 0x100000
    } else if (strcmp(race, "幻神兽") == 0) {
        return RACE_DIVINE;         // 0x200000
    } else if (strcmp(race, "创造神") == 0) {
        return RACE_CREATORGOD;     // 0x400000
    } else if (strcmp(race, "幻龙") == 0) {
        return RACE_WYRM;           // 0x800000
    } else if (strcmp(race, "电子界") == 0) {
        return RACE_CYBERSE;        // 0x1000000
    } else if (strcmp(race, "幻想魔") == 0) {
        return RACE_ILLUSION;       // 0x2000000
    }
    
    return 0;
}

// 获取strings.conf文件路径
static gchar* get_strings_conf_path(void) {
    // 从offline_data模块获取cards目录路径
    // 这里需要包含app_path.h和offline_data相关的逻辑
    const char *data_home = g_get_user_data_dir();
    return g_build_filename(data_home, "ygo-deck-builder", "cards", "strings.conf", NULL);
}

// 从字段名字符串获取对应的setcode值（十进制）
uint64_t get_setcode_from_field_name(const char* field_name) {
    if (!field_name || field_name[0] == '\0') {
        return 0;
    }
    
    gchar *strings_path = get_strings_conf_path();
    if (!strings_path || !g_file_test(strings_path, G_FILE_TEST_EXISTS)) {
        g_free(strings_path);
        return 0;
    }
    
    // 读取strings.conf文件
    gchar *content = NULL;
    GError *error = NULL;
    if (!g_file_get_contents(strings_path, &content, NULL, &error)) {
        if (error) {
            g_warning("Failed to read strings.conf: %s", error->message);
            g_error_free(error);
        }
        g_free(strings_path);
        return 0;
    }
    g_free(strings_path);
    
    // 逐行解析文件
    gchar **lines = g_strsplit(content, "\n", -1);
    g_free(content);
    
    uint64_t result = 0;
    for (int i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strstrip(lines[i]);
        
        // 跳过空行和注释
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        
        // 查找以"!setname"开头的行
        if (g_str_has_prefix(line, "!setname")) {
            // 格式：!setname 0x十六进制 字段名
            gchar **parts = g_strsplit(line, " ", 3);
            if (g_strv_length(parts) >= 3) {
                const gchar *hex_str = parts[1];
                const gchar *name = parts[2];
                
                // 检查字段名是否匹配
                if (name && strstr(name, field_name) != NULL) {
                    // 解析十六进制数（去掉"0x"前缀）
                    if (g_str_has_prefix(hex_str, "0x") || g_str_has_prefix(hex_str, "0X")) {
                        result = g_ascii_strtoull(hex_str + 2, NULL, 16);
                        g_strfreev(parts);
                        break;
                    }
                }
            }
            g_strfreev(parts);
        }
    }
    
    g_strfreev(lines);
    return result;
}

// 检查卡片的setcode是否匹配给定的字段名
gboolean match_setcode_with_field(uint64_t card_setcode, const char* field_name) {
    if (!field_name || field_name[0] == '\0') {
        return TRUE;  // 空字段名视为不筛选
    }
    
    if (card_setcode == 0) {
        return FALSE;  // 卡片没有setcode
    }
    
    // 获取字段名对应的setcode
    uint64_t target_setcode = get_setcode_from_field_name(field_name);
    if (target_setcode == 0) {
        return FALSE;  // 未找到对应的字段
    }
    
    // setcode是64位整数，可能包含多个字段代码（每16位一个）
    // 需要逐个检查
    for (int shift = 0; shift < 64; shift += 16) {
        uint64_t code_part = (card_setcode >> shift) & 0xFFFF;
        if (code_part == 0) {
            break;  // 没有更多字段代码
        }
        
        // 检查是否匹配（考虑掩码）
        uint64_t target_masked = target_setcode & 0xFFF;  // 取低12位作为实际代码
        uint64_t card_masked = code_part & 0xFFF;
        
        if (card_masked == target_masked) {
            return TRUE;
        }
    }
    
    return FALSE;
}
