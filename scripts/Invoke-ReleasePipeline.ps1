#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Preflight', 'Build', 'Package')][string]$Stage,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [switch]$Clean,
    [string]$Tag,
    [string]$ResultPath,
    [string]$ReleaseDirectory
)
. "$PSScriptRoot/Release.Common.ps1"

# Local builds and Actions enter through these same stages. Fixture tests run in
# separate shells because they replace Windows APIs and intentionally emit errors.
# Keep that fixture output in its own log, not in compiler warning totals.
function Invoke-PipelineTest([string]$Name, [string]$Shell, [string[]]$Arguments = @()) {
    $directory = Join-Path $ProjectRoot ('build/logs/validation/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 6))
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $log = Join-Path $directory "$Name.txt"
    Write-Host "WHAT build task: Validating $Name"
    Write-Host "Validation log: $log"
    & $Shell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$ProjectRoot/tests/$Name.ps1" @Arguments *> $log
    $testExitCode = $LASTEXITCODE
    if ($testExitCode -ne 0) {
        Get-Content -LiteralPath $log -Tail 40 | ForEach-Object { Write-Host $_ }
        throw "$Name failed with exit code $testExitCode. See $log"
    }
    Write-Host "PASS: $Name"
}

switch ($Stage) {
    'Preflight' {
        $version = Get-ProjectVersion
        if ($Tag -and $Tag -ne "v$version") {
            throw "Release tag '$Tag' does not match source version '$version' (expected 'v$version'). Commit the version bump before tagging."
        }
        $shell = Join-Path $PSHOME 'pwsh.exe'
        foreach ($name in @('release-policy-tests', 'release-upload-tests', 'setup-layout-tests')) {
            Invoke-PipelineTest $name $shell
        }
    }
    'Build' {
        & "$PSScriptRoot/Build-Release.ps1" -Architecture $Architecture -Clean:$Clean -ResultPath $ResultPath
    }
    'Package' {
        & "$PSScriptRoot/Build-Installers.ps1" -Architecture $Architecture -ReleaseDirectory $ReleaseDirectory
        $suffix = if ($Architecture -eq 'x64') { '' } else { '-arm64' }
        $result = Get-Content "$ProjectRoot/build/latest-installers$suffix.json" -Raw | ConvertFrom-Json
        if ($result.version -ne (Get-ProjectVersion)) { throw "Packaging did not produce the current version for $Architecture." }
        # Test the exact signed payload and certificate distributed by this build.
        # The real updater runs in Windows PowerShell, so its tests do too.
        $certificate = Join-Path (Split-Path $result.updatePackage -Parent) 'MSIX/WHAT.cer'
        Invoke-PipelineTest 'update-helper-tests' "$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe" @(
            '-PackagePath', $result.updatePackage, '-CertificatePath', $certificate)
    }
}
