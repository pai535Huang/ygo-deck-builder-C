project('ygo-deck-builder', 'c',
  version: '0.1.0',
  default_options: [
    'warning_level=2',
    'c_std=c11',
  ],
)

gtk4_dep = dependency('gtk4', version: '>=4.8')
adw_dep = dependency('libadwaita-1', version: '>=1.2')
json_glib_dep = dependency('json-glib-1.0')
libsoup_dep = dependency('libsoup-3.0')
gdk_pixbuf_dep = dependency('gdk-pixbuf-2.0')
libarchive_dep = dependency('libarchive')
sqlite3_dep = dependency('sqlite3')

deps = [gtk4_dep, adw_dep, json_glib_dep, libsoup_dep, gdk_pixbuf_dep, libarchive_dep, sqlite3_dep]

subdir('src')
