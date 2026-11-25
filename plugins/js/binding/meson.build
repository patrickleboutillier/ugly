# The JS binding: js.c again, as the native ES module ugly.so, for qjs
# programs (import "ugly.so"). It doesn't link QuickJS: its engine symbols come
# from the host qjs, which exports them (-rdynamic). It must come from the
# same QuickJS release as that qjs, since the C API isn't stable across
# releases; the tests use the qjs built from the same subproject.

js_binding = shared_module('ugly',
  '../js.c',
  name_prefix: '',
  c_args: '-DUGLY_BINDING',
  dependencies: [ugly_dep, qjs_dep.partial_dependency(compile_args: true, includes: true)],
  install: false,
)
binding_dirs += { 'js': meson.current_build_dir() }
bindings += js_binding
