[CmdletBinding(DefaultParameterSetName='Path')]
param(
    [Parameter(Mandatory=$true,ParameterSetName='Path')][string]$CertificatePath,
    [Parameter(Mandatory=$true,ParameterSetName='Bytes')][string]$CertificateBase64,
    [Parameter(Mandatory=$true)][string]$ExpectedThumbprint
)
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
    $bytes = if ($PSCmdlet.ParameterSetName -eq 'Bytes') { [Convert]::FromBase64String($CertificateBase64) } else { [IO.File]::ReadAllBytes($CertificatePath) }
    $certificate = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new([byte[]]$bytes)
    if ($certificate.Thumbprint -ne $ExpectedThumbprint) { throw 'Certificate changed after confirmation.' }
    Assert-CodeSigningCertificate $certificate
    # Import the exact object validated above. Never reopen a mutable certificate path.
    $store = [Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople', 'LocalMachine')
    try {
        $store.Open([Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $store.Add($certificate)
    } finally { $store.Dispose() }
    if (!(Test-Path -LiteralPath ('Cert:\LocalMachine\TrustedPeople\' + $certificate.Thumbprint))) { throw 'Certificate import did not persist in Local Machine / Trusted People.' }
    exit 0
} catch { Write-Error $_; exit 1 }
