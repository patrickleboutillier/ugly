# The perl5 plugin: embeds perl interpreters. Loaded by ugly_language_init()
# with dlopen("libugly_perl5.so").

perl_embed_check = run_command(perl, '-MExtUtils::Embed', '-e', '1', check: false)
if perl_embed_check.returncode() != 0
  error('perl\'s ExtUtils::Embed is required to build the perl5 plugin')
endif
perl_ccopts = run_command(perl, '-MExtUtils::Embed', '-e', 'ccopts', check: true).stdout().strip().split()
perl_ldopts = run_command(perl, '-MExtUtils::Embed', '-e', 'ldopts', check: true).stdout().strip().split()

libugly_perl5 = shared_module('ugly_perl5',
  'utils.c', 'language.c', 'runtime.c', 'object.c',
  c_args: perl_ccopts,
  link_args: perl_ldopts,
  dependencies: libugly_dep,
  install: false,
)

# Helper code, loaded at runtime from <UGLY_LANGUAGE_DIR>/perl5/helper/ (the
# source tree's lang/ in tests).

plugins += libugly_perl5
plugin_dirs += meson.current_build_dir()
