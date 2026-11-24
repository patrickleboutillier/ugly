# The XS module itself. It lives in this directory only so that its build
# output is <build>/lib/perl5/auto/Ugly/Ugly.so (see ../../meson.build).

ugly_xs = shared_module('Ugly',
  ugly_xs_c,
  name_prefix: '',
  name_suffix: 'so',
  c_args: perl_ccopts + [
    '-DVERSION="@0@"'.format(perl_xs_version),
    '-DXS_VERSION="@0@"'.format(perl_xs_version),
  ],
  include_directories: include_directories('../..'),
  dependencies: libugly_dep,
  install: true,
  install_dir: perl_installdir / 'auto' / 'Ugly',
  install_rpath: get_option('prefix') / get_option('libdir'),
)
