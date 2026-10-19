[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Executable,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$ExpectedVersion,
    [string]$IntegrationRoot = $env:WHAT_INTEGRATION_SOURCE
)
. "$PSScriptRoot/Release.Common.ps1"
$bundled = Join-Path $ProjectRoot 'tests/integration_contract'
$manifest = Get-Content "$bundled/source.json" -Raw | ConvertFrom-Json
if (!$IntegrationRoot) {
    $sibling = Join-Path (Split-Path $ProjectRoot -Parent) 'what-integration'
    $IntegrationRoot = if (Test-Path -LiteralPath $sibling) { $sibling } else { $bundled }
}
$IntegrationRoot = [IO.Path]::GetFullPath($IntegrationRoot)
foreach ($entry in $manifest.files.PSObject.Properties) {
    foreach ($root in @($bundled, $IntegrationRoot)) {
        $file = Join-Path $root $entry.Name
        if (!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.Value) {
            throw "Integration contract differs: $file. Run scripts/Sync-IntegrationContract.ps1 against the intended integration checkout, review, and commit the updated contract before releasing."
        }
    }
}
# Release PATH includes MSYS2 for OpenSSL. Its Python uses a different venv
# layout and wheels; select the native Windows interpreter explicitly.
$python = $null
foreach ($candidate in @(Get-Command python.exe -All -ErrorAction SilentlyContinue)) {
    if ($candidate.Source -like '*\WindowsApps\*') { continue }
    $scheme = & $candidate.Source -c "import sys, sysconfig; print(sysconfig.get_default_scheme() if sys.version_info >= (3,12) else 'old')"
    if ($LASTEXITCODE -eq 0 -and $scheme -eq 'nt') { $python = $candidate.Source; break }
}
if (!$python) { throw 'Native Windows Python 3.12+ is required on PATH for the integration contract tests (not MSYS2 Python).' }
$venv = Join-Path $ProjectRoot 'build/contract-python-win32'
if (!(Test-Path "$venv/Scripts/python.exe")) { Invoke-Checked $python @('-m','venv',$venv) }
$python = "$venv/Scripts/python.exe"
& $python -c "import importlib.util, sys; sys.exit(0 if all(importlib.util.find_spec(m) for m in ('pytest','aiohttp','voluptuous','yaml')) else 1)"
if ($LASTEXITCODE -ne 0) { Invoke-Checked $python @('-m','pip','install','pytest','aiohttp','voluptuous','PyYAML') }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$previousExe = $env:WHAT_TEST_EXE; $previousProfile = $env:WHAT_TEST_PROFILE; $previousVersion = $env:WHAT_EXPECTED_VERSION
try {
    $env:WHAT_TEST_EXE = [IO.Path]::GetFullPath($Executable)
    $env:WHAT_TEST_PROFILE = Join-Path $OutputDirectory 'profile'
    $env:WHAT_EXPECTED_VERSION = $ExpectedVersion
    Push-Location $IntegrationRoot
    try {
        "Integration contract source: $IntegrationRoot; manifest SHA256: $((Get-FileHash "$bundled/source.json" -Algorithm SHA256).Hash); WHAT version: $ExpectedVersion" | Set-Content "$OutputDirectory/revisions.txt"
        Invoke-Checked $python @('-B','-m','pytest','-q','-p','no:cacheprovider','tests/test_live_app.py','--basetemp',"$OutputDirectory/temp",'--junitxml',"$OutputDirectory/results.xml")
    } finally { Pop-Location }
} finally {
    $env:WHAT_TEST_EXE = $previousExe; $env:WHAT_TEST_PROFILE = $previousProfile; $env:WHAT_EXPECTED_VERSION = $previousVersion
}
