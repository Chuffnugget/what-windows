param([Parameter(Mandatory=$true)][string]$CertificatePath,[Parameter(Mandatory=$true)][string]$ExpectedThumbprint)
$ErrorActionPreference = 'Stop'
function Assert-CodeSigningCertificate($Certificate) {
    $now = [DateTime]::UtcNow
    if ($Certificate.NotBefore.ToUniversalTime() -gt $now -or $Certificate.NotAfter.ToUniversalTime() -le $now) { throw 'Signing certificate is outside its validity period.' }
    $eku = @($Certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' })
    $usage = @($Certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.15' })
    $constraints = @($Certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.19' })
    if ($eku.Count -ne 1 -or '1.3.6.1.5.5.7.3.3' -notin @($eku[0].EnhancedKeyUsages | ForEach-Object Value)) { throw 'Certificate must explicitly allow code signing.' }
    if ($usage.Count -ne 1 -or !($usage[0].KeyUsages -band [Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature)) { throw 'Certificate must allow digital signatures.' }
    if ($constraints.Count -ne 1 -or $constraints[0].CertificateAuthority) { throw 'Signing certificate must be a leaf certificate, not a certificate authority.' }
}


try {
    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($CertificatePath)
    if ($certificate.Thumbprint -ne $ExpectedThumbprint) { throw 'Certificate changed after confirmation.' }
    Assert-CodeSigningCertificate $certificate
    Import-Certificate -FilePath $CertificatePath -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople' | Out-Null
    if (!(Test-Path -LiteralPath ('Cert:\LocalMachine\TrustedPeople\' + $certificate.Thumbprint))) { throw 'Certificate import did not persist in Local Machine / Trusted People.' }
    exit 0
} catch { Write-Error $_; exit 1 }
