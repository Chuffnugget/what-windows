[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PackagePath,
    [Parameter(Mandatory)][ValidatePattern('^[a-fA-F0-9]{64}$')][string]$ExpectedHash,
    [Parameter(Mandatory)][version]$ExpectedVersion,
    [Parameter(Mandatory)][string]$Family,
    [Parameter(Mandatory)][int]$ParentId,
    [switch]$AllowDowngrade,
    [switch]$ValidateOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$folder = Split-Path -Parent $PackagePath
$parentProcess = $null
$restart = $false
$acknowledged = $false
$failureCode = ''
try {
    if ($Family -notmatch '^WHAT\.Desktop_[a-z0-9]+$') { throw 'Unexpected package family.' }
    $installed = @(Get-AppxPackage -Name 'WHAT.Desktop' | Where-Object PackageFamilyName -eq $Family)
    if ($installed.Count -ne 1) { throw 'The installed WHAT package could not be identified.' }
    $installed = $installed[0]
    if ($installed.Publisher -ne 'CN=WHAT') { throw 'Unsupported signing publisher. Install the current WHAT release manually.' }
    if ($ExpectedVersion -eq [version]$installed.Version) { throw 'This package version is already installed.' }
    if ($ExpectedVersion -lt [version]$installed.Version -and !$AllowDowngrade) { throw 'A downgrade requires explicit confirmation in WHAT.' }
    if ((Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash -ne $ExpectedHash) { throw 'Package checksum mismatch.' }
    $signature = Get-AuthenticodeSignature -LiteralPath $PackagePath
    if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate) {
        $failureCode = if ($signature.SignerCertificate -and $signature.SignerCertificate.NotAfter.ToUniversalTime() -le [DateTime]::UtcNow) { 'certificate_expired' } else { 'signature_untrusted' }
        throw 'Windows could not verify the downloaded package signature. Nothing was installed. Get the latest Setup.exe from the official WHAT GitHub release; do not bypass signature checks.'
    }
    # Pin to the certificate that signed the already installed package, not to a
    # certificate supplied by GitHub or by the downloaded update.
    Add-Type -AssemblyName System.Security
    $signedData = [IO.File]::ReadAllBytes((Join-Path $installed.InstallLocation 'AppxSignature.p7x'))
    if ($signedData.Length -lt 5 -or [Text.Encoding]::ASCII.GetString($signedData,0,4) -ne 'PKCX') { throw 'Installed package signature is unreadable.' }
    $cms = New-Object System.Security.Cryptography.Pkcs.SignedCms
    $cms.Decode([byte[]]$signedData[4..($signedData.Length-1)])
    $cms.CheckSignature($true)
    if ($cms.SignerInfos.Count -ne 1) { throw 'Installed package must have one signing identity.' }
    $currentThumbprint = $cms.SignerInfos[0].Certificate.Thumbprint
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($PackagePath)
    try {
        $entry = $zip.GetEntry('AppxManifest.xml')
        if (!$entry -or $entry.Length -gt 1048576) { throw 'Missing or oversized package manifest.' }
        $reader = New-Object IO.StreamReader($entry.Open())
        try {
            $xml = New-Object System.Xml.XmlDocument
            $xml.XmlResolver = $null
            $xml.LoadXml($reader.ReadToEnd())
        } finally { $reader.Dispose() }
        if ($currentThumbprint -ne $signature.SignerCertificate.Thumbprint) {
            $failureCode = 'certificate_changed'
            throw 'The signing certificate differs from the installed version. Download and run Setup.exe from the official WHAT GitHub release. No uninstall or settings reset is needed.'
        }
        $identity = $xml.Package.Identity
        if ($identity.Name -ne $installed.Name -or $identity.Publisher -ne $installed.Publisher -or $identity.ProcessorArchitecture -notin @('x64', 'arm64') -or $identity.ProcessorArchitecture -ne $installed.Architecture.ToString() -or [version]$identity.Version -ne $ExpectedVersion) { throw 'Update package identity, architecture or version does not match.' }
    } finally { $zip.Dispose() }
    if ($ValidateOnly) { Write-Output 'Update validation passed; installation skipped.'; exit 0 }
    $parentProcess = [Diagnostics.Process]::GetProcessById($ParentId)
    # Keep a process handle so PID reuse cannot cause us to wait for another app.
    $null = $parentProcess.Handle
    Set-Content -LiteralPath (Join-Path $folder 'ready') -Value ([DateTimeOffset]::UtcNow.ToUnixTimeSeconds())
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (!(Test-Path -LiteralPath (Join-Path $folder 'ack'))) {
        if ([DateTime]::UtcNow -gt $deadline) { throw 'Application did not acknowledge the update; installation cancelled.' }
        Start-Sleep -Milliseconds 100
    }
    $acknowledged = $true
    Remove-Item -LiteralPath (Join-Path $folder 'ready') -ErrorAction SilentlyContinue
    if (!$parentProcess.WaitForExit(60000)) { throw 'WHAT did not exit; no update was installed. Close WHAT in Task Manager, then reopen it or run the new Setup.exe.' }
    $restart = $true
    if ((Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash -ne $ExpectedHash) { throw 'Package changed after verification.' }
    Add-AppxPackage -Path $PackagePath -ForceUpdateFromAnyVersion:$AllowDowngrade -ErrorAction Stop
    Remove-Item -LiteralPath $PackagePath -ErrorAction SilentlyContinue
    Set-Content -LiteralPath (Join-Path $folder 'result.txt') -Value "Installed $ExpectedVersion"
} catch {
    Remove-Item -LiteralPath (Join-Path $folder 'ready') -ErrorAction SilentlyContinue
    $message = $_.Exception.Message
    # A stable reason lets the GUI offer recovery without parsing translated Windows errors.
    [IO.File]::WriteAllText((Join-Path $folder 'error-code.txt'), $failureCode, [Text.Encoding]::UTF8)
    [IO.File]::WriteAllText((Join-Path $folder 'error.txt'), $message, [Text.Encoding]::UTF8)
    if ($acknowledged) {
        Add-Type -AssemblyName PresentationFramework
        [System.Windows.MessageBox]::Show("Update failed. Your previous installation has been retained.`n$message", 'WHAT update') | Out-Null
    }
    exit 1
} finally {
    if ($parentProcess) { $parentProcess.Dispose() }
    if ($restart) { Start-Process -FilePath "$env:SystemRoot\explorer.exe" -ArgumentList "shell:AppsFolder\$Family!App" -WindowStyle Hidden }
}
