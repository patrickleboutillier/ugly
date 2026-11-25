# The Perl 5 plugin, built as ugly_perl5.so. Needs perl's embedding library
# (libperl-dev). Optional: -Dperl5=disabled skips it.

perl_prog = find_program('perl', required: get_option('perl5'))
perl_embed_ok = false
if perl_prog.found()
  perl_embed_ok = run_command(perl_prog, '-MExtUtils::Embed', '-e', '1', check: false).returncode() == 0
  if not perl_embed_ok and get_option('perl5').enabled()
    error('the perl5 plugin needs perl\'s ExtUtils::Embed (libperl-dev)')
  endif
endif

if perl_embed_ok
  perl_ccopts = run_command(perl_prog, '-MExtUtils::Embed', '-e', 'ccopts', check: true).stdout().strip().split()
  perl_ldopts = run_command(perl_prog, '-MExtUtils::Embed', '-e', 'ldopts', check: true).stdout().strip().split()
  perl_dep = declare_dependency(compile_args: perl_ccopts, link_args: perl_ldopts)

  # helper.pm, the Perl side of the plugin, as the C string helper_pm.
  helper_pm_h = custom_target('helper_pm.h',
    input: 'helper.pm',
    output: 'helper_pm.h',
    command: [perl_prog, files('embed.pl'), 'helper_pm', '@INPUT@'],
    capture: true,
  )

  ugly_perl5 = shared_module('ugly_perl5',
    'perl5.c', helper_pm_h,
    name_prefix: '',
    dependencies: [ugly_dep, perl_dep],
    install: true,
    install_dir: ugly_plugin_dir,
  )
  plugins += ugly_perl5
  plugin_dirs += meson.current_build_dir()
  test_plugins += 'perl5'
  subdir('binding')
endif
