[CmdletBinding()]
param([Parameter(Mandatory)][string]$ReleaseDirectory)
. "$PSScriptRoot/Release.Common.ps1"
$release = (Resolve-Path -LiteralPath $ReleaseDirectory).Path
$state = Get-Content -LiteralPath "$release/release.json" -Raw | ConvertFrom-Json
$native = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
if ($state.architecture -eq 'arm64' -and $native -ne 'ARM64') { throw 'ARM64 runtime tests require a Windows ARM64 PC.' }
if ($state.architecture -notin @('x64', 'arm64')) { throw 'Invalid release architecture.' }
if ($state.version -ne (Get-ProjectVersion)) { throw 'Release version differs from this checkout.' }
$sources = Get-SourceHashes
if ($sources.Count -ne @($state.sources.PSObject.Properties).Count) { throw 'Source files changed since compilation.' }
foreach ($entry in $state.sources.PSObject.Properties) {
    if (!$sources.Contains($entry.Name) -or $sources[$entry.Name] -ne $entry.Value) { throw "Source changed since compilation: $($entry.Name)" }
}
$runtime = Join-Path $release 'portable'
$tests = Join-Path $release 'test-runtime'
$logs = Join-Path $release 'validation'
New-Item -ItemType Directory -Path $logs -Force | Out-Null
foreach ($folder in @($runtime, $tests)) {
    $hashes = Get-Content -LiteralPath "$folder/SHA256SUMS.json" -Raw | ConvertFrom-Json
    $files = @(Get-ChildItem -LiteralPath $folder -Recurse -File | Where-Object Name -ne 'SHA256SUMS.json')
    if ($files.Count -ne @($hashes.PSObject.Properties).Count) { throw "Runtime file set changed: $folder" }
    foreach ($entry in $hashes.PSObject.Properties) {
        $path = Join-Path $folder $entry.Name
        if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.Value) {
            throw "Runtime changed since compilation: $path"
        }
    }
    & "$PSScriptRoot/Test-RuntimeArchitecture.ps1" -Runtime $folder -Architecture $state.architecture
}
$oldPath = $env:PATH
$oldPlugins = $env:QT_PLUGIN_PATH
$oldQmlImports = $env:QML_IMPORT_PATH
$oldQml2Imports = $env:QML2_IMPORT_PATH
$oldPlatformPlugins = $env:QT_QPA_PLATFORM_PLUGIN_PATH
try {
    # Native tests must use the deployment's DLLs and QML, not the Qt kit.
    $env:PATH = "$runtime;$env:SystemRoot/System32;$env:SystemRoot"
    $env:QT_PLUGIN_PATH = $runtime
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = "$runtime/platforms"
    $env:QML_IMPORT_PATH = $null
    $env:QML2_IMPORT_PATH = $null
    Invoke-QtTests "$tests/controller_tests.exe" "$logs/controller-tests.txt"
    Write-Host 'WHAT build task: Testing hardware readers'
    Invoke-Checked "$tests/reader_tests.exe" @()
    Invoke-QtTests "$tests/update_tests.exe" "$logs/update-tests.txt"
    Invoke-QtTests "$tests/settings_tests.exe" "$logs/settings-tests.txt"
    # The contract harness needs the configured Windows Python on PATH.
    $env:PATH = "$runtime;$oldPath"
    Write-Host 'WHAT build task: Checking the integration contract'
    & "$PSScriptRoot/Test-IntegrationContract.ps1" -Executable "$runtime/WHAT.exe" -OutputDirectory "$logs/integration-contract" -ExpectedVersion $state.version
    Write-Host 'WHAT build task: Testing packaged application startup'
    # Keep developer Qt/OpenSSL folders out of the deployment smoke test.
    $env:PATH = "$env:SystemRoot/System32;$env:SystemRoot"
    $env:QT_PLUGIN_PATH = $runtime
    $smoke = Start-Process -FilePath "$runtime/WHAT.exe" -ArgumentList @('--test-mode', '--data-dir', "`"$release/smoke-profile`"", '--quit-after', '3') -WindowStyle Hidden -PassThru -Wait -RedirectStandardError "$logs/startup.txt"
    if ($smoke.ExitCode -ne 0) { throw "Packaged startup failed ($($smoke.ExitCode))." }
    $state.tests = 'passed'
    $state.release = $release
    $state.runtime = $runtime
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$release/release.json" -Encoding UTF8
    $suffix = if ($state.architecture -eq 'x64') { '' } else { '-arm64' }
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$ProjectRoot/build/latest-release$suffix.json" -Encoding UTF8
    Write-Host "Runtime tests passed: $($state.architecture)" -ForegroundColor Green
} finally {
    $env:PATH = $oldPath
    $env:QT_PLUGIN_PATH = $oldPlugins
    $env:QML_IMPORT_PATH = $oldQmlImports
    $env:QML2_IMPORT_PATH = $oldQml2Imports
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = $oldPlatformPlugins
}
