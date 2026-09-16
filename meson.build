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
