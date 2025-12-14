# Yu-Gi-Oh! Deck Builder

一个基于 **GTK4** 和 **libadwaita** 的桌面端游戏王卡组编辑器。

## 项目特色
- 提供完整的卡组编辑器体验，包括查卡、编辑卡组、导入/导出等功能
- 支持在线/离线双模式，可从在线API（https://ygocdb.com/api）/本地数据库获取信息
- 基于 GTK4 和 Adwaita 的现代化界面，适配 Linux 桌面环境
- 自动生成本地图片缓存，降低对在线 API 的占用

## 依赖环境
- GTK4 >= 4.8
- libadwaita >= 1.2
- json-glib
- libsoup-3.0
- gdk-pixbuf-2.0
- libarchive
- sqlite3

## 构建与运行
1. 安装依赖（以 Debian/Ubuntu 为例）：
	```bash
	sudo apt install libgtk-4-dev libadwaita-1-dev libjson-glib-dev libsoup-3.0-dev libgdk-pixbuf-2.0-dev libarchive-dev libsqlite3-dev meson ninja-build
	```
2. 初始化构建目录：
	```bash
	meson setup build
	```
3. 编译项目：
	```bash
	meson compile -C build
	```
4. 运行程序：
	```bash
	./build/src/ygo-deck-builder
	```

## 使用说明
- 启动后可通过搜索栏检索卡片并自由编辑自己的卡组，支持类似 YGOPro 的点击和拖拽互动
- 通过顶部菜单栏进行各种操作
- 禁限卡表自动下载并可切换 OCG/TCG/简中版本，卡组内卡的数量会受到所选择卡表的限制
- 先行卡需手动下载
- 图片缓存默认存储在 ~/.cache/ygo-deck-builder 中，可手动清除
- 可以从文件/URL导入卡组，编辑好的卡组也可以导出为文件/URL

## 待实现
- ~~支持更多种筛选与排序（如限定种族/属性/攻击/守备的检索）~~
- ~~利用 YGO-DA 协议（https://www.zybuluo.com/feihuaduo/note/1824534）将编辑好的卡组导出为分享 URL 或从其他人分享的 URL 直接加载卡组~~
- 修复一些已知 bug
- 进一步优化性能
