param([Parameter(Mandatory)][string]$PackagePath, [Parameter(Mandatory)][string]$CertificatePath)
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.IO.Compression.FileSystem
$testRoot = Join-Path $env:TEMP ('WHAT-update-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$archive = [IO.Compression.ZipFile]::OpenRead($PackagePath)
$manifestReader = [IO.StreamReader]::new($archive.GetEntry('AppxManifest.xml').Open())
try {
    $identity = ([xml]$manifestReader.ReadToEnd()).Package.Identity
    $targetVersion = [version]$identity.Version
    $global:whatTestArchitecture = [string]$identity.ProcessorArchitecture
} finally { $manifestReader.Dispose() }
try { [IO.Compression.ZipFileExtensions]::ExtractToFile($archive.GetEntry('AppxSignature.p7x'), (Join-Path $testRoot 'AppxSignature.p7x')) } finally { $archive.Dispose() }
$global:whatTestCertificate = New-Object Security.Cryptography.X509Certificates.X509Certificate2($CertificatePath)
$global:whatTestTrusted = $true
$global:whatTestPublisher = $global:whatTestCertificate.Subject
$global:whatTestVersion = '0.0.0.0'
# Mock only the Windows installation/trust boundary: never install or trust anything.
function Get-AppxPackage { param($Name) [pscustomobject]@{ Name='WHAT.Desktop'; PackageFamilyName='WHAT.Desktop_test'; Publisher=$global:whatTestPublisher; Version=$global:whatTestVersion; Architecture=$global:whatTestArchitecture; InstallLocation=$testRoot } }
function Get-AuthenticodeSignature { param($LiteralPath) [pscustomobject]@{ Status=$(if($global:whatTestTrusted){'Valid'}else{'NotTrusted'}); SignerCertificate=$global:whatTestCertificate } }
function Add-AppxPackage { throw 'TEST FAILURE: installer must never run in validation mode' }
$helper = Join-Path (Split-Path $PSScriptRoot) 'scripts/Apply-Update.ps1'
$hash = (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash
$argsForHelper = @{ PackagePath=$PackagePath; ExpectedHash=$hash; ExpectedVersion=$targetVersion; Family='WHAT.Desktop_test'; ParentId=$PID; ValidateOnly=$true }
# Use a test-local payload path so error logs do not touch release staging.
Copy-Item -LiteralPath $PackagePath -Destination "$testRoot/update.msix"
$argsForHelper.PackagePath = "$testRoot/update.msix"
& $helper @argsForHelper
if ($LASTEXITCODE -ne 0) { throw 'Valid package failed validation' }
$packageArchitecture = $global:whatTestArchitecture
$global:whatTestArchitecture = if ($packageArchitecture -eq 'x64') { 'arm64' } else { 'x64' }
& $helper @argsForHelper
if ($LASTEXITCODE -ne 1) { throw 'Package for a different architecture was accepted' }
$global:whatTestArchitecture = $packageArchitecture
$global:whatTestTrusted = $false
& $helper @argsForHelper
if ($LASTEXITCODE -ne 1) { throw 'Untrusted package was accepted' }
if ((Get-Content "$testRoot/error-code.txt" -Raw).Trim() -ne 'signature_untrusted') { throw 'Missing untrusted-signature recovery reason' }
# Expired and untrusted signing certificates need different recovery explanations.
$validCert = $global:whatTestCertificate
$global:whatTestCertificate = [pscustomobject]@{ Thumbprint=$validCert.Thumbprint; NotAfter=[DateTime]::UtcNow.AddDays(-1) }
& $helper @argsForHelper
if ($LASTEXITCODE -ne 1 -or (Get-Content "$testRoot/error-code.txt" -Raw).Trim() -ne 'certificate_expired') { throw 'Expired signature did not report its recovery reason' }
# Windows can still accept an expired certificate when the package has a valid timestamp.
$global:whatTestTrusted = $true
& $helper @argsForHelper
if ($LASTEXITCODE -ne 0) { throw 'Certificate expiry overrode a valid Windows signature result' }
$global:whatTestCertificate = $validCert
$global:whatTestTrusted = $true
$argsForHelper.ExpectedHash = '0' * 64
& $helper @argsForHelper
if ($LASTEXITCODE -ne 1) { throw 'Corrupt package was accepted' }
if ([IO.File]::ReadAllText("$testRoot/error-code.txt").Trim()) { throw 'Checksum failure was incorrectly reported as a certificate failure' }
$argsForHelper.ExpectedHash = $hash
$global:whatTestVersion = $targetVersion
& $helper @argsForHelper
if ($LASTEXITCODE -ne 1) { throw 'Same-version package was accepted' }
$global:whatTestVersion = ([version]::new($targetVersion.Major+1,0,0,0))
& $helper @argsForHelper
if ($LASTEXITCODE -ne 1) { throw 'Downgrade was accepted' }
$argsForHelper.AllowDowngrade = $true
& $helper @argsForHelper
if ($LASTEXITCODE -ne 0) { throw 'Explicit downgrade failed validation' }
$global:whatTestTrusted = $false
& $helper @argsForHelper
if ($LASTEXITCODE -ne 1) { throw 'Explicit downgrade bypassed signature trust' }
$global:whatTestTrusted = $true
$argsForHelper.Remove('AllowDowngrade')
$global:whatTestVersion = '0.0.0.0'
$savedCert = $global:whatTestCertificate
$global:whatTestCertificate = [pscustomobject]@{ Thumbprint=('0' * 40) }
& $helper @argsForHelper
if ($LASTEXITCODE -ne 1) { throw 'Changed signing identity was accepted' }
if ((Get-Content "$testRoot/error-code.txt" -Raw).Trim() -ne 'certificate_changed' -or
    (Get-Content "$testRoot/error.txt" -Raw) -notmatch 'No uninstall or settings reset') { throw 'Changed certificate lacks manual-install recovery instructions' }
$global:whatTestCertificate = $savedCert
$argsForHelper.ExpectedVersion = ([version]::new($targetVersion.Major,$targetVersion.Minor,$targetVersion.Build,$targetVersion.Revision+1))
& $helper @argsForHelper
if ($LASTEXITCODE -ne 1) { throw 'Manifest version mismatch was accepted' }
Write-Output 'Passed updater signature, recovery and version checks (Windows trust/installation mocked; installed signature parsed and cryptographically verified).'


$argsForHelper.ExpectedVersion=$targetVersion
$global:whatTestCertificate=$savedCert
$argsForHelper.ExpectedHash=$hash; $argsForHelper.ValidateOnly=$false
& $helper @argsForHelper
if ($LASTEXITCODE -ne 1 -or (Test-Path "$testRoot/ready") -or (Get-Content "$testRoot/error.txt" -Raw) -notmatch 'did not acknowledge') { throw 'Unacknowledged handoff was not cancelled cleanly' }
Write-Output 'Passed handoff check; no package installed.'

# Exercise a stuck parent without hanging a real WHAT process or opening a dialog.
Add-Type -TypeDefinition @'
namespace System.Windows {
    public static class MessageBox {
        public static string LastMessage;
        public static int Show(string message, string title) { LastMessage = message; return 0; }
    }
}
'@
function Add-Type {
    param([string]$AssemblyName)
    if ($AssemblyName -ne 'PresentationFramework') {
        Microsoft.PowerShell.Utility\Add-Type -AssemblyName $AssemblyName
    }
}
$global:whatTestParentDisposed = $false
$global:whatTestParentWait = 0
function Get-TestParentProcess {
    $process = [pscustomobject]@{ Handle = 1 }
    $process | Add-Member ScriptMethod WaitForExit { param($milliseconds) $global:whatTestParentWait = $milliseconds; return $false }
    $process | Add-Member ScriptMethod Dispose { $global:whatTestParentDisposed = $true }
    return $process
}
function Start-Process { throw 'TEST FAILURE: a stuck parent must not trigger a restart' }
$helperSource = Get-Content -LiteralPath $helper -Raw
$processCall = '[Diagnostics.Process]::GetProcessById($ParentId)'
if (!$helperSource.Contains($processCall)) { throw 'Update process fixture needs to match the helper process lookup.' }
$fixtureHelper = Join-Path $testRoot 'Apply-Update.ps1'
Set-Content -LiteralPath $fixtureHelper -Value $helperSource.Replace($processCall, '(Get-TestParentProcess)') -Encoding UTF8
Set-Content -LiteralPath "$testRoot/ack" -Value 'ack'
& $fixtureHelper @argsForHelper
if ($LASTEXITCODE -ne 1 -or (Test-Path "$testRoot/ready")) { throw 'Stuck-parent handoff was not cancelled.' }
if ([System.Windows.MessageBox]::LastMessage -notmatch 'WHAT did not exit; no update was installed' -or
    [System.Windows.MessageBox]::LastMessage -notmatch 'Task Manager') { throw 'Shutdown failure was not shown with recovery instructions.' }
if (!$global:whatTestParentDisposed -or $global:whatTestParentWait -ne 60000) { throw 'Parent process wait or handle cleanup changed.' }
Write-Output 'Passed shutdown timeout check; failure shown, no installation or restart attempted.'
exit 0
