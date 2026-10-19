# Optional, user-run installer. Never invoked by WHAT itself.
param([switch]$FromSetup, [string]$FamilyOutputPath, [switch]$InspectOnly, [string]$InspectionOutputPath, [string]$ExpectedInstalledVersion, [switch]$AllowDowngrade)
$ErrorActionPreference = 'Stop'
$packagePath = Join-Path $PSScriptRoot 'WHAT-Notifications.msix'
$certificatePath = Join-Path $PSScriptRoot 'WHAT.cer'
$certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($certificatePath)
if ($certificate.Subject -ne 'CN=WHAT') { throw 'Only the CN=WHAT signing publisher is supported.' }
# Read the identity from the actual payload, never guess by product name alone.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($packagePath)
try {
    $entry = $archive.GetEntry('AppxManifest.xml')
    if (!$entry -or $entry.Length -gt 1MB) { throw 'Package manifest missing or too large.' }
    $reader = [IO.StreamReader]::new($entry.Open())
    try { $manifest = [xml]::new(); $manifest.XmlResolver = $null; $manifest.LoadXml($reader.ReadToEnd()) } finally { $reader.Dispose() }
} finally { $archive.Dispose() }
$identity = $manifest.Package.Identity
if ($identity.ProcessorArchitecture -notin @('x64', 'arm64')) { throw 'Unsupported WHAT package architecture.' }
if ($identity.ProcessorArchitecture -eq 'arm64') {
    $native = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    if ($native -ne 'ARM64') { throw 'This installer requires a Windows ARM64 PC. Download the x64 installer for this computer.' }
    if ([Environment]::OSVersion.Version.Build -lt 22621) { throw 'Native ARM64 WHAT requires Windows 11 22H2 or newer.' }
}
if ($identity.Name -notin @('WHAT.Desktop','WHAT.Development') -or $identity.Publisher -ne $certificate.Subject) { throw 'Package identity does not match the supplied publisher certificate.' }
# Inspect only this user's registration for this product, never unrelated apps.
$existing = @(Get-AppxPackage -Name $identity.Name)
$matching = @($existing | Where-Object { $_.Publisher -eq $identity.Publisher })
$foreign = @($existing | Where-Object { $_.Publisher -ne $identity.Publisher })
$mode = 'install'
$previousVersion = 'none'
if ($foreign.Count) { $mode = 'publisher-change'; $previousVersion = [string]$foreign[0].Version }
elseif ($matching.Count -gt 1) { $mode = 'ambiguous' }
elseif ($matching.Count -eq 1) {
    $previousVersion = [string]$matching[0].Version
    if ([version]$previousVersion -gt [version]$identity.Version) { $mode = 'downgrade' }
    elseif ([version]$previousVersion -eq [version]$identity.Version) { $mode = 'reinstall' }
    else { $mode = 'update' }
}
if ($InspectOnly) {
    if (!$InspectionOutputPath) { throw 'Inspection output path required.' }
    [IO.File]::WriteAllLines($InspectionOutputPath, [string[]]@($mode, $previousVersion, [string]$identity.Version))
    $currentLocation = if ($matching.Count -eq 1) { [string]$matching[0].InstallLocation } else { '' }
    [IO.File]::WriteAllText($InspectionOutputPath + '.location', $currentLocation)
    return
}
if ($mode -eq 'publisher-change') { throw 'An installation with a different WHAT publisher exists. Back up your profile, uninstall the old WHAT entry in Windows Settings, then run this installer. It cannot be overwritten as an update.' }
if ($mode -eq 'ambiguous') { throw 'Multiple matching WHAT installations found. Resolve these in Windows Settings before installing.' }
if ($mode -eq 'downgrade' -and !$AllowDowngrade) { throw "Downgrade from $previousVersion to $($identity.Version) requires explicit confirmation. Use Setup or pass -AllowDowngrade to this script." }
if ($FromSetup -and $ExpectedInstalledVersion -ne $previousVersion) { throw 'The installed WHAT version changed since confirmation. Retry Setup to review the current installation.' }
Write-Host ("Installation action: $mode; installed: $previousVersion; installer: " + $identity.Version)
$signature = Get-AuthenticodeSignature -LiteralPath $packagePath
if (!$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) { throw 'Package signer does not match the supplied certificate.' }
# Windows reports an untrusted self-signed MSIX as UnknownError on some versions.
# Only the post-trust Valid check below authorizes installation.
if ($signature.Status -notin @('Valid','NotTrusted','UnknownError')) { throw ('Package signature is invalid: ' + $signature.Status) }
Write-Host 'Install WHAT by Chuffnugget (including optional Windows notification access).'
Write-Host ('Signing certificate subject: ' + $certificate.Subject)
Write-Host 'It trusts the included signing certificate in Local Machine / Trusted People.'
Write-Host 'Windows may request administrator approval for that certificate step.'
Write-Host ('Certificate SHA-1 thumbprint: ' + $certificate.Thumbprint)
Write-Host 'Quit WHAT first. Your existing profile is retained.'
if (!$FromSetup -and (Read-Host 'Type INSTALL to continue') -cne 'INSTALL') { return }
if (!(Test-Path -LiteralPath ('Cert:\LocalMachine\TrustedPeople\' + $certificate.Thumbprint))) {
    $trustScript = Join-Path $PSScriptRoot 'Trust-Certificate.ps1'
    $helper = Start-Process -FilePath 'powershell.exe' -Verb RunAs -WindowStyle Hidden -Wait -PassThru -ArgumentList @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File',('"' + $trustScript + '"'),'-CertificatePath',('"' + $certificatePath + '"'),'-ExpectedThumbprint',$certificate.Thumbprint)
    if ($helper.ExitCode -ne 0) { throw 'Certificate trust was not granted; package installation cancelled.' }
}
if (!(Test-Path -LiteralPath ('Cert:\LocalMachine\TrustedPeople\' + $certificate.Thumbprint))) {
    throw 'The certificate was not found in Local Machine / Trusted People after the trust step. Installation cancelled.'
}
$signature = Get-AuthenticodeSignature -LiteralPath $packagePath
if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $certificate.Thumbprint) { throw 'Package signature could not be verified after certificate trust. Installation cancelled.' }
try {
    Add-AppxPackage -Path $packagePath -ForceUpdateFromAnyVersion:($mode -eq 'downgrade' -and $AllowDowngrade) -ErrorAction Stop
} catch {
    Write-Host 'Installation failed. If Windows reports 0x800B010A or 0x800B0109, verify that this MSIX and CER came from the same release and that machine policy permits certificate trust.'
    Write-Host ('Expected signing certificate: ' + $certificate.Subject + ' / ' + $certificate.Thumbprint)
    throw
}
$installed = @(Get-AppxPackage -Name $identity.Name | Where-Object { $_.Publisher -eq $identity.Publisher -and $_.Version -eq [version]$identity.Version -and $_.Architecture.ToString() -eq $identity.ProcessorArchitecture })
if ($installed.Count -ne 1) { throw 'Could not resolve the exact installed package identity.' }
if ($FamilyOutputPath) {
    [IO.File]::WriteAllText($FamilyOutputPath, $installed[0].PackageFamilyName)
    [IO.File]::WriteAllText($FamilyOutputPath + '.location', $installed[0].InstallLocation)
}
Write-Host 'Installed. Launch WHAT from Start, enable notification collection, then click Request notification access.'
Write-Host 'Window titles and notification contents remain off until you enable them separately.'
