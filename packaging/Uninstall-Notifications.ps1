$ErrorActionPreference = 'Stop'
$certificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new((Join-Path $PSScriptRoot 'WHAT.cer'))
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::OpenRead((Join-Path $PSScriptRoot 'WHAT-Notifications.msix'))
try {
    $entry=$zip.GetEntry('AppxManifest.xml')
    if (!$entry -or $entry.Length -gt 1MB) { throw 'Package manifest missing or too large.' }
    $reader=[IO.StreamReader]::new($entry.Open())
    try { $manifest=[xml]::new(); $manifest.XmlResolver=$null; $manifest.LoadXml($reader.ReadToEnd()) } finally { $reader.Dispose() }
} finally { $zip.Dispose() }
$identity=$manifest.Package.Identity
if($identity.Name -notin @('WHAT.Desktop','WHAT.Development') -or $identity.Publisher -ne $certificate.Subject){throw 'Unexpected package identity.'}
$packages=@(Get-AppxPackage -Name $identity.Name | Where-Object Publisher -eq $identity.Publisher)
if($packages.Count -ne 1){throw 'Could not identify exactly one matching installation. Use Windows Settings > Apps to select the package to remove.'}
Write-Host 'This removes the installed WHAT app. Your profile and any separate portable copies are retained.'
if ((Read-Host 'Type REMOVE to continue') -cne 'REMOVE') { return }
$packages[0] | Remove-AppxPackage
Write-Host 'You can run the portable WHAT.exe again. Certificate trust is retained because other installed packages may use it.'
