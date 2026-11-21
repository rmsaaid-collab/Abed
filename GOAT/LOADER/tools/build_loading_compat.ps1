$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$ndkRoot = 'C:\Users\laith\AppData\Local\Android\Sdk\ndk\25.1.8937393'
$clang = Join-Path $ndkRoot 'toolchains\llvm\prebuilt\windows-x86_64\bin\aarch64-linux-android21-clang++.cmd'
$source = Join-Path $projectRoot 'app\src\main\cpp\loading_compat.cpp'
$libraryDir = Join-Path $projectRoot 'app\src\main\jniLibs\arm64-v8a'
$dobby = Join-Path $libraryDir 'libdobby.so'
$output = Join-Path $libraryDir 'libloading_compat.so'

foreach ($required in @($clang, $source, $dobby)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required loading-compat build input is missing: $required"
    }
}

& $clang `
    -std=c++17 `
    -O2 `
    -fPIC `
    -fvisibility=hidden `
    -ffunction-sections `
    -fdata-sections `
    -fstack-protector-strong `
    -shared `
    -static-libstdc++ `
    "-L$libraryDir" `
    '-Wl,--gc-sections' `
    '-Wl,-z,relro' `
    '-Wl,-z,now' `
    '-Wl,--no-undefined' `
    $source `
    -ldobby `
    -llog `
    -ldl `
    -o $output

if ($LASTEXITCODE -ne 0) {
    throw "loading compatibility native build failed with exit code $LASTEXITCODE"
}
if (-not (Test-Path -LiteralPath $output) -or (Get-Item -LiteralPath $output).Length -le 0) {
    throw 'loading compatibility native build produced no library'
}

Get-Item -LiteralPath $output | Select-Object FullName, Length, LastWriteTime
