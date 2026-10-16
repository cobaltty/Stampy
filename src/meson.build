cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required: true)

gtk_dep = dependency('gtk4')
gexiv2_dep = dependency('gexiv2', version: '>= 0.14.0', required: true)
libadwaita_dep = dependency('libadwaita-1', version: '>= 1.5')
json_glib_dep = dependency('json-glib-1.0', required: true)
gst_dep = dependency('gstreamer-1.0', required: true)

stampy_deps = [
  gtk_dep,
  libadwaita_dep,
  json_glib_dep,
  gexiv2_dep,
  m_dep,
  gst_dep,
]

stampy_c_args = []

stampy_sources = [
  'main.c',
  'stampy-application.c',
  'stampy-window.c',
  'stampy-utils.c',
  'stampy-handlers.c',
  'stampy-file-handlers.c',
  'stampy-export.c',
  'stampy-config.c',
  'stampy-preferences.c',
  'stampy-preset-handlers.c',
  'engine/stampy-metadata.c',
]

stampy_sources += gnome.compile_resources('stampy-resources',
  'stampy.gresource.xml',
  c_name: 'stampy'
)

executable('stampy', stampy_sources,
  dependencies: stampy_deps,
  c_args: stampy_c_args,
  install: true,
)