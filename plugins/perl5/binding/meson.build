# The Perl binding: perl5.c again, as the XS module Ugly (auto/Ugly/Ugly.so,
# loaded by lib/Ugly.pm), for use Ugly from a plain perl program. It doesn't
# link libperl: its perl symbols come from the host perl, which exports them.

subdir('auto/Ugly')
binding_dirs += { 'perl5': meson.current_build_dir() }
perl_binding_lib = meson.current_source_dir() / 'lib'
