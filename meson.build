project('ygo-deck-builder', 'c',
  version: '0.2.5',
  license: 'MIT',
  meson_version: '>=0.59.0',
  default_options: [
    'warning_level=2',
    'c_std=c11',
  ],
)

project_id = 'com.pai535.YGODeckBuilder'
project_name = 'YGODBR'

glib_dep = dependency('glib-2.0')
gtk4_dep = dependency('gtk4', version: '>=4.8')
adw_dep = dependency('libadwaita-1', version: '>=1.2')
json_glib_dep = dependency('json-glib-1.0')
libsoup_dep = dependency('libsoup-3.0')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')
libarchive_dep = dependency('libarchive')
sqlite3_dep = dependency('sqlite3')

deps = [gtk4_dep, adw_dep, json_glib_dep, libsoup_dep, gdk_pixbuf_dep, libarchive_dep, sqlite3_dep]

subdir('src')

# 单元测试：deck_url 编解码往返与协议边界
test_deck_url = executable('test_deck_url',
  'tests/test_deck_url.c',
  'src/deck_url.c',
  include_directories: ['src'],
  dependencies: [json_glib_dep],
)
test('deck_url', test_deck_url)

# 单元测试：压缩包条目名路径安全（Zip Slip / 路径穿越）
test_archive_path = executable('test_archive_path',
  'tests/test_archive_path.c',
  'src/archive_path.c',
  include_directories: ['src'],
  dependencies: [glib_dep],
)
test('archive_path', test_archive_path)

# 单元测试：JsonNode 深拷贝独立性（防止浅拷贝污染共享解析缓存）
test_json_utils = executable('test_json_utils',
  'tests/test_json_utils.c',
  'src/json_utils.c',
  include_directories: ['src'],
  dependencies: [json_glib_dep],
)
test('json_utils', test_json_utils)

# 单元测试：异画索引与用户选择 JSON（搜索结果异画切换）
test_alt_art_index = executable('test_alt_art_index',
  'tests/test_alt_art_index.c',
  'src/alt_art_index.c',
  include_directories: ['src'],
  dependencies: [glib_dep, json_glib_dep],
)
test('alt_art_index', test_alt_art_index)

# 单元测试：异画映射加载与“默认异画”选择持久化
test_alt_art = executable('test_alt_art',
  'tests/test_alt_art.c',
  'tests/test_app_path_stub.c',
  'src/alt_art.c',
  'src/alt_art_index.c',
  'src/offline_data.c',
  'src/archive_path.c',
  include_directories: ['src'],
  dependencies: [glib_dep, json_glib_dep, libsoup_dep, libarchive_dep],
)
test('alt_art', test_alt_art)

# 单元测试：取消“启用先行卡”后清空本地先行卡数据
test_prerelease_clear = executable('test_prerelease_clear',
  'tests/test_prerelease_clear.c',
  'tests/test_app_path_stub.c',
  'src/prerelease.c',
  'src/archive_path.c',
  include_directories: ['src'],
  dependencies: [glib_dep, json_glib_dep, libsoup_dep, libarchive_dep, sqlite3_dep],
)
test('prerelease_clear', test_prerelease_clear)

# 单元测试：settings.conf 布尔配置默认值与持久化
test_prefs = executable('test_prefs',
  'tests/test_prefs.c',
  'tests/test_app_path_stub.c',
  'src/prefs.c',
  include_directories: ['src'],
  dependencies: [glib_dep],
)
test('prefs', test_prefs)

# 单元测试：strings.conf 解析与缓存（字段筛选性能修复）
test_card_info = executable('test_card_info',
  'tests/test_card_info.c',
  'tests/test_app_path_stub.c',
  'src/card_info.c',
  include_directories: ['src'],
  dependencies: [glib_dep],
)
test('card_info', test_card_info)

# 配置并安装 desktop 文件
desktop_conf = configuration_data()
desktop_conf.set('BINDIR', get_option('prefix') / get_option('bindir'))

desktop_file = configure_file(
  input: 'data/com.pai535.YGODeckBuilder.desktop',
  output: 'com.pai535.YGODeckBuilder.desktop',
  configuration: desktop_conf,
  install: true,
  install_dir: get_option('datadir') / 'applications'
)
