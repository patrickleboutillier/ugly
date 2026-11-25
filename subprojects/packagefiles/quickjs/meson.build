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

# The qjs interpreter, for the JS binding's tests (a plain qjs program using
# other languages through ugly.so). Linked with export_dynamic, as upstream
# does (-rdynamic): native modules it loads use its engine's symbols.
quickjs_defines = ['-D_GNU_SOURCE', '-DCONFIG_VERSION="' + meson.project_version() + '"', '-w']
libdl = cc.find_library('dl', required: false)

quickjs_libc = static_library('quickjs-libc',
  'quickjs-libc.c',
  c_args: quickjs_defines,
  dependencies: [quickjs_deps, libdl],
)

qjsc = executable('qjsc',
  'qjsc.c',
  c_args: quickjs_defines + ['-DCONFIG_CC="cc"', '-DCONFIG_PREFIX="/usr/local"'],
  link_with: [quickjs_lib, quickjs_libc],
  dependencies: [quickjs_deps, libdl],
  native: true,
)

repl_c = custom_target('repl.c',
  input: 'repl.js',
  output: 'repl.c',
  command: [qjsc, '-s', '-c', '-o', '@OUTPUT@', '-m', '@INPUT@'],
)

qjs = executable('qjs',
  'qjs.c', repl_c,
  c_args: quickjs_defines,
  link_with: [quickjs_lib, quickjs_libc],
  dependencies: [quickjs_deps, libdl],
  export_dynamic: true,
)
