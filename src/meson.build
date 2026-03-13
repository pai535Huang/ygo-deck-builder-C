gnome = import('gnome')

app_resources = gnome.compile_resources(
  'ygo-resources',
  '../resources/ygo-resources.gresource.xml',
  source_dir: '../resources',
  c_name: 'ygo_resources'
)

executable(
  'ygo-deck-builder',
  [
    app_resources,
    'main.c',
    'startup_update.c',
    'prerelease.c',
    'offline_data.c',
    'card_info.c',
    'card_sort.c',
    'card_shuffle.c',
    'deck_slot.c',
    'forbidden_list.c',
    'deck_clear.c',
    'deck_io.c',
    'deck_export_sheet.c',
    'image_loader.c',
    'dnd_manager.c',
    'search_filter.c',
    'deck_url.c'
  ],
  dependencies: deps,
  install: true,
)
