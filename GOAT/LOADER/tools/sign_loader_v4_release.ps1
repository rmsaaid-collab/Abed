$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$unsignedApk = Join-Path $projectRoot 'app\build\outputs\apk\release\app-release-unsigned.apk'
$alignedApk = Join-Path $projectRoot 'app\build\outputs\apk\release\app-release-v4-aligned.apk'
$outputApk = 'C:\Users\laith\Downloads\GOAT_JAWAKER_LOADER_VVIP_V4_SOLITAIRE_ENGINE_20260826.apk'
$keystore = 'C:\Users\laith\Downloads\src Project V28.2.90\GOAT_MULTI_GAME_WORK\goat_loader_release_20260820.jks'
$credential = Join-Path $env:LOCALAPPDATA 'GOAT\loader-release-password.dpapi'
$buildTools = 'C:\Users\laith\AppData\Local\Android\Sdk\build-tools\36.0.0'
$zipalign = Join-Path $buildTools 'zipalign.exe'
$apksigner = Join-Path $buildTools 'apksigner.bat'

foreach ($required in @($unsignedApk, $keystore, $credential, $zipalign, $apksigner)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required release-signing file is missing: $required"
    }
}

if (Test-Path -LiteralPath $outputApk) {
    throw "Refusing to overwrite the existing V4 artifact: $outputApk"
}

$securePassword = Get-Content -LiteralPath $credential -Raw | ConvertTo-SecureString
$passwordPointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($securePassword)
try {
    $plainPassword = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($passwordPointer)
}
finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($passwordPointer)
}

try {
    Remove-Item -LiteralPath $alignedApk -Force -ErrorAction SilentlyContinue

    & $zipalign -f -p 4 $unsignedApk $alignedApk
    if ($LASTEXITCODE -ne 0) { throw "zipalign failed with exit code $LASTEXITCODE" }

    $env:GOAT_SIGN_PASS = $plainPassword
    & $apksigner sign `
        --ks $keystore `
        --ks-key-alias 'goat-loader-release' `
        --ks-pass 'env:GOAT_SIGN_PASS' `
        --key-pass 'env:GOAT_SIGN_PASS' `
        --out $outputApk `
        $alignedApk
    if ($LASTEXITCODE -ne 0) { throw "APK signing failed with exit code $LASTEXITCODE" }

    & $apksigner verify --verbose --print-certs $outputApk
    if ($LASTEXITCODE -ne 0) { throw "APK signature verification failed with exit code $LASTEXITCODE" }

    Get-Item -LiteralPath $outputApk | Select-Object FullName, Length, LastWriteTime
}
finally {
    $env:GOAT_SIGN_PASS = $null
    $plainPassword = $null
    $securePassword.Dispose()
    Remove-Item -LiteralPath $alignedApk -Force -ErrorAction SilentlyContinue
}
