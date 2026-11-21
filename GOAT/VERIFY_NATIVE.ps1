$ErrorActionPreference = 'Stop'
$restoreRoot = $PSScriptRoot
$cmakeExe = 'C:\Users\laith\Downloads\src Project V28.2.90\GOAT_SOLITAIRE_V4_FAST_EXPERT_WORK_20260828\BUILD_TOOLS\cmake-4.4.3-windows-x86_64\bin\cmake.exe'
$ndkRoot = 'C:\Users\laith\AppData\Local\Android\Sdk\ndk\25.1.8937393'
$ninjaExe = 'C:\Users\laith\AppData\Local\Android\Sdk\cmake\3.10.2.4988404\bin\ninja.exe'
$engineSource = Join-Path $restoreRoot 'ENGINE'
$engineBuild = Join-Path $engineSource 'build-v61-verify'
& $cmakeExe -S $engineSource -B $engineBuild -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_TOOLCHAIN_FILE=$ndkRoot\build\cmake\android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 "-DCMAKE_MAKE_PROGRAM=$ninjaExe"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& $cmakeExe --build $engineBuild --parallel 4
if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
$builtLibrary = Join-Path $engineBuild 'libengine.so'
& "$ndkRoot\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe" --strip-unneeded $builtLibrary
if ($LASTEXITCODE -ne 0) { throw 'Native strip failed' }
$expected = '732934D77189886C63FD93AFFD69EE1645159BB7EAE481835E39CB816B0D48A7'
$actual = (Get-FileHash -LiteralPath $builtLibrary -Algorithm SHA256).Hash
if ($actual -ne $expected) { throw "Not the exact V6.1 binary: $actual" }
Write-Output "PASS: rebuilt V6.1 native library is byte-identical by SHA-256: $actual"
# This script does not install an APK, access the phone, sign, or update a server.
