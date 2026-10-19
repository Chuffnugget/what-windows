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

function Initialize-SigningIdentity($Config, [string]$OpenSSL, [string]$Private) {
    $subject = Get-Configured $Config 'Publisher' ''
    if (!$subject) { throw 'Configure Publisher and the existing SigningPfx/SigningCertificate. Packaging never generates a signing identity.' }
    $pfx = Get-Configured $Config 'SigningPfx' (Join-Path $Private 'publisher.pfx')
    $cer = Get-Configured $Config 'SigningCertificate' (Join-Path $Private 'publisher.cer')
    if (!(Test-Path -LiteralPath $pfx -PathType Leaf) -or !(Test-Path -LiteralPath $cer -PathType Leaf)) { throw 'Signing identity missing. Restore the existing PFX and CER; do not generate replacements for an established release.' }
    if ($subject -ne 'CN=WHAT') { throw 'Only the CN=WHAT signing publisher is supported.' }
    $protectedPassword = Get-Configured $Config 'SigningPasswordFile' (Join-Path $Private 'password.dpapi')
    Add-Type -AssemblyName System.Security
    if (!$env:WHAT_PFX_PASSWORD) {
        if (Test-Path -LiteralPath $protectedPassword) {
            $env:WHAT_PFX_PASSWORD = [Text.Encoding]::UTF8.GetString([Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($protectedPassword),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser))
        } else {
            throw 'Signing password unavailable. Configure SigningPasswordFile or supply WHAT_PFX_PASSWORD.'
        }
    }
    # Verify the configured encrypted PFX.
    Invoke-Checked $OpenSSL @('pkcs12','-in',$pfx,'-noout','-passin','env:WHAT_PFX_PASSWORD')
    $certificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2($cer)
    if ($certificate.Subject -ne $subject) { throw 'Signing certificate subject does not match Publisher.' }
    # Keep verification exports with the configured identity, not in a second
    # default signing folder when local configuration overrides its location.
    $identityDirectory = Split-Path -Parent $pfx
    $publicPem = Join-Path $identityDirectory 'pfx-public.pem'; $publicCer = Join-Path $identityDirectory 'pfx-public.cer'
    Invoke-Checked $OpenSSL @('pkcs12','-in',$pfx,'-clcerts','-nokeys','-out',$publicPem,'-passin','env:WHAT_PFX_PASSWORD')
    Invoke-Checked $OpenSSL @('x509','-in',$publicPem,'-outform','DER','-out',$publicCer)
    $pfxCertificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2($publicCer)
    try {
        if ($pfxCertificate.Thumbprint -ne $certificate.Thumbprint) { throw 'Public signing certificate does not match the PFX.' }
    } finally { $pfxCertificate.Dispose() }
    Assert-CodeSigningCertificate $certificate
    $days = ($certificate.NotAfter.ToUniversalTime() - [DateTime]::UtcNow).TotalDays
    if ($days -le 0) { throw 'Signing certificate expired. Renew before packaging.' }
    if ($days -le 90) { Write-Warning "Signing certificate expires in $([int]$days) days. Arrange renewal now." }
    return @{ Pfx=$pfx; Cer=$cer; Publisher=$subject; Certificate=$certificate }
}

# Explicit renewal only. Copy the established X.509 subject through a CSR rather
# than constructing a replacement publisher string or relying on a legacy name.
function New-RenewalCertificate([string]$OpenSSL, [string]$OldCertificatePem, [string]$Destination) {
    $key = Join-Path $Destination 'new-key-encrypted.pem'
    $request = Join-Path $Destination 'renewal.csr'
    $certificate = Join-Path $Destination 'new-certificate.pem'
    Invoke-Checked $OpenSSL @('genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:3072','-aes-256-cbc','-pass','env:WHAT_NEW_PFX_PASSWORD','-out',$key)
    Invoke-Checked $OpenSSL @('x509','-x509toreq','-in',$OldCertificatePem,'-signkey',$key,'-passin','env:WHAT_NEW_PFX_PASSWORD','-out',$request)
    Invoke-Checked $OpenSSL @('req','-x509','-in',$request,'-key',$key,'-passin','env:WHAT_NEW_PFX_PASSWORD','-sha256','-days','1825','-addext','basicConstraints=critical,CA:FALSE','-addext','keyUsage=critical,digitalSignature','-addext','extendedKeyUsage=codeSigning','-out',$certificate)
}
