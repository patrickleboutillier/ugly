# Builds Fabrice Bellard's QuickJS (the release in ../../quickjs.wrap) as a
# static, position-independent library for the JS plugin. Only the engine:
# not quickjs-libc (std/os modules), qjs or qjsc.

project('quickjs', 'c',
  version: '2026-06-04',
  license: 'MIT',
  meson_version: '>= 0.61',
)

cc = meson.get_compiler('c')
quickjs_deps = [cc.find_library('m', required: false), dependency('threads')]

quickjs_lib = static_library('quickjs',
  'quickjs.c', 'dtoa.c', 'libregexp.c', 'libunicode.c', 'cutils.c',
  c_args: ['-D_GNU_SOURCE', '-DCONFIG_VERSION="' + meson.project_version() + '"', '-w'],
  pic: true,
  dependencies: quickjs_deps,
)

quickjs_dep = declare_dependency(
  link_with: quickjs_lib,
  include_directories: include_directories('.'),
  dependencies: quickjs_deps,
)
