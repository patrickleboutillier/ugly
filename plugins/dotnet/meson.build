# The .NET plugin, built as ugly_dotnet.so, with its C# side Ugly.dll (and
# Ugly.runtimeconfig.json, which the CLR is started from) next to it. Needs a
# .NET 10 SDK: dotnet, with the nethost pack it ships (nethost.h, hostfxr.h,
# libnethost.a). Optional: -Ddotnet=disabled.

dotnet = find_program('dotnet', required: get_option('dotnet'))
dotnet_ok = false

if dotnet.found()
  # nethost: in the SDK's apphost pack, <dotnet root>/packs/Microsoft.NETCore.App.Host.<rid>/<version>/runtimes/<rid>/native.
  nethost_dir = run_command('sh', '-c',
    'root=$(dirname "$(readlink -f "$1")"); ls -d "$root"/packs/Microsoft.NETCore.App.Host.*/*/runtimes/*/native 2>/dev/null | sort -V | tail -n 1',
    'sh', dotnet.full_path(), check: false).stdout().strip()
  if nethost_dir != '' and import('fs').exists(nethost_dir / 'libnethost.a')
    dotnet_ok = true
  elif get_option('dotnet').enabled()
    error('dotnet: no nethost pack (libnethost.a) found')
  endif
endif

if dotnet_ok
  nethost_dep = declare_dependency(
    include_directories: include_directories(nethost_dir),
    link_args: [nethost_dir / 'libnethost.a', '-lstdc++', '-ldl'],
  )

  # dotnet build, out of the source tree: intermediate files in the private dir.
  ugly_dll = custom_target('Ugly.dll',
    input: ['Ugly.csproj', 'Bridge.cs'],
    output: ['Ugly.dll', 'Ugly.runtimeconfig.json'],
    command: [dotnet, 'build', '@INPUT0@', '--nologo', '-v', 'quiet', '-c', 'Release',
      '--artifacts-path', '@PRIVATE_DIR@', '-o', '@OUTDIR@'],
    env: {'DOTNET_CLI_TELEMETRY_OPTOUT': '1', 'DOTNET_NOLOGO': '1', 'DOTNET_SKIP_FIRST_TIME_EXPERIENCE': '1'},
    install: true,
    install_dir: ugly_plugin_dir,
  )

  ugly_dotnet = shared_module('ugly_dotnet',
    'dotnet.c',
    name_prefix: '',
    dependencies: [ugly_dep, nethost_dep],
    install: true,
    install_dir: ugly_plugin_dir,
  )
  plugins += [ugly_dotnet, ugly_dll]
  plugin_dirs += meson.current_build_dir()

  # Not in the conformance suite yet ('test_plugins'): a smoke test instead.
  test('dotnet-smoke', executable('dotnet_smoke', 'smoke.c', dependencies: ugly_dep),
    env: {'UGLY_PLUGIN_PATH': meson.current_build_dir()},
    depends: [ugly_dotnet, ugly_dll],
    protocol: 'tap',
    suite: ['dotnet', 'clr'],
  )
endif
