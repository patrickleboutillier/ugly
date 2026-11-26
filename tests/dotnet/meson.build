# The C# fixtures: conformance.dll in this build directory, found through UGLY_DOTNET_PATH.
conformance_dll = custom_target('conformance.dll',
  input: ['conformance.csproj', 'conformance.cs'],
  output: 'conformance.dll',
  command: [dotnet, 'build', '@INPUT0@', '--nologo', '-v', 'quiet', '-c', 'Release',
    '-p:UglyDll=' + ugly_dll[0].full_path(), '--artifacts-path', '@PRIVATE_DIR@', '-o', '@OUTDIR@'],
  env: dotnet_env,
  depends: ugly_dll,
)
plugins += conformance_dll
dotnet_fixture_dir = meson.current_build_dir()
