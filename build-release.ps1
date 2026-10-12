param(
    [Parameter(Mandatory=$true)][string]$Gcc,
    [Parameter(Mandatory=$true)][string]$Python,
    [string]$Version = 'v0.1.0-xp-test.1'
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$Gcc = (Resolve-Path $Gcc).Path
$Python = (Resolve-Path $Python).Path
$env:PATH = (Split-Path $Gcc) + ';' + $env:PATH
$out = Join-Path $root "build\release\$Version"
if (Test-Path -LiteralPath $out) { throw "Use a fresh output directory: $out" }
$package = Join-Path $out 'XP-Harness-Desktop'
New-Item -ItemType Directory -Path "$package\licenses" -Force | Out-Null
Push-Location $root
try {
    # MinGW-w64 GCC 8.1.0, i686, MSVCRT. No UCRT or compiler DLL required.
    $flags = @('-std=gnu89', '-Os', '-march=pentium3', '-fms-extensions',
        '-DCJSON_NESTING_LIMIT=64', '-static', '-static-libgcc', '-s',
        '-Wall', '-Wextra', '-Wno-unknown-pragmas', '-I', 'dep/curl-8.20/include',
        '-Wl,--major-os-version,5,--minor-os-version,1,--major-subsystem-version,5,--minor-subsystem-version,1')
    $shared = @('src/cJSON.c', 'src/ipc.c', '-luser32', '-ladvapi32')
    $gui = @('-lgdi32', '-lshell32', '-lole32', '-lcomctl32', '-lcomdlg32')
    & $Gcc @flags src/main.c @shared dep/curl-8.20/lib/libcurl.lib -o "$package\AgentCore.exe"
    if ($LASTEXITCODE) { throw 'Core build failed' }
    & $Gcc @flags src/desktop.c @shared @gui -mwindows -o "$package\XP-Harness-Desktop.exe"
    if ($LASTEXITCODE) { throw 'Desktop build failed' }
    Copy-Item -LiteralPath 'dep/curl-8.20/bin/libcurl-4.dll', 'dep/curl-8.20/bin/curl.exe',
        'dep/busybox-1.38/busybox.exe', 'dep/jq-1.8.2/jq.exe', 'curl-ca-bundle.crt',
        'vc6ai.sample.ini', 'LICENSE', 'NOTICE', 'RELEASE-TEST.txt' -Destination $package
    Copy-Item -LiteralPath 'vc6ai.sample.ini' -Destination "$package\vc6ai.ini"
    Copy-Item -LiteralPath 'dep/busybox-1.38/LICENSE' -Destination "$package\licenses\busybox.txt"
    Copy-Item -LiteralPath 'dep/curl-8.20/COPYING' -Destination "$package\licenses\curl.txt"
    Copy-Item -LiteralPath 'dep/jq-1.8.2/COPYING' -Destination "$package\licenses\jq.txt"
    Copy-Item -LiteralPath 'dep/jq-1.8.2/COPYING.oniguruma' -Destination "$package\licenses\oniguruma.txt"
    $toolchain = Split-Path (Split-Path $Gcc)
    foreach ($component in @('gcc', 'mingw-w64', 'winpthreads')) {
        Copy-Item -LiteralPath "$toolchain\licenses\$component" -Destination "$package\licenses" -Recurse
    }
    & $Gcc @flags tests/native_tests.c @shared dep/curl-8.20/lib/libcurl.lib -o "$out\native_tests.exe"
    if ($LASTEXITCODE) { throw 'Native test build failed' }
    & $Gcc @flags tests/desktop_tests.c @shared @gui -o "$out\desktop_tests.exe"
    if ($LASTEXITCODE) { throw 'Desktop test build failed' }
    Copy-Item -LiteralPath "$package\libcurl-4.dll" -Destination $out
    foreach ($test in @('native_tests', 'desktop_tests')) {
        & "$out\$test.exe"
        if ($LASTEXITCODE) { throw "$test failed" }
    }
    foreach ($test in @('smoke', 'stream_smoke', 'desktop_memory_smoke', 'approval_recovery_smoke', 'desktop_basics_smoke')) {
        & $Python "tests/$test.py" "$package\AgentCore.exe"
        if ($LASTEXITCODE) { throw "$test failed" }
    }
    $archive = Join-Path $out "XP-Harness-Desktop-$Version-win32.zip"
    Compress-Archive -LiteralPath $package -DestinationPath $archive
    $hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $(Split-Path $archive -Leaf)" | Set-Content -Encoding ascii "$archive.sha256"
    Write-Output "Tested archive: $archive (real XP validation still required)"
} finally { Pop-Location }
