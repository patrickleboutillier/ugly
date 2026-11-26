# The Haskell plugin, built as ugly_haskell.so (the vtable, in C) and
# libugly_hs.so next to it (the Haskell side, Ugly/*.hs, with rts.c), which
# ghc builds against GHC's shared libraries and threaded RTS. Needs GHC 8.8 or
# later with both (Debian and Ubuntu's ghc package). Optional: -Dhaskell=disabled.

ghc = find_program('ghc', required: get_option('haskell'))
haskell_ok = false

if ghc.found()
  ghc_version = run_command(ghc, '--numeric-version', check: true).stdout().strip()
  ghc_ways = run_command(ghc, '--info', check: true).stdout()
  if ghc_version.version_compare('>=8.8') and ghc_ways.contains('thr_dyn')
    haskell_ok = true
    message('haskell: GHC ' + ghc_version)
  elif get_option('haskell').enabled()
    error('haskell: needs GHC 8.8 or later with shared libraries and the threaded RTS (found ' + ghc_version + ')')
  endif
endif

if haskell_ok
  # A shared library built by ghc doesn't link an RTS unless told to: 9.0 and
  # later with -flink-rts (the threaded one, with -threaded), 8.x by name.
  if ghc_version.version_compare('>=9.0')
    ghc_rts = ['-flink-rts']
  else
    ghc_rts = ['-lHSrts_thr-ghc' + ghc_version]
  endif
  ghc_werror = get_option('werror') ? ['-Werror'] : []

  # The Haskell modules are a package of their own ('ugly'), so that modules
  # built later against it share these symbols. Its .hi files stay in the
  # private dir for now.
  hs_sources = files('Ugly/Example.hs', 'Ugly/ExampleExports.hs', 'Ugly/Export.hs', 'Ugly/Native.hs',
    'Ugly/Plugin.hs', 'Ugly/Smoke.hs', 'Ugly/Value.hs')
  ugly_hs = custom_target('libugly_hs.so',
    input: ['rts.c', 'glue.c', hs_sources],
    output: 'libugly_hs.so',
    command: [ghc, '-v0', '-O', '-Wall'] + ghc_werror + [
      '-dynamic', '-shared', '-fPIC', '-threaded', '-no-hs-main', '-this-unit-id', 'ugly',
      '-i' + meson.current_source_dir(), '-I' + (meson.project_source_root() / 'include'),
      '-odir', '@PRIVATE_DIR@', '-hidir', '@PRIVATE_DIR@', '-stubdir', '@PRIVATE_DIR@',
      '-optc-Wall', '-optl-Wl,-soname,libugly_hs.so',
      '-o', '@OUTPUT@', '@INPUT@'] + ghc_rts,
    install: true,
    install_dir: ugly_plugin_dir,
  )

  ugly_haskell = shared_module('ugly_haskell',
    'haskell.c',
    name_prefix: '',
    dependencies: ugly_dep,
    link_with: ugly_hs,
    install_rpath: '$ORIGIN',
    install: true,
    install_dir: ugly_plugin_dir,
  )
  plugins += [ugly_haskell, ugly_hs]
  plugin_dirs += meson.current_build_dir()

  # Not in the conformance suite yet: the RTS and runtimes (smoke), values
  # and boxes (values) and calls (calls), from the built-in module Ugly.Smoke,
  # and uglyModule's export tables (exports), from Ugly.Example. Suite
  # 'ghc' is anything that starts the RTS (see the valgrind setups).
  foreach t : ['smoke', 'values', 'calls', 'exports']
    test('haskell-' + t,
      executable('haskell_' + t, t + '.c', dependencies: [ugly_dep, dependency('threads'), cc.find_library('dl')]),
      env: {'UGLY_PLUGIN_PATH': meson.current_build_dir()},
      depends: [ugly_haskell, ugly_hs],
      protocol: 'tap',
      suite: ['haskell', 'ghc'],
    )
  endforeach
endif
