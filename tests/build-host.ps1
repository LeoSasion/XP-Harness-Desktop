param([string]$Zig = "$PSScriptRoot\..\.tools\zig-windows-x86_64-0.13.0\zig.exe")
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..").Path
$Zig = (Resolve-Path $Zig).Path
$out = Join-Path $root 'build\host'
New-Item -ItemType Directory -Force $out | Out-Null
$flags = @('cc', '-target', 'x86-windows-gnu', '-std=gnu89', '-Os', '-fms-extensions',
    '-U_WIN32_WINNT', '-Wno-deprecated-declarations', '-DCJSON_NESTING_LIMIT=64',
    '-I', "$root\dep\curl-8.20\include", '-L', "$root\dep\curl-8.20\lib")
$shared = @("$root\src\cJSON.c", "$root\src\ipc.c", '-luser32', '-ladvapi32')
& $Zig @flags "$root\src\main.c" @shared "$root\dep\curl-8.20\lib\libcurl.lib" '-o' "$out\AgentCore.exe"
if ($LASTEXITCODE) { throw 'AgentCore compilation failed' }
& $Zig @flags "$root\src\desktop.c" @shared '-lgdi32' '-lshell32' '-lole32' '-lcomctl32' '-Wl,--subsystem,windows' '-o' "$out\XP-Harness-Desktop.exe"
if ($LASTEXITCODE) { throw 'Desktop compilation failed' }
& $Zig @flags "$root\tests\native_tests.c" @shared "$root\dep\curl-8.20\lib\libcurl.lib" '-o' "$out\native_tests.exe"
if ($LASTEXITCODE) { throw 'Native test compilation failed' }
Copy-Item -LiteralPath "$root\dep\curl-8.20\bin\libcurl-4.dll", "$root\curl-ca-bundle.crt", "$root\vc6ai.sample.ini" -Destination $out -Force
Write-Output "Host test build: $out (not an XP-certified release)"
& "$out\native_tests.exe"
if ($LASTEXITCODE) { throw 'Native regression tests failed' }
