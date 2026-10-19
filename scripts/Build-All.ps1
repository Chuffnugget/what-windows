[CmdletBinding()]
param(
    [switch]$ValidateOnly,
    [ValidateSet('all', 'x64', 'arm64')][string]$Architecture = 'all',
    [Parameter(DontShow)][switch]$BuildWorker
)
$ErrorActionPreference = 'Stop'
if (!$BuildWorker) {
    . "$PSScriptRoot/Build.Progress.ps1"
    $buildExitCode = Invoke-BuildProgress -ScriptPath $PSCommandPath -Architecture $Architecture -ValidateOnly:$ValidateOnly
    if ($buildExitCode -ne 0) { throw "Build-All failed with exit code $buildExitCode. See the build output and compilation report above." }
    return
}
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
. "$PSScriptRoot/Release.Common.ps1"
. "$PSScriptRoot/Signing.Common.ps1"
. "$PSScriptRoot/Build.Report.ps1"
$architectures = if ($Architecture -eq 'all') { @('x64', 'arm64') } else { @($Architecture) }
$report = New-BuildReport $ProjectRoot $architectures ([bool]$ValidateOnly)
$failureMessage = ''
$transcribing = $false
$previousPassword = $env:WHAT_PFX_PASSWORD
$previousOpenSSLConfig = $env:OPENSSL_CONF
try {
    Start-Transcript -LiteralPath $report.log | Out-Null
    $transcribing = $true
    Set-BuildReportStage $report 'Prerequisites'
    $configuration = Read-Configuration
    $report.version = Get-ProjectVersion
    # Validate every requested toolchain before spending time on either build.
    foreach ($target in $architectures) {
        $releaseTools = Get-ReleaseTools $configuration $target -PrepareDependencies:(!$ValidateOnly)
        Write-Host "Qt ($target): $($releaseTools.Qt)"
        $report.tools.Add(@{ architecture=$target; qt=$releaseTools.Qt; compilerVersion=$releaseTools.CompilerVersion })
    }
    Set-BuildReportStage $report 'Signing'
    $env:OPENSSL_CONF = 'NUL'
    Write-Host 'Checking the existing signing identity before building...' -ForegroundColor Cyan
    $signingIdentity = Initialize-SigningIdentity $configuration $releaseTools.OpenSSL (Join-Path $ProjectRoot '_local/signing')
    try {
        # Signing can succeed without trust; SignTool verification cannot. Check
        # the exact self-signed certificate before spending time on compilation.
        $certificate = $signingIdentity.Certificate
        if ($certificate.Subject -eq $certificate.Issuer) {
            $thumbprint = $certificate.Thumbprint
            $trusted = (Test-Path -LiteralPath "Cert:\LocalMachine\TrustedPeople\$thumbprint") -or
                       (Test-Path -LiteralPath "Cert:\LocalMachine\Root\$thumbprint") -or
                       (Test-Path -LiteralPath "Cert:\CurrentUser\Root\$thumbprint")
            if (!$trusted) {
                throw "The configured signing certificate ($($certificate.Subject), $thumbprint) is not trusted on this build PC. Before building, run packaging/Trust-Certificate.ps1 in an administrator PowerShell with -CertificatePath '$($signingIdentity.Cer)' -ExpectedThumbprint '$thumbprint'. This trusts only the configured certificate in Local Machine / Trusted People. No build or tests have been started."
            }
        }
        Write-Host "Publisher: $($signingIdentity.Publisher)"
    } finally { $signingIdentity.Certificate.Dispose() }
    if ($ValidateOnly) { $report.status = 'Passed'; Write-Host 'Release prerequisites and signing identity verified.'; return }
    # Run the existing scripts in order. A failure stops the complete release.
    foreach ($target in $architectures) {
        $resultPath = Join-Path $ProjectRoot ('build/build-all-' + [guid]::NewGuid().ToString('N') + '.json')
        try {
            Set-BuildReportStage $report "$target Build"
            # Flush native compiler output while the transcript is active.
            & "$PSScriptRoot/Build-Release.ps1" -Architecture $target -ResultPath $resultPath | Out-Host
            $built = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
            $releaseState = Get-Content -LiteralPath (Join-Path $built.release 'release.json') -Raw | ConvertFrom-Json
            $output = [ordered]@{ architecture=$target; release=$built.release; runtimeTests=$releaseState.tests; installer=$null; portable=$null }
            $report.outputs.Add($output)
            Set-BuildReportStage $report "$target Packaging"
            & "$PSScriptRoot/Build-Installers.ps1" -Architecture $target -ReleaseDirectory $built.release | Out-Host
        } finally {
            if (Test-Path -LiteralPath $resultPath) { Remove-Item -LiteralPath $resultPath -Force }
        }
        $stateName = if ($target -eq 'x64') { 'latest-installers.json' } else { 'latest-installers-arm64.json' }
        $artifacts = Get-Content (Join-Path $ProjectRoot "build/$stateName") -Raw | ConvertFrom-Json
        if ($artifacts.version -ne (Get-ProjectVersion)) { throw "Packaging did not produce the current version for $target." }
        $output.installer = $artifacts.installer
        $output.portable = $artifacts.portable
        Write-Host "$target installers created: $(Split-Path $artifacts.installer -Parent)" -ForegroundColor Green
        Write-Host 'Includes Setup.exe, MSIX/update manifest, portable ZIP and source archives.'
    }
    $report.status = 'Passed'
    Write-Host 'Requested builds completed. Nothing is uploaded.'
    $native = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    $recommended = if ($native -eq 'ARM64') { 'arm64' } else { 'x64' }
    foreach ($output in $report.outputs) {
        $label = if ($output.architecture -eq 'x64') { 'Intel / AMD PCs (x64)' } else { 'Windows on ARM (ARM64)' }
        Write-Host "${label}: $($output.installer)"
    }
    foreach ($output in $report.outputs) {
        if ($output.architecture -eq $recommended) {
            Write-Host "Installer for this PC: $($output.installer)" -ForegroundColor Cyan
        }
    }
} catch {
    $failureMessage = $_.Exception.Message
    throw
} finally {
    $env:WHAT_PFX_PASSWORD = $previousPassword
    $env:OPENSSL_CONF = $previousOpenSSLConfig
    Complete-BuildReportStage $report $report.status
    if ($transcribing) { Stop-Transcript | Out-Null }
    # Reporting must not replace the original compilation or signing failure.
    try { Write-BuildReport $report $failureMessage }
    catch { Write-Warning "Could not write the compilation report: $($_.Exception.Message). Build log: $($report.log)" }
}
