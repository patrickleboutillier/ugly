# The .NET binding's test program, in its own directory (with its copy of Ugly.dll).
test_binding_dll = custom_target('test_binding.dll',
  input: ['test_binding.csproj', 'test_binding.cs'],
  output: 'test_binding.dll',
  command: [dotnet, 'build', '@INPUT0@', '--nologo', '-v', 'quiet', '-c', 'Release',
    '-p:UglyDll=' + ugly_dll[0].full_path(), '--artifacts-path', '@PRIVATE_DIR@', '-o', '@OUTDIR@'],
  env: dotnet_env,
  depends: ugly_dll,
)
