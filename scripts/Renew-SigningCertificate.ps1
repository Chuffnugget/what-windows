[CmdletBinding()]
param([string]$NewSigningPfx, [string]$NewSigningCertificate)
. "$PSScriptRoot/Release.Common.ps1"
. "$PSScriptRoot/Signing.Common.ps1"
$config = Read-Configuration
$state = Get-Content -LiteralPath "$ProjectRoot/build/latest-release.json" -Raw | ConvertFrom-Json
$openssl = $state.tools.OpenSSL
$savedPassword = $env:WHAT_PFX_PASSWORD
$savedNewPassword = $env:WHAT_NEW_PFX_PASSWORD
$savedConfig = $env:OPENSSL_CONF
$destination = Join-Path $ProjectRoot ('_local/signing-renewal/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $destination -Force | Out-Null
try {
    $env:OPENSSL_CONF = 'NUL'
    $old = Initialize-SigningIdentity $config $openssl (Join-Path $ProjectRoot '_local/signing')
    $oldCert = Join-Path $destination 'old-certificate.pem'
    Invoke-Checked $openssl @('x509','-inform','DER','-in',$old.Cer,'-out',$oldCert)
    if ($NewSigningPfx -or $NewSigningCertificate) {
        if (!$NewSigningPfx -or !$NewSigningCertificate -or !$env:WHAT_NEW_PFX_PASSWORD) { throw 'Supply both new PFX/CER paths and WHAT_NEW_PFX_PASSWORD.' }
        Copy-Item -LiteralPath $NewSigningPfx -Destination "$destination/publisher.pfx"
        Copy-Item -LiteralPath $NewSigningCertificate -Destination "$destination/publisher.cer"
    } else {
        $env:WHAT_NEW_PFX_PASSWORD = [guid]::NewGuid().ToString('N') + [guid]::NewGuid().ToString('N')
        New-RenewalCertificate $openssl $oldCert $destination
        Invoke-Checked $openssl @('x509','-in',"$destination/new-certificate.pem",'-outform','DER','-out',"$destination/publisher.cer")
        Invoke-Checked $openssl @('pkcs12','-export','-inkey',"$destination/new-key-encrypted.pem",'-in',"$destination/new-certificate.pem",'-out',"$destination/publisher.pfx",'-passin','env:WHAT_NEW_PFX_PASSWORD','-passout','env:WHAT_NEW_PFX_PASSWORD')
    }
    $new = New-Object Security.Cryptography.X509Certificates.X509Certificate2("$destination/publisher.cer")
    Assert-CodeSigningCertificate $new
    if ($new.Subject -ne $old.Publisher -or $new.NotAfter -le [DateTime]::Now) { throw 'New certificate must have the same publisher and be unexpired.' }
    Invoke-Checked $openssl @('pkcs12','-in',"$destination/publisher.pfx",'-noout','-passin','env:WHAT_NEW_PFX_PASSWORD')
    Invoke-Checked $openssl @('pkcs12','-in',"$destination/publisher.pfx",'-clcerts','-nokeys','-out',"$destination/pfx-public.pem",'-passin','env:WHAT_NEW_PFX_PASSWORD')
    Invoke-Checked $openssl @('x509','-in',"$destination/pfx-public.pem",'-outform','DER','-out',"$destination/pfx-public.cer")
    $pfxCertificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2("$destination/pfx-public.cer")
    if ($pfxCertificate.Thumbprint -ne $new.Thumbprint) { throw 'New public certificate does not match the PFX.' }
    $protected = [Security.Cryptography.ProtectedData]::Protect([Text.Encoding]::UTF8.GetBytes($env:WHAT_NEW_PFX_PASSWORD),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser)
    [IO.File]::WriteAllBytes("$destination/password.dpapi",$protected)
    # Point local builds at the complete new identity only after every step succeeds.
    $config.SigningPfx = "$destination/publisher.pfx"
    $config.SigningCertificate = "$destination/publisher.cer"
    $config.SigningPasswordFile = "$destination/password.dpapi"
    [void]$config.Remove('SigningTransitionDirectory')
    $config.Publisher = $old.Publisher
    $config | ConvertTo-Json | Set-Content -LiteralPath "$ProjectRoot/release.local.json" -Encoding UTF8
    Write-Host "Renewal prepared: $destination. Run Build-Installers.cmd. No C++ recompile required."
    Write-Host 'Certificate changes require manual installation; automatic migration from older certificates is not supported.'
    Write-Host 'Self-signed replacements still require explicit Windows certificate trust during manual installation.'
} finally {
    foreach ($file in @('old-key-encrypted.pem','new-key-encrypted.pem')) { $path = Join-Path $destination $file; if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path } }
    $env:WHAT_PFX_PASSWORD = $savedPassword; $env:WHAT_NEW_PFX_PASSWORD = $savedNewPassword; $env:OPENSSL_CONF = $savedConfig
}
