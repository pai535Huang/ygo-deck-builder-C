# 安装说明

本程序支持两种运行模式：

## 1. 便携模式（Portable Mode）

适合不想安装到系统的用户，所有数据文件都存储在程序目录下。

**启用方法：**
在可执行文件所在目录创建一个名为 `.portable` 的空文件：

```bash
cd build/src
touch .portable
./ygo-deck-builder
```

**目录结构：**
```
ygo-deck-builder/
├── ygo-deck-builder       # 可执行文件
├── .portable              # 便携模式标记文件
├── settings.conf          # 配置文件
├── img/                   # 图片缓存
│   ├── 12345.png
│   └── ...
└── data/                  # 数据目录
    ├── ocg_forbidden.json
    ├── tcg_forbidden.json
    ├── sc_forbidden.json
    ├── cards/             # 离线卡片数据
    └── pre-release/       # 先行卡数据
```

## 2. 系统安装模式（System Install Mode）

符合 Linux 桌面标准（XDG Base Directory），适合系统级安装。

**安装方法：**

```bash
# 配置构建（安装到 /usr/local）
meson setup builddir --prefix=/usr/local

# 编译
meson compile -C builddir

# 安装到系统（需要 root 权限）
sudo meson install -C builddir
```

**文件位置：**
- **可执行文件**：`/usr/local/bin/ygo-deck-builder`
- **配置文件**：`~/.config/ygo-deck-builder/settings.conf`
- **数据文件**：`~/.local/share/ygo-deck-builder/`
  - 禁限卡表：`~/.local/share/ygo-deck-builder/ocg_forbidden.json` 等
  - 离线数据：`~/.local/share/ygo-deck-builder/cards/`
  - 先行卡：`~/.local/share/ygo-deck-builder/pre-release/`
- **图片缓存**：`~/.cache/ygo-deck-builder/images/`

**优点：**
- 符合 Linux 文件系统标准
- 支持多用户，每个用户有独立的配置和数据
- 可以从任何位置运行程序
- 遵循 XDG Base Directory 规范

**卸载：**
```bash
# 删除程序文件
sudo rm /usr/local/bin/ygo-deck-builder

# 删除用户数据（可选）
rm -rf ~/.config/ygo-deck-builder
rm -rf ~/.local/share/ygo-deck-builder
rm -rf ~/.cache/ygo-deck-builder
```

## 环境变量支持

系统安装模式遵循 XDG 环境变量，可以自定义数据位置：

- `XDG_CONFIG_HOME` - 配置文件位置（默认：`~/.config`）
- `XDG_DATA_HOME` - 数据文件位置（默认：`~/.local/share`）
- `XDG_CACHE_HOME` - 缓存文件位置（默认：`~/.cache`）

例如：
```bash
export XDG_DATA_HOME=/custom/path/data
ygo-deck-builder
# 数据将存储在 /custom/path/data/ygo-deck-builder/
```

## 模式切换

程序会自动检测运行模式：
- 如果可执行文件所在目录存在 `.portable` 文件 → 便携模式
- 否则 → 系统安装模式

查看运行日志可以确认当前模式：
```bash
# 便携模式会显示：
Running in portable mode (data in program directory)

# 系统安装模式会显示：
Running in system install mode (using XDG directories)
```
