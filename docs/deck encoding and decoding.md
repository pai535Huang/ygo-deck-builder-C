# 卡组URL编码/解码

本项目实现了YGO决斗助手协议（YGO-DA协议）v1.0的卡组URI编码和解码功能。

## 协议说明

基于 [YGO-DA协议文档](https://www.zybuluo.com/feihuaduo/note/1824534)

### URL格式

```
协议://主机?ygotype=deck&v=1&d={Base64Url编码的卡组数据}
```

### 编码算法

1. 统计每个区域（主卡组、额外卡组、副卡组）的不同种类卡片及数量
2. 将每种卡片编码为29位二进制：
   - 前2位：卡片数量（1-3张）
   - 后27位：卡片ID
3. 添加16位头部信息：
   - 前8位：主卡组种类数（0-255）
   - 9-12位：额外卡组种类数（0-15）
   - 13-16位：副卡组种类数（0-15）
4. 将完整的二进制数据转换为Base64Url（使用`-`和`_`替换`+`和`/`）

### 解码算法

1. 从URL提取`d`参数的Base64Url值
2. 解码为二进制数据
3. 读取前16位获取各区域种类数量
4. 按29位为一组读取卡片信息并还原卡组

## API使用

### 头文件

```c
#include "deck_url.h"
```

#### 编码函数

```c
char* deck_encode_to_url(
    const int *main_cards, int main_count,
    const int *extra_cards, int extra_count,
    const int *side_cards, int side_count,
    const char *base_url
);
```

**参数：**
- `main_cards`: 主卡组卡片ID数组
- `main_count`: 主卡组卡片数量
- `extra_cards`: 额外卡组卡片ID数组
- `extra_count`: 额外卡组卡片数量
- `side_cards`: 副卡组卡片ID数组
- `side_count`: 副卡组卡片数量
- `base_url`: 基础URL（如 "https://example.com/deck"），NULL则使用默认值

**返回值：**
- 成功：完整的卡组URL字符串（需要使用`g_free()`释放）
- 失败：NULL

**示例：**

```c
int main_cards[] = {89631139, 89631139, 89631139};
int extra_cards[] = {44508094, 44508094};
int side_cards[] = {14087893};

char *url = deck_encode_to_url(
    main_cards, 3,
    extra_cards, 2,
    side_cards, 1,
    "https://example.com/deck"
);

if (url) {
    printf("URL: %s\n", url);
    g_free(url);
}
```

### 解码函数

```c
bool deck_decode_from_url(
    const char *url,
    int **main_cards, int *main_count,
    int **extra_cards, int *extra_count,
    int **side_cards, int *side_count,
    GError **error
);
```

**参数：**
- `url`: 卡组URL字符串
- `main_cards`: 输出参数，主卡组卡片ID数组（需要使用`g_free()`释放）
- `main_count`: 输出参数，主卡组卡片数量
- `extra_cards`: 输出参数，额外卡组卡片ID数组（需要使用`g_free()`释放）
- `extra_count`: 输出参数，额外卡组卡片数量
- `side_cards`: 输出参数，副卡组卡片ID数组（需要使用`g_free()`释放）
- `side_count`: 输出参数，副卡组卡片数量
- `error`: 输出参数，错误信息（如果失败）

**返回值：**
- 成功：true
- 失败：false

**示例：**

```c
const char *url = "https://example.com/deck?ygotype=deck&v=1&d=ARHqvU0cqcjvka3tqg";

int *main_cards = NULL, *extra_cards = NULL, *side_cards = NULL;
int main_count = 0, extra_count = 0, side_count = 0;
GError *error = NULL;

if (deck_decode_from_url(url, 
                         &main_cards, &main_count,
                         &extra_cards, &extra_count,
                         &side_cards, &side_count,
                         &error)) {
    printf("主卡组: %d张\n", main_count);
    printf("额外卡组: %d张\n", extra_count);
    printf("副卡组: %d张\n", side_count);
    
    // 使用卡片数据...
    
    g_free(main_cards);
    g_free(extra_cards);
    g_free(side_cards);
} else {
    printf("解码失败: %s\n", error ? error->message : "未知错误");
    if (error) g_error_free(error);
}
```

## 测试验证

已通过以下测试：
- ✅ 简单卡组编码/解码
- ✅ 空卡组处理
- ✅ 复杂卡组（多种类卡片）
- ✅ 真实YGO-DA协议URL解码

测试示例URL：
```
http://deck.ourygo.top?ygotype=deck&v=1&d=FNhefVLXC2RMpY_w-43iOvy2SnXARcGDa4Gf-WWVKlHxmGQN9gbi5Y-FDdvkNIpufUXGkPmlV3n70HzV3OV58le_LRnThgSJlIImmKZAMuPJSqBUEax8yF1rIQy7GidRET65azdhGIVy2w4rI9b5cwTxrZ5JsWGN-uRnxrXZ0jdujdvkOJ9zVDCtMtezkT-6CuKtQso-yA
```

成功解码为包含40张主卡组、15张额外卡组、14张副卡组的完整卡组。

## 注意事项

1. 卡片数量限制：每种卡片最多3张（2位二进制）
2. 种类数量限制：
   - 主卡组：最多255种（8位）
   - 额外卡组：最多15种（4位）
   - 副卡组：最多15种（4位）
3. 卡片ID范围：0-134217727（27位二进制）
4. 自动排序：编码时会自动将相同ID的卡片合并
5. 内存管理：所有返回的字符串和数组都需要调用者使用`g_free()`释放

## 相关文件

- `src/deck_url.h`: 头文件，包含API声明
- `src/deck_url.c`: 实现文件，包含编码/解码算法