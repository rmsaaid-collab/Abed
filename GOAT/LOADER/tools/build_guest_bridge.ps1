$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $projectRoot 'app\src\main\java\com\google\firebase\TrainerOverlayView.java'
$assetDir = Join-Path $projectRoot 'app\src\main\assets'
$asset = Join-Path $assetDir 'goat_bridge.jar'

$sdkRoot = $env:ANDROID_SDK_ROOT
if ([string]::IsNullOrWhiteSpace($sdkRoot)) {
    $sdkRoot = $env:ANDROID_HOME
}
if ([string]::IsNullOrWhiteSpace($sdkRoot)) {
    throw 'ANDROID_SDK_ROOT or ANDROID_HOME must point to the Android SDK.'
}

$androidJar = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'platforms') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'android.jar') } |
    Sort-Object { if ($_.Name -match '(\d+)') { [int]$matches[1] } else { 0 } } -Descending |
    Select-Object -First 1 |
    ForEach-Object { Join-Path $_.FullName 'android.jar' }

$d8 = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'build-tools') -Directory |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'd8.bat') } |
    Sort-Object { [version]($_.Name -replace '-.*$', '') } -Descending |
    Select-Object -First 1 |
    ForEach-Object { Join-Path $_.FullName 'd8.bat' }

if (!(Test-Path -LiteralPath $source)) { throw "Missing bridge source: $source" }
if (!(Test-Path -LiteralPath $androidJar)) { throw 'No Android platform android.jar was found.' }
if (!(Test-Path -LiteralPath $d8)) { throw 'No Android build-tools d8.bat was found.' }

$workRoot = Join-Path $projectRoot 'build\guest-bridge'
$classes = Join-Path $workRoot 'classes'
$dexOut = Join-Path $workRoot 'dex'
New-Item -ItemType Directory -Force -Path $classes, $dexOut, $assetDir | Out-Null

& javac -encoding UTF-8 -source 8 -target 8 -classpath $androidJar -d $classes $source
if ($LASTEXITCODE -ne 0) { throw "javac failed with exit code $LASTEXITCODE" }

$bridgeClass = Join-Path $classes 'com\google\firebase\TrainerOverlayView.class'
& $d8 --release --lib $androidJar --output $dexOut $bridgeClass
if ($LASTEXITCODE -ne 0) { throw "d8 failed with exit code $LASTEXITCODE" }

& jar cf $asset -C $dexOut classes.dex
if ($LASTEXITCODE -ne 0) { throw "jar failed with exit code $LASTEXITCODE" }
Write-Output "Guest bridge ready: $asset"
