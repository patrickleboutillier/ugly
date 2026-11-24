# The Perl binding (XS), so perl code can drive the C API. The embedded perl's
# helper (lang/perl5/helper/helper.pm) uses it too.
#
# Built without MakeMaker: xsubpp generates Ugly.c here, and
# auto/Ugly/meson.build compiles it, so the module lands in
# <build>/lib/perl5/auto/Ugly/Ugly.so, where XSLoader finds it through @INC.

xsubpp = find_program('xsubpp')

perl_xs_version = run_command(perl, '-MExtUtils::MakeMaker', '-e', 'print MM->parse_version(shift)',
  files('lib/Ugly.pm'), check: true).stdout().strip()

perl_installdir = get_option('perl_installdir')
if perl_installdir == ''
  perl_installdir = run_command(perl, '-MConfig', '-e', 'print $Config{installsitearch}',
    check: true).stdout().strip()
endif

ugly_xs_c = custom_target('Ugly.c',
  input: 'Ugly.xs',
  output: 'Ugly.c',
  depend_files: files('typemap'),
  # xsubpp chdirs to the .xs file's directory, so the typemap path must be
  # absolute; the generated C goes to stdout.
  command: [xsubpp, '-typemap', meson.current_source_dir() / 'typemap', '@INPUT@'],
  capture: true,
)

subdir('auto/Ugly')

install_data('lib/Ugly.pm', install_dir: perl_installdir)
install_data('lib/Ugly/Value.pm', install_dir: perl_installdir / 'Ugly')
