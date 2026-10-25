#Requires -Version 7.0
param([string]$OutputDirectory = (Join-Path $env:TEMP ('what-pipeline-' + [guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$fixture = Join-Path $OutputDirectory 'clone with spaces'
New-Item -ItemType Directory -Path "$fixture/scripts", "$fixture/tests", "$fixture/build" -Force | Out-Null
Copy-Item -LiteralPath "$root/scripts/Invoke-ReleasePipeline.ps1" -Destination "$fixture/scripts"
@'
$script:ProjectRoot = Split-Path $PSScriptRoot -Parent
function Get-ProjectVersion { '0.1.0' }
'@ | Set-Content "$fixture/scripts/Release.Common.ps1"
foreach ($name in @('release-policy-tests', 'release-upload-tests', 'setup-layout-tests')) {
    @'
$root = Split-Path $PSScriptRoot -Parent
$name = [IO.Path]::GetFileNameWithoutExtension($PSCommandPath)
Add-Content "$root/build/calls.txt" $name
Write-Host 'warning C4996: intentional fixture output'
if ((Get-Content "$root/build/failure.txt") -eq $name) { exit 9 }
'@ | Set-Content "$fixture/tests/$name.ps1"
}
@'
param($Architecture, [switch]$Clean, $ResultPath)
@{ architecture=$Architecture; clean=[bool]$Clean } | ConvertTo-Json | Set-Content $ResultPath
'@ | Set-Content "$fixture/scripts/Build-Release.ps1"
@'
param($Architecture, $ReleaseDirectory)
$root = Split-Path $PSScriptRoot -Parent
$expected = Get-Content "$root/build/package-arguments.json" -Encoding UTF8 -Raw | ConvertFrom-Json
if ($ReleaseDirectory -ne $expected.releaseDirectory) { throw 'Wrong release directory.' }
$suffix = if ($Architecture -eq 'x64') { '' } else { '-arm64' }
@{version='0.1.0'; updatePackage=$expected.packagePath} | ConvertTo-Json | Set-Content "$root/build/latest-installers$suffix.json"
'@ | Set-Content "$fixture/scripts/Build-Installers.ps1"
@'
param($PackagePath, $CertificatePath)
$root = Split-Path $PSScriptRoot -Parent
if ($PSVersionTable.PSVersion.Major -ne 5) { throw 'Updater tests must use Windows PowerShell.' }
$expected = Get-Content "$root/build/package-arguments.json" -Encoding UTF8 -Raw | ConvertFrom-Json
if ($PackagePath -ne $expected.packagePath -or $CertificatePath -ne $expected.certificatePath) { throw 'Wrong payload or certificate passed to updater tests.' }
Add-Content "$root/build/calls.txt" 'updater'
if ((Get-Content "$root/build/failure.txt") -eq 'updater') { exit 11 }
'@ | Set-Content "$fixture/tests/update-helper-tests.ps1"

function Reset-Fixture([string]$Failure = '') {
    Set-Content "$fixture/build/failure.txt" $Failure
    Set-Content "$fixture/build/calls.txt" ''
}
function Get-Calls { (@(Get-Content "$fixture/build/calls.txt" | Where-Object { $_ }) -join ',') }
Reset-Fixture
$output = & "$fixture/scripts/Invoke-ReleasePipeline.ps1" -Stage Preflight -Tag v0.1.0 6>&1 | Out-String
if ((Get-Calls) -ne 'release-policy-tests,release-upload-tests,setup-layout-tests') { throw 'Preflight omitted or reordered tests.' }
if ($output -match 'C4996') { throw 'Intentional fixture warnings leaked into the compilation report.' }
if ((Get-ChildItem "$fixture/build/logs/validation" -Recurse -Filter '*.txt' | Select-String 'C4996').Count -ne 3) { throw 'Fixture output was not retained in logs.' }
Write-Host 'PASS: shared preflight checks, isolated shells and fixture logs.'
foreach ($failure in @('release-policy-tests', 'release-upload-tests', 'setup-layout-tests')) {
    Reset-Fixture $failure
    $rejected = $false
    try { & "$fixture/scripts/Invoke-ReleasePipeline.ps1" -Stage Preflight 6>$null }
    catch { $rejected = $_.Exception.Message -like "$failure failed with exit code 9*" }
    if (!$rejected -or (Get-Calls).Split(',')[-1] -ne $failure) { throw "Failed preflight did not stop at $failure." }
}
Reset-Fixture
$rejected = $false
try { & "$fixture/scripts/Invoke-ReleasePipeline.ps1" -Stage Preflight -Tag v0.2.0 }
catch { $rejected = $_.Exception.Message -like 'Release tag*does not match*' }
if (!$rejected -or (Get-Calls)) { throw 'Tag mismatch must fail before validation work.' }
Write-Host 'PASS: preflight failures and tag mismatches stop immediately.'
foreach ($architecture in @('x64', 'arm64')) {
    foreach ($clean in @($false, $true)) {
        & "$fixture/scripts/Invoke-ReleasePipeline.ps1" -Stage Build -Architecture $architecture -Clean:$clean -ResultPath "$fixture/build/result.json"
        $result = Get-Content "$fixture/build/result.json" -Raw | ConvertFrom-Json
        if ($result.architecture -ne $architecture -or $result.clean -ne $clean) { throw 'Build arguments changed.' }
    }
    # Check the caller's literal arguments, not paths rebuilt from PSScriptRoot:
    # PowerShell expands Windows short names there, including CI's TEMP alias.
    # The dot segment also exercises differing spellings on machines without 8.3 names.
    $releaseDirectory = "$fixture/build/./release with spaces"
    $packagePath = "$fixture/build/$architecture/update.msix"
    @{
        releaseDirectory = $releaseDirectory
        packagePath = $packagePath
        certificatePath = Join-Path (Split-Path $packagePath -Parent) 'MSIX/WHAT.cer'
    } | ConvertTo-Json | Set-Content "$fixture/build/package-arguments.json"
    foreach ($failure in @('', 'updater')) {
        Reset-Fixture $failure
        $rejected = $false
        try { & "$fixture/scripts/Invoke-ReleasePipeline.ps1" -Stage Package -Architecture $architecture -ReleaseDirectory $releaseDirectory 6>$null }
        catch { $rejected = $_.Exception.Message -like 'update-helper-tests failed with exit code 11*'; if (!$rejected) { throw } }
        if ($rejected -ne [bool]$failure -or (Get-Calls) -ne 'updater') { throw 'Signed-package validation result was lost.' }
    }
}
Write-Host 'PASS: both architectures, literal path forwarding, clean forwarding and signed-package failures.'

# Execute only build-directory selection from the real script: clean builds must
# avoid existing CMake caches without deleting the incremental tree or keys.
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile("$root/scripts/Build-Release.ps1", [ref]$tokens, [ref]$errors)
$cleanBlock = $ast.Find({ param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Extent.Text.StartsWith('if ($Clean)') }, $true)
$ProjectRoot = $fixture; $runId = 'fixture'; $kitName = 'qt-fixture'; $Clean = $true
$build = "$fixture/build/release/$kitName"
New-Item -ItemType Directory -Path $build -Force | Out-Null
Set-Content "$build/CMakeCache.txt" 'keep existing cache'
. ([scriptblock]::Create($cleanBlock.Extent.Text))
if ($build -notlike "$fixture*build*release*clean-*qt-fixture" -or (Test-Path -LiteralPath $build) -or !(Test-Path "$fixture/build/release/$kitName/CMakeCache.txt")) { throw 'Clean build must select a new tree and retain previous output.' }
Write-Host 'PASS: clean compilation directory preserves existing builds.'

# Actions and Build-All must delegate all validation stages instead of growing
# independent lists of checks again.
foreach ($path in @("$root/scripts/Build-All.ps1", "$root/.github/workflows/windows-release.yml")) {
    $source = Get-Content $path -Raw
    foreach ($stage in @('Preflight', 'Build', 'Package')) {
        if ($source -notmatch "Invoke-ReleasePipeline\.ps1[^\r\n]*-Stage $stage\b") { throw "Missing shared $stage stage in $path" }
    }
}
Write-Host 'PASS: local and GitHub entry points use all shared stages.'
