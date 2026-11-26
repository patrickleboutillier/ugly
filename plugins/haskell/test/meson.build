# Module libraries for the Haskell plugin's own tests, on their
# UGLY_HASKELL_PATH (this directory, in the build tree):
#
#   Smoke.so     module Smoke, a box written by hand (uglyModuleFrom)
#   Example.so   modules Example and Example.Points (uglyModule): one
#                library, two modules, found by the longest prefix
#   Broken.so    not a library at all

haskell_test_modules = meson.current_build_dir()

haskell_test_libs = [
  custom_target('Smoke.so',
    input: 'Smoke.hs',
    output: 'Smoke.so',
    command: ghc_module,
    depends: ghc_module_depends,
  ),
  custom_target('Example.so',
    input: ['Example.hs', 'Example/Ugly.hs', 'Example/Points.hs'],
    output: 'Example.so',
    command: ghc_module,
    depends: ghc_module_depends,
  ),
]

configure_file(input: 'Broken.txt', output: 'Broken.so', copy: true)
