# The Haskell fixture: module "conformance" in the library conformance.so,
# on UGLY_HASKELL_PATH (this directory, in the build tree).
conformance_hs = custom_target('conformance.so',
  input: 'Conformance.hs',
  output: 'conformance.so',
  command: ghc_module,
  depends: ghc_module_depends,
)
plugins += conformance_hs
haskell_fixture_dir = meson.current_build_dir()

# The binding's test: a Haskell program as the host (TestBinding.hs), built
# against the package 'ugly' as any program would be. -rdynamic: its own
# export module is found in the process. Registered with the binding tests.
haskell_test_binding = custom_target('test_binding_haskell',
  input: 'TestBinding.hs',
  output: 'test_binding_haskell',
  command: [ghc, '-v0', '-O', '-Wall'] + ghc_werror + ['-dynamic', '-threaded', '-rdynamic',
    '-package-db', ugly_package_db, '-package', 'ugly',
    '-outputdir', '@PRIVATE_DIR@', '-o', '@OUTPUT@', '@INPUT@'],
  depends: ghc_module_depends,
)
