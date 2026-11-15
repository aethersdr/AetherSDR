subdir('shared')

subdir('processors')

specbleach_sources = [shared_sources, processors_sources]

src_inc = include_directories('.')

# Build of the shared object
libspecbleach = library('specbleach',
  sources: specbleach_sources,
  c_args: lib_c_args,
  link_args: lib_link_args,
  dependencies: dep,
  include_directories: [inc, src_inc],
  version: meson.project_version(),
  soversion: '0',
  install: true)

# Generate package module configuration
pkg_mod = import('pkgconfig')
pkg_mod.generate(libraries: libspecbleach,
                 version: meson.project_version(),
                 name: 'libspecbleach',
                 description: 'C library for audio noise reduction and other spectral effects')

# Note: libspecbleach_dep is now declared in the root meson.build