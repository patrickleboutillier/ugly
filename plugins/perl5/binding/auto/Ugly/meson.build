# Built here so it lands at <build>/plugins/perl5/binding/auto/Ugly/Ugly.so,
# where XSLoader finds it through @INC.

perl_binding = shared_module('Ugly',
  '../../../perl5.c', helper_pm_h,
  name_prefix: '',
  c_args: perl_ccopts + ['-DUGLY_BINDING'],
  dependencies: ugly_dep,
  install: false,
)
bindings += perl_binding
