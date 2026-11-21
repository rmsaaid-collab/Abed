param(
    [Parameter(Mandatory = $true)]
    [string]$BuildRoot,

    [string]$OutputDirectory = 'C:\Users\laith\Downloads'
)

$ErrorActionPreference = 'Stop'

$keystore = 'C:\Users\laith\Downloads\src Project V28.2.90\GOAT_MULTI_GAME_WORK\goat_loader_release_20260820.jks'
$credential = Join-Path $env:LOCALAPPDATA 'GOAT\loader-release-password.dpapi'
$buildTools = 'C:\Users\laith\AppData\Local\Android\Sdk\build-tools\36.0.0'
$zipalign = Join-Path $buildTools 'zipalign.exe'
$apksigner = Join-Path $buildTools 'apksigner.bat'

$variants = @(
    @{
        Name = 'DEVICE'
        Input = Join-Path $BuildRoot 'app\build\outputs\apk\device\release\app-device-release-unsigned.apk'
        Output = Join-Path $OutputDirectory 'GOAT_SOLITAIRE_PRO_EXPERT_V5_DEVICE_20260829.apk'
    },
    @{
        Name = 'PC_EMULATOR'
        Input = Join-Path $BuildRoot 'app\build\outputs\apk\emulator\release\app-emulator-release-unsigned.apk'
        Output = Join-Path $OutputDirectory 'GOAT_SOLITAIRE_PRO_EXPERT_V5_PC_EMULATOR_20260829.apk'
    }
)

foreach ($required in @($keystore, $credential, $zipalign, $apksigner)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required release-signing file is missing: $required"
    }
}

foreach ($variant in $variants) {
    if (-not (Test-Path -LiteralPath $variant.Input)) {
        throw "Missing unsigned $($variant.Name) APK: $($variant.Input)"
    }
    if (Test-Path -LiteralPath $variant.Output) {
        throw "Refusing to overwrite existing release artifact: $($variant.Output)"
    }
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
    $env:GOAT_SIGN_PASS = $plainPassword
    foreach ($variant in $variants) {
        $aligned = Join-Path $BuildRoot "app\build\outputs\apk\$($variant.Name.ToLowerInvariant())-v5-aligned.apk"
        try {
            & $zipalign -f -p 4 $variant.Input $aligned
            if ($LASTEXITCODE -ne 0) { throw "zipalign failed for $($variant.Name) with exit code $LASTEXITCODE" }

            & $apksigner sign `
                --ks $keystore `
                --ks-key-alias 'goat-loader-release' `
                --ks-pass 'env:GOAT_SIGN_PASS' `
                --key-pass 'env:GOAT_SIGN_PASS' `
                --out $variant.Output `
                $aligned
            if ($LASTEXITCODE -ne 0) { throw "APK signing failed for $($variant.Name) with exit code $LASTEXITCODE" }

            & $apksigner verify --verbose --print-certs $variant.Output
            if ($LASTEXITCODE -ne 0) { throw "APK signature verification failed for $($variant.Name) with exit code $LASTEXITCODE" }

            $hash = Get-FileHash -Algorithm SHA256 -LiteralPath $variant.Output
            $file = Get-Item -LiteralPath $variant.Output
            Write-Output "$($variant.Name)|$($file.FullName)|$($file.Length)|$($hash.Hash)"
        }
        finally {
            Remove-Item -LiteralPath $aligned -Force -ErrorAction SilentlyContinue
        }
    }
}
finally {
    $env:GOAT_SIGN_PASS = $null
    $plainPassword = $null
    $securePassword.Dispose()
}
