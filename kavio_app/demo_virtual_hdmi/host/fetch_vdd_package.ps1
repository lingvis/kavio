param(
    [string]$Tag = "25.7.23"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$targetDir = Join-Path $scriptDir "vdd_package"
$tempRoot = Join-Path $env:TEMP ("kavio-vdd-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

try {
    $headers = @{ "User-Agent" = "Kavio-VirtualHdmiStream" }
    if ($Tag -eq "latest") {
        $releaseUrl = "https://api.github.com/repos/VirtualDrivers/Virtual-Display-Driver/releases/latest"
    } else {
        $releaseUrl = "https://api.github.com/repos/VirtualDrivers/Virtual-Display-Driver/releases/tags/$Tag"
    }

    Write-Host "Querying VirtualDrivers release: $releaseUrl"
    $release = Invoke-RestMethod -Uri $releaseUrl -Headers $headers
    $zipAssets = @($release.assets | Where-Object { $_.name -match '\.zip$' })
    if ($zipAssets.Count -eq 0) {
        throw "The selected release does not contain a ZIP asset with manual driver files."
    }

    # The 25.7.23 project names its normal Intel/AMD driver-only asset
    # "VirtualDisplayDriver-x86.Driver.Only.zip" even though the actual VDD DLL
    # used on 64-bit Windows is AMD64. Prefer that small official driver-only
    # archive, then fall back to inspecting the full control package. The PE
    # machine type is validated after extraction, so the name cannot make us
    # accidentally stage a 32-bit driver.
    $zipAssets = $zipAssets | Sort-Object -Property @(
        @{ Expression = {
            if ($_.name -match '(?i)^VirtualDisplayDriver-x86\.Driver\.Only\.zip$') { 0 }
            elseif ($_.name -match '(?i)signed.*x64|x64.*signed') { 1 }
            elseif ($_.name -match '(?i)^VDD\.Control\.') { 2 }
            elseif ($_.name -match '(?i)x64') { 3 }
            elseif ($_.name -match '(?i)arm64') { 9 }
            else { 5 }
        } },
        @{ Expression = { $_.name } }
    )

    $driverDir = $null
    foreach ($asset in $zipAssets) {
        $zipPath = Join-Path $tempRoot $asset.name
        $extractDir = Join-Path $tempRoot ([IO.Path]::GetFileNameWithoutExtension($asset.name))
        Write-Host "Inspecting $($asset.name)"
        Invoke-WebRequest -Uri $asset.browser_download_url -Headers $headers -OutFile $zipPath
        Expand-Archive -Path $zipPath -DestinationPath $extractDir -Force

        $infCandidates = @(Get-ChildItem -Path $extractDir -Filter MttVDD.inf -Recurse -File)
        foreach ($inf in $infCandidates) {
            $candidate = $inf.Directory.FullName
            if ((Test-Path (Join-Path $candidate "MttVDD.dll")) -and
                (Test-Path (Join-Path $candidate "mttvdd.cat"))) {
                # If the archive contains multiple architectures, prefer a path that
                # explicitly identifies x64/amd64 and reject obvious x86/arm folders.
                $normalized = $candidate.ToLowerInvariant()
                if ($normalized -match '\\(x86|win32|arm64|arm)\\') { continue }
                $driverDir = $candidate
                break
            }
        }
        if ($driverDir) { break }
    }

    if (-not $driverDir) {
        throw "Could not locate the signed x64 MttVDD.inf/MttVDD.dll/mttvdd.cat set in release $($release.tag_name)."
    }

    New-Item -ItemType Directory -Force -Path $targetDir | Out-Null
    foreach ($name in @("MttVDD.inf", "MttVDD.dll", "mttvdd.cat")) {
        Copy-Item -Force (Join-Path $driverDir $name) (Join-Path $targetDir $name)
    }

    # Verify that the selected user-mode display driver is actually AMD64
    # (IMAGE_FILE_MACHINE_AMD64 = 0x8664).
    $dllPath = Join-Path $targetDir "MttVDD.dll"
    $peBytes = [IO.File]::ReadAllBytes($dllPath)
    if ($peBytes.Length -lt 256 -or $peBytes[0] -ne 0x4D -or $peBytes[1] -ne 0x5A) {
        throw "MttVDD.dll is not a valid PE image."
    }
    $peOffset = [BitConverter]::ToInt32($peBytes, 0x3C)
    if ($peOffset -lt 0 -or $peOffset + 6 -gt $peBytes.Length -or
        $peBytes[$peOffset] -ne 0x50 -or $peBytes[$peOffset + 1] -ne 0x45) {
        throw "MttVDD.dll has an invalid PE header."
    }
    $machine = [BitConverter]::ToUInt16($peBytes, $peOffset + 4)
    if ($machine -ne 0x8664) {
        throw ("MttVDD.dll is not AMD64/x64 (PE machine=0x{0:X4})." -f $machine)
    }

    foreach ($name in @("MttVDD.dll", "mttvdd.cat")) {
        $path = Join-Path $targetDir $name
        $signature = Get-AuthenticodeSignature -FilePath $path
        $signer = $signature.SignerCertificate
        Write-Host "$name signature: $($signature.Status)"
        if ($null -eq $signer -or $signature.Status -eq "NotSigned" -or
            $signature.Status -eq "HashMismatch") {
            throw "$name does not contain an intact Authenticode signature."
        }
        $identity = "$($signer.Subject) $($signer.Issuer)"
        if ($identity -notmatch '(?i)SignPath') {
            throw "$name is not signed through the expected SignPath-based VirtualDrivers chain."
        }
        if ($signature.Status -ne "Valid") {
            Write-Warning "$name is signed, but this build machine does not yet trust the publisher chain ($($signature.Status)). The elevated runtime installer establishes the bundled VDD SignPath chain before driver installation."
        }
    }

    Get-FileHash -Algorithm SHA256 -Path (Join-Path $targetDir "MttVDD.inf"),
        (Join-Path $targetDir "MttVDD.dll"),
        (Join-Path $targetDir "mttvdd.cat") |
        ForEach-Object { "$($_.Hash)  $([IO.Path]::GetFileName($_.Path))" } |
        Set-Content -Encoding ASCII (Join-Path $targetDir "VDD_SHA256.txt")

    Write-Host "Imported VirtualDrivers VDD $($release.tag_name) into:"
    Write-Host "  $targetDir"
}
finally {
    Remove-Item -Recurse -Force $tempRoot -ErrorAction SilentlyContinue
}
