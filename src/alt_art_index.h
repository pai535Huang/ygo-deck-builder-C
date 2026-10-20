#ifndef ALT_ART_INDEX_H
#define ALT_ART_INDEX_H

#include <glib.h>

/**
 * 异画索引：保存“本体卡 ID -> 异画 ID 列表”以及反向映射。
 *
 * 这里只做纯内存数据操作（不联网、不读写文件），便于单元测试。
 * 运行时缓存、下载与选择持久化由 alt_art.c 负责。
 */
typedef struct AltArtIndex AltArtIndex;

AltArtIndex *alt_art_index_new(void);
void alt_art_index_free(AltArtIndex *index);

/**
 * 添加一条异画映射。art_id 与 base_id 相同或非正数时忽略。
 * @return TRUE 表示新插入，FALSE 表示重复或无效
 */
gboolean alt_art_index_add(AltArtIndex *index, int base_id, int art_id);

/** 对每个本体的异画列表排序并去重（构建完成后调用一次）。 */
void alt_art_index_sort(AltArtIndex *index);

/**
 * 获取某张卡的完整版本列表（本体在前，异画按 ID 升序）。
 * @return 新的 GArray（元素为 int），调用者需 g_array_unref；没有异画返回 NULL
 */
GArray *alt_art_index_get_variants(AltArtIndex *index, int base_id);

/** 异画 ID -> 本体 ID；不是已知异画时原样返回 art_id。 */
int alt_art_index_get_base(AltArtIndex *index, int art_id);

/** 该本体是否有可用异画。 */
gboolean alt_art_index_has_variants(AltArtIndex *index, int base_id);

/** 有异画的本体数量。 */
guint alt_art_index_base_count(AltArtIndex *index);

/**
 * 从 {"version":1,"bases":{"89631139":[89631140,...]}} 解析。
 * @return 成功 TRUE；JSON 无效或结构不对返回 FALSE 并设置 error
 */
gboolean alt_art_index_load_json(AltArtIndex *index, const char *json, GError **error);

/** 序列化为上面的 JSON 结构；返回新分配字符串，调用者 g_free。 */
gchar *alt_art_index_to_json(AltArtIndex *index);

/**
 * 解析用户选择：{"choices":{"14558127":14558129}}。
 * 无效输入返回空表（不是 NULL），元素为 int -> int。
 */
GHashTable *alt_art_choices_parse_json(const char *json);

/** 序列化用户选择；空表输出 {"choices":{}}。 */
gchar *alt_art_choices_to_json(GHashTable *choices);

/**
 * 从卡图 CDN 的 metadata 文本中提取“不在 base_ids 里的图片 ID”。
 * base_ids 是 int -> int 的哈希表（值可任意），键为已知本体卡 ID。
 * @return 新的 GArray（元素 int），调用者 g_array_unref；无候选返回空数组
 */
GArray *alt_art_parse_image_metadata(const char *metadata, GHashTable *base_ids);

/**
 * 把 ygocdb /api/v0/cardset 的响应合并进索引。
 * 只接受 altart == 请求键、且本体存在于 base_ids 的条目。
 * @return 新增的异画数量
 */
guint alt_art_apply_cardset_json(AltArtIndex *index, const char *json, GHashTable *base_ids);

#endif // ALT_ART_INDEX_H
