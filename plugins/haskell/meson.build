# The Haskell plugin, built as ugly_haskell.so (the vtable, in C), with
# libugly_hs.so (the Haskell side, Ugly/*.hs) and libugly_rts.so (rts.c)
# next to it, which ghc builds against GHC's shared libraries and threaded
# RTS. Needs GHC 8.8 or later with both (Debian and Ubuntu's ghc package).
# Optional: -Dhaskell=disabled.

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
  ghc_common = [ghc, '-v0', '-O', '-Wall'] + ghc_werror + ['-dynamic', '-shared', '-fPIC',
    '-odir', '@PRIVATE_DIR@', '-hidir', '@PRIVATE_DIR@', '-stubdir', '@PRIVATE_DIR@']

  # Three libraries. libugly_rts.so starts the RTS (rts.c: only ghc knows
  # where its RTS headers are) and links it. libugly_hs.so is the package
  # 'ugly' (the Haskell modules, and glue.c), with no RTS of its own: Template
  # Haskell loads it into ghc, against ghc's RTS, to build module libraries.
  # ugly_haskell.so, the vtable, links both.
  ugly_rts = custom_target('libugly_rts.so',
    input: 'rts.c',
    output: 'libugly_rts.so',
    command: ghc_common + ['-threaded', '-no-hs-main', '-optc-Wall', '-optl-Wl,-soname,libugly_rts.so',
      '-o', '@OUTPUT@', '@INPUT@'] + ghc_rts,
    install: true,
    install_dir: ugly_plugin_dir,
  )

  # Interfaces are .dyn_hi, which is what ghc -dynamic looks for in a package.
  hs_sources = files('Ugly.hs', 'Ugly/Export.hs', 'Ugly/Marshal.hs', 'Ugly/Native.hs', 'Ugly/Plugin.hs',
    'Ugly/Types.hs', 'Ugly/Value.hs')
  # binding.c is haskell.c again, in binding mode (a Haskell program as the host).
  ugly_hs = custom_target('libugly_hs.so',
    input: ['glue.c', 'binding.c', hs_sources],
    depend_files: files('haskell.c'),
    output: 'libugly_hs.so',
    command: ghc_common + ['-this-unit-id', 'ugly', '-hisuf', 'dyn_hi', '-osuf', 'dyn_o',
      '-i' + meson.current_source_dir(), '-I' + (meson.project_source_root() / 'include'),
      '-optc-Wall', '-optl-Wl,-soname,libugly_hs.so', '-o', '@OUTPUT@', '@INPUT@'],
    install: true,
    install_dir: ugly_plugin_dir,
  )

  ugly_haskell = shared_module('ugly_haskell',
    'haskell.c',
    name_prefix: '',
    dependencies: [ugly_dep, cc.find_library('dl')],
    link_with: [ugly_hs, ugly_rts],
    install_rpath: '$ORIGIN',
    install: true,
    install_dir: ugly_plugin_dir,
  )
  plugins += [ugly_haskell, ugly_hs, ugly_rts]
  plugin_dirs += meson.current_build_dir()
  test_plugins += 'haskell'

  # The package 'ugly' in a package database in the build tree: module
  # libraries are built against it with ghc_module (below). Installed, it's
  # <libdir>/ugly/haskell/package.db, with the interfaces beside it.
  ghc_pkg = find_program('ghc-pkg-' + ghc_version, 'ghc-pkg')
  package_py = find_program('package.py')
  ugly_package_db = meson.current_build_dir() / 'package.db'
  ugly_package = custom_target('ugly.conf',
    output: 'ugly.conf',
    command: [package_py, 'build', ghc_pkg, ugly_package_db, '@OUTPUT@', meson.project_version(),
      meson.current_build_dir() / 'libugly_hs.so.p', meson.current_build_dir() + ':' + (meson.project_build_root() / 'src')],
    depends: ugly_hs,
  )
  meson.add_install_script(package_py, 'install', ghc_pkg.full_path(), meson.current_build_dir() / 'libugly_hs.so.p',
    get_option('prefix') / get_option('libdir') / 'ugly' / 'haskell',
    ugly_plugin_dir + ':' + (get_option('prefix') / get_option('libdir')), meson.project_version())

  # A module library: a custom_target's command, its input the export
  # modules (uglyModule) and the modules they need, its output <Name>.so,
  # named after the module (or a prefix of the modules) it exports. No RTS:
  # it joins the plugin's. depends: ghc_module_depends.
  ghc_module = ghc_common + ['-package-db', ugly_package_db, '-package', 'ugly', '-o', '@OUTPUT@', '@INPUT@']
  ghc_module_depends = [ugly_package, ugly_hs]

  # Not in the conformance suite yet: the RTS and runtimes, and module
  # libraries found on the path (smoke), values and boxes (values) and calls
  # (calls), from the test module Smoke, uglyModule's export tables
  # (exports), from Example, and foreign values in Haskell (foreign). Suite
  # 'ghc' is anything that starts the RTS (see the valgrind setups).
  subdir('test')
  foreach t : ['smoke', 'values', 'calls', 'exports', 'foreign']
    test('haskell-' + t,
      executable('haskell_' + t, t + '.c', dependencies: [ugly_dep, dependency('threads'), cc.find_library('dl')]),
      env: {'UGLY_PLUGIN_PATH': meson.current_build_dir(), 'UGLY_HASKELL_PATH': haskell_test_modules},
      depends: [ugly_haskell, ugly_hs, ugly_rts] + haskell_test_libs,
      protocol: 'tap',
      suite: ['haskell', 'ghc'],
    )
  endforeach
endif
