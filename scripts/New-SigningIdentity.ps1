[CmdletBinding()]
param([switch]$ReplacePublisher)
. "$PSScriptRoot/Release.Common.ps1"
. "$PSScriptRoot/Signing.Common.ps1"
Add-Type -AssemblyName System.Security
$config = Read-Configuration
$publisher = Get-Configured $config 'Publisher' ''
if ($publisher -eq 'CN=WHAT') { throw 'WHAT identity already configured. Use Renew-SigningCertificate.ps1 for renewal; do not replace it.' }
if ($publisher -and !$ReplacePublisher) { throw 'Changing publisher requires -ReplacePublisher and a manual reinstall for existing users.' }
$destination = Join-Path $ProjectRoot ('_local/signing-what/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $destination | Out-Null
# Restrict access before writing any key material. No certificate store imports.
$acl = [Security.AccessControl.DirectorySecurity]::new()
$acl.SetAccessRuleProtection($true, $false)
foreach ($sid in @([Security.Principal.WindowsIdentity]::GetCurrent().User, [Security.Principal.SecurityIdentifier]::new('S-1-5-18'), [Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))) {
    $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow'))
}
Set-Acl -LiteralPath $destination -AclObject $acl
$rsa = [Security.Cryptography.RSA]::Create(3072)
$certificate = $null
$roundtrip = $null
try {
    $request = [Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=WHAT', $rsa, [Security.Cryptography.HashAlgorithmName]::SHA256, [Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false,$false,0,$true))
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new([Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature,$true))
    $oids = [Security.Cryptography.OidCollection]::new()
    [void]$oids.Add([Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))
    $request.CertificateExtensions.Add([Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($oids,$false))
    $certificate = $request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddMinutes(-5),[DateTimeOffset]::UtcNow.AddYears(5))
    $certificate.FriendlyName = 'WHAT'
    Assert-CodeSigningCertificate $certificate
    $random = New-Object byte[] 48
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($random) } finally { $rng.Dispose() }
    $password = [Convert]::ToBase64String($random)
    $pfx = $certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Pfx,$password)
    $roundtrip = [Security.Cryptography.X509Certificates.X509Certificate2]::new($pfx,$password,[Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
    if (!$roundtrip.HasPrivateKey -or $roundtrip.Thumbprint -ne $certificate.Thumbprint) { throw 'PFX validation failed.' }
    [IO.File]::WriteAllBytes("$destination/publisher.pfx",$pfx)
    [IO.File]::WriteAllBytes("$destination/publisher.cer",$certificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
    [IO.File]::WriteAllBytes("$destination/password.dpapi",[Security.Cryptography.ProtectedData]::Protect([Text.Encoding]::UTF8.GetBytes($password),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser))
    $configPath = Join-Path $ProjectRoot 'release.local.json'
    if (Test-Path -LiteralPath $configPath) { Copy-Item -LiteralPath $configPath -Destination "$destination/previous-release.local.json" }
    $config.Publisher='CN=WHAT'
    $config.PublisherDisplayName='WHAT'
    $config.SigningPfx="$destination/publisher.pfx"
    $config.SigningCertificate="$destination/publisher.cer"
    $config.SigningPasswordFile="$destination/password.dpapi"
    [void]$config.Remove('SigningTransitionDirectory')
    $tempConfig="$destination/release.local.json"
    $config | ConvertTo-Json | Set-Content -LiteralPath $tempConfig -Encoding UTF8
    if (Test-Path -LiteralPath $configPath) { [IO.File]::Replace($tempConfig,$configPath,"$destination/replaced-release.local.json") }
    else { [IO.File]::Move($tempConfig,$configPath) }
    Write-Host "Created CN=WHAT signing identity; previous identity preserved. Expires $($certificate.NotAfter.ToString('yyyy-MM-dd'))."
    Write-Host 'Windows trust stores and installed applications were not changed.'
} finally {
    if ($roundtrip) { $roundtrip.Dispose() }
    if ($certificate) { $certificate.Dispose() }
    $rsa.Dispose()
    $password=$null; $pfx=$null
}

