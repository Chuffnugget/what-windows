param(
    [Parameter(Mandatory=$true)][string]$OpenSSL,
    [string]$OutputDirectory = (Join-Path $env:TEMP ('what-renewal-tests-' + [guid]::NewGuid().ToString('N')))
)
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot/../scripts/Release.Common.ps1"
. "$PSScriptRoot/../scripts/Signing.Common.ps1"
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$savedPassword = $env:WHAT_NEW_PFX_PASSWORD
$savedConfig = $env:OPENSSL_CONF
try {
    $env:OPENSSL_CONF = 'NUL'
    $env:WHAT_NEW_PFX_PASSWORD = [guid]::NewGuid().ToString('N')
    $oldKey = Join-Path $OutputDirectory 'fixture-old-key.pem'
    $oldPem = Join-Path $OutputDirectory 'fixture-old.pem'
    Invoke-Checked $OpenSSL @('req','-x509','-newkey','rsa:2048','-sha256','-days','1','-subj','/O=WHAT Test Fixture/CN=Publisher With Spaces','-passout','env:WHAT_NEW_PFX_PASSWORD','-keyout',$oldKey,'-out',$oldPem)
    New-RenewalCertificate $OpenSSL $oldPem $OutputDirectory
    Invoke-Checked $OpenSSL @('verify','-check_ss_sig','-CAfile',(Join-Path $OutputDirectory 'new-certificate.pem'),(Join-Path $OutputDirectory 'new-certificate.pem'))
    $oldDer = Join-Path $OutputDirectory 'fixture-old.cer'
    $newDer = Join-Path $OutputDirectory 'fixture-new.cer'
    Invoke-Checked $OpenSSL @('x509','-in',$oldPem,'-outform','DER','-out',$oldDer)
    Invoke-Checked $OpenSSL @('x509','-in',(Join-Path $OutputDirectory 'new-certificate.pem'),'-outform','DER','-out',$newDer)
    $old = [Security.Cryptography.X509Certificates.X509Certificate2]::new($oldDer)
    $new = [Security.Cryptography.X509Certificates.X509Certificate2]::new($newDer)
    try {
        if ($old.Subject -ne $new.Subject) { throw 'Renewal changed the established publisher subject.' }
        if ($old.GetPublicKeyString() -eq $new.GetPublicKeyString()) { throw 'Renewal did not create a replacement key.' }
        if ($new.NotAfter -le [DateTime]::Now.AddYears(4)) { throw 'Unexpected renewal validity.' }
        $eku = $new.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' }
        if ('1.3.6.1.5.5.7.3.3' -notin $eku.EnhancedKeyUsages.Value) { throw 'Renewal lacks code-signing usage.' }
        if ((Get-PackageVersion '0.7.8-beta.3') -ne '0.7.8.3') { throw 'Invalid beta transition version.' }
    } finally { $old.Dispose(); $new.Dispose() }
    Write-Host 'PASS: renewal preserves the complete publisher subject, creates a new signing key and retains code-signing usage. No real signing identity or certificate store was changed.'
} finally {
    foreach ($name in @('fixture-old-key.pem','new-key-encrypted.pem')) {
        $path = Join-Path $OutputDirectory $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path }
    }
    $env:WHAT_NEW_PFX_PASSWORD = $savedPassword
    $env:OPENSSL_CONF = $savedConfig
}
