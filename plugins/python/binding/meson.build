# The Python binding: python.c again, as the extension module "ugly", for
# import ugly from a plain python3 process. Built as an extension module
# (no libpython link): its Python symbols come from the host interpreter
# (a second CPython in the process would be fatal).

python_binding = pyinst.extension_module('ugly',
  '../python.c',
  c_args: '-DUGLY_BINDING',
  dependencies: [ugly_dep, pyinst.dependency(embed: false)],
  install: true,
)
binding_dirs += { 'python': meson.current_build_dir() }
bindings += python_binding
