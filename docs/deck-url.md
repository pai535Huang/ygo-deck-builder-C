# 卡组分享链接

YGODBR 使用 [YGO-DA v1 卡组协议](https://www.zybuluo.com/feihuaduo/note/1824534)。链接包含 `ygotype=deck`、`v=1` 和承载卡组数据的 Base64url 参数 `d`。

同一区域内相同 ID 的卡片会合并计数，并保留各 ID 首次出现的顺序。导出时，每个 ID 在同一区域最多记录 3 张，超出 27 位范围的 ID 会被跳过。主卡组、额外卡组和副卡组分别最多编码 255、15、15 种 ID，任一区域超出种类上限时，编码失败。

接口见 [`deck_url.h`](../src/deck_url.h)。[编解码测试](../tests/test_deck_url.c) 包含外部工具生成的链接。
