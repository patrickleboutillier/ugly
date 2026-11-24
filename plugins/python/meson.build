# The CPython plugin, built as ugly_python.so. Optional: -Dpython=disabled skips it.

python_dep = dependency('python3-embed', required: get_option('python'))

if python_dep.found()
  ugly_python = shared_module('ugly_python',
    'python.c',
    name_prefix: '',
    dependencies: [ugly2_dep, python_dep],
    install: true,
    install_dir: ugly_plugin_dir,
  )
  v2_plugins += ugly_python
  v2_plugin_dirs += meson.current_build_dir()
  v2_test_plugins += 'python'
endif
