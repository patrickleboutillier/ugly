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
