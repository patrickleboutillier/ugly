# The JavaScript plugin (QuickJS), built as ugly_js.so. QuickJS isn't commonly
# packaged, so it comes from the pinned release in subprojects/quickjs.wrap,
# downloaded at setup. Optional: -Djs=disabled skips it.

qjs_dep = dependency('quickjs', required: get_option('js'), allow_fallback: true)

if qjs_dep.found()
  ugly_js = shared_module('ugly_js',
    'js.c',
    name_prefix: '',
    dependencies: [ugly_dep, qjs_dep],
    install: true,
    install_dir: ugly_plugin_dir,
  )
  plugins += ugly_js
  plugin_dirs += meson.current_build_dir()
  test_plugins += 'js'
  subdir('binding')
  # qjs, when QuickJS comes from our subproject: the host for the binding's tests.
  qjs_sub = subproject('quickjs', required: false)
  if qjs_sub.found()
    qjs_exe = qjs_sub.get_variable('qjs')
  endif
endif
