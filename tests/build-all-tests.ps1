param([string]$OutputDirectory = (Join-Path $env:TEMP ('what-build-all-tests-' + [guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$fixture = Join-Path $OutputDirectory 'clone with spaces'
New-Item -ItemType Directory -Path "$fixture/scripts", "$fixture/build" -Force | Out-Null
Copy-Item -LiteralPath "$root/scripts/Build-All.ps1", "$root/scripts/Build.Report.ps1" -Destination "$fixture/scripts"
@'
param($Stage, $Architecture, $ResultPath, $ReleaseDirectory, [switch]$Clean)
switch ($Stage) {
    'Preflight' {
        $fixtureCalls.Add('preflight')
        if ($fixtureFailure -eq 'preflight') { throw 'fixture preflight failure' }
    }
    'Build' {
        if ([bool]$Clean -ne [bool]$fixtureClean) { throw 'Clean option was not forwarded.' }
        & "$PSScriptRoot/Build-Release.ps1" -Architecture $Architecture -ResultPath $ResultPath
    }
    'Package' { & "$PSScriptRoot/Build-Installers.ps1" -Architecture $Architecture -ReleaseDirectory $ReleaseDirectory }
    default { throw 'Unexpected pipeline stage.' }
}
'@ | Set-Content -LiteralPath "$fixture/scripts/Invoke-ReleasePipeline.ps1" -Encoding UTF8

# Exercise the real orchestrator with only its build/signing boundaries replaced.
# Nothing is compiled, signed, installed or uploaded by these tests.
@'
$script:ProjectRoot = Split-Path $PSScriptRoot -Parent
function Read-Configuration { @{} }
function Get-ProjectVersion { '0.1.0' }
function Get-ReleaseTools($Config, $Architecture, [switch]$PrepareDependencies) {
    if ([bool]$PrepareDependencies -eq [bool]$fixtureValidateOnly) { throw 'Wrong automatic dependency preparation policy.' }
    $fixtureCalls.Add("tools:$Architecture")
    if ($fixtureFailure -eq "tools:$Architecture") { throw $fixtureFailure }
    @{ Qt="fixture/$Architecture"; OpenSSL='fixture/openssl.exe'; CompilerVersion='19.44' }
}
'@ | Set-Content -LiteralPath "$fixture/scripts/Release.Common.ps1" -Encoding UTF8
@'
function Initialize-SigningIdentity($Config, $OpenSSL, $Directory) {
    $fixtureCalls.Add('signing')
    if ($fixtureFailure -eq 'signing') { throw $fixtureFailure }
    $env:WHAT_PFX_PASSWORD = 'temporary fixture value'
    $certificate = [pscustomobject]@{ Subject='CN=Fixture'; Issuer='CN=Fixture CA' }
    $certificate | Add-Member -MemberType ScriptMethod -Name Dispose -Value {}
    @{ Certificate=$certificate; Publisher='CN=Fixture' }
}
'@ | Set-Content -LiteralPath "$fixture/scripts/Signing.Common.ps1" -Encoding UTF8
@'
param($Architecture, $ResultPath)
$fixtureCalls.Add("build:$Architecture")
Write-Host 'fixture.cpp(12): warning C4805: fixture warning'
Write-Host 'fixture.cpp(12): warning C4805: fixture warning'
Write-Host '0 errors, 0 warnings'
# Build-Release uses its own transcript inside Build-All's transcript.
Start-Transcript -Path "$ProjectRoot/build/nested-$Architecture.log" | Out-Null
try { Invoke-Checked $env:ComSpec @('/d', '/c', 'echo warning C4996: native compiler warning & echo warning C4997: native stderr warning 1>&2') }
finally { Stop-Transcript | Out-Null }
if ($fixtureFailure -eq "build:$Architecture") {
    Write-Host 'fixture.cpp(15): error C2039: fixture compile failure'
    throw $fixtureFailure
}
$release = "$ProjectRoot/build/staging/$Architecture"
New-Item -ItemType Directory -Path $release -Force | Out-Null
$testLog = "$release/controller-tests.txt"
Set-Content -LiteralPath $testLog -Value 'QWARN : Fixture::test() warning hidden from live output'
Write-Host "Running controller_tests.exe. Full log: $testLog"
Write-Host 'Totals: 3 passed, 0 failed, 1 skipped, 0 blacklisted'
$native = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
$testState = if ($Architecture -eq 'arm64' -and $native -ne 'ARM64') { 'pending' } else { 'passed' }
@{ tests=$testState } | ConvertTo-Json | Set-Content -LiteralPath "$release/release.json" -Encoding UTF8
@{ release="$ProjectRoot/build/staging/$Architecture" } | ConvertTo-Json | Set-Content -LiteralPath $ResultPath -Encoding UTF8
'@ | Set-Content -LiteralPath "$fixture/scripts/Build-Release.ps1" -Encoding UTF8
@'
param($Architecture, $ReleaseDirectory)
$fixtureCalls.Add("package:$Architecture")
if ($ReleaseDirectory -ne "$ProjectRoot/build/staging/$Architecture") { throw 'Wrong release sent to packaging.' }
if ($fixtureFailure -eq "package:$Architecture") { throw $fixtureFailure }
$state = if ($Architecture -eq 'x64') { 'latest-installers.json' } else { 'latest-installers-arm64.json' }
$version = if ($fixtureFailure -eq 'stale-version') { '0.0.0' } else { '0.1.0' }
@{ version=$version; installer="$ProjectRoot/build/$Architecture/Setup.exe"; portable="$ProjectRoot/build/$Architecture/portable.zip" } |
    ConvertTo-Json | Set-Content -LiteralPath "$ProjectRoot/build/$state" -Encoding UTF8
'@ | Set-Content -LiteralPath "$fixture/scripts/Build-Installers.ps1" -Encoding UTF8
# Use the real native-command capture function without replacing fixture tool discovery.
$parseTokens = $null
$parseErrors = $null
$commonAst = [Management.Automation.Language.Parser]::ParseFile("$root/scripts/Release.Common.ps1", [ref]$parseTokens, [ref]$parseErrors)
$checkedSource = $commonAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-Checked' }, $false).Extent.Text
Add-Content -LiteralPath "$fixture/scripts/Release.Common.ps1" -Value $checkedSource
. ([scriptblock]::Create($checkedSource))
$nativeOutput = @(Invoke-Checked $env:ComSpec @('/d', '/c', 'echo warning: stderr-only diagnostic 1>&2'))
if (($nativeOutput -join '') -notmatch 'stderr-only diagnostic') { throw 'Native stderr was not captured.' }
$nativeFailed = $false
try { Invoke-Checked $env:ComSpec @('/d', '/c', 'exit /b 7') }
catch { $nativeFailed = $_.Exception.Message -match 'exit code 7' }
if (!$nativeFailed) { throw 'Native exit code was not preserved.' }
$missingFailed = $false
try { Invoke-Checked "$fixture/missing.exe" @() }
catch { $missingFailed = $true }
if (!$missingFailed) { throw 'Missing build executable was accepted.' }
Write-Host 'PASS: native stderr, exit code and missing executable checks.'

# CI has both Windows and Git tools on PATH. Resolve one executable in PATH order.
& {
    $findSource = $commonAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Find-Tool' }, $false).Extent.Text
    . ([scriptblock]::Create($findSource))
    $firstDirectory = Join-Path $OutputDirectory 'first tools'
    $secondDirectory = Join-Path $OutputDirectory 'second tools'
    New-Item -ItemType Directory -Path $firstDirectory, $secondDirectory -Force | Out-Null
    $toolName = 'what-path-fixture.exe'
    foreach ($directory in @($firstDirectory, $secondDirectory)) {
        Copy-Item -LiteralPath $env:ComSpec -Destination (Join-Path $directory $toolName)
    }
    $originalPath = $env:PATH
    try {
        $env:PATH = "$firstDirectory;$secondDirectory;$originalPath"
        if (@(Get-Command $toolName -CommandType Application -All).Count -ne 2) { throw 'Expected two fixture executables on PATH.' }
        $output = @(Invoke-Checked $toolName @('/d', '/c', 'echo selected-from-path'))
        if ($output.Count -ne 1 -or $output[0] -ne 'selected-from-path') { throw 'Duplicate executable lookup failed.' }
        $resolved = Find-Tool $toolName @()
        if ($resolved -isnot [string] -or $resolved -ne (Join-Path $firstDirectory $toolName)) { throw 'Tool discovery did not return the first PATH executable.' }
        $override = Join-Path $secondDirectory $toolName
        if ((Find-Tool $toolName @($override)) -ne $override) { throw 'An explicit tool candidate must take priority over PATH.' }
    } finally {
        $env:PATH = $originalPath
    }
}
Write-Host 'PASS: duplicate executable names on PATH and explicit tool priority.'

# Dependency output must still be transcribed when tool discovery returns a value.
$prepareAst = [Management.Automation.Language.Parser]::ParseFile("$root/scripts/Prepare-Arm64OpenSSL.ps1", [ref]$parseTokens, [ref]$parseErrors)
$preparePipeline = $prepareAst.Find({ param($node) $node -is [Management.Automation.Language.PipelineAst] -and $node.Extent.Text.StartsWith('Invoke-Checked $env:ComSpec') }, $false).Extent.Text
$command = "$fixture/dependency-output.cmd"
Set-Content -LiteralPath $command -Value '@echo warning C4998: dependency compiler warning 1>&2'
$dependencyLog = "$fixture/dependency-output.log"
Start-Transcript -LiteralPath $dependencyLog | Out-Null
try { $discovered = & { . ([scriptblock]::Create($preparePipeline)); 'dependency root' } }
finally { Stop-Transcript | Out-Null }
if ($discovered -ne 'dependency root' -or (Get-Content -LiteralPath $dependencyLog -Raw) -notmatch 'C4998') {
    throw 'Dependency compiler diagnostics were not transcribed during tool discovery.'
}
Write-Host 'PASS: dependency compiler transcript during tool discovery.'

# Source classification must retain every diagnostic, including unknown sources
# and ARM64 validation reminders. Dependencies must not inflate WHAT's own count.
. "$root/scripts/Build.Report.ps1"
$sourceReport = New-BuildReport (Join-Path $OutputDirectory 'report sources') @('x64', 'arm64') $false
$sourceReport.status = 'Passed'
$sourceMessages = @(
    @{ Stage='Prerequisites'; Source='OpenSSL'; Text='include\internal\tsan_assist.h(104): warning C4133: fixture OpenSSL warning' },
    @{ Stage='x64 Build'; Source='WHAT'; Text='C:\clone with spaces\src\app\fixture.cpp(12): warning C4805: fixture application warning' },
    @{ Stage='x64 Build'; Source='yaml-cpp'; Text='C:\clone\build\release\_deps\yaml_cpp-src\src\fptostring.cpp(34): warning C4244: fixture YAML warning' },
    @{ Stage='x64 Build'; Source='Other dependency'; Text='C:\clone\build\release\_deps\paho-src\src\fixture.c(5): warning C4244: fixture dependency warning' },
    @{ Stage='x64 Build'; Source='Tests'; Text='QWARN : Fixture::test() fixture Qt warning' },
    @{ Stage='x64 Build'; Source='Qt'; Text='QWARN : Fixture::test() There are still "2" items in the process of being created at engine destruction.' },
    @{ Stage='x64 Build'; Source='Tests'; Text='WARNING: ControllerTests::test() Maximum amount of warnings exceeded.' },
    @{ Stage='x64 Build'; Source='Build configuration'; Text='CMake Warning:' },
    @{ Stage='arm64 Build'; Source='Release validation'; Text='WARNING: ARM64 build staged, not runtime-tested: fixture' },
    @{ Stage='arm64 Packaging'; Source='Release validation'; Text='WARNING: Packaging ARM64 without runtime testing.' },
    @{ Stage='x64 Build'; Source='Other'; Text='fixture.cpp(1): warning C4244: unknown source' }
)
$sourceLines = @(foreach ($message in $sourceMessages) { "WHAT build stage: $($message.Stage)"; $message.Text })
$sourceLines | Set-Content -LiteralPath $sourceReport.log
Write-BuildReport $sourceReport '' 6>$null
$sourceSummary = Get-Content -LiteralPath "$($sourceReport.directory)/compilation-report.json" -Raw | ConvertFrom-Json
if ($sourceSummary.errors -ne 0 -or $sourceSummary.warnings -ne $sourceMessages.Count) { throw 'Diagnostic source classification lost messages.' }
foreach ($message in $sourceMessages) {
    $issue = @($sourceSummary.diagnostics | Where-Object { $_.message -eq ($message.Text -replace '^WARNING:\s*', '') })
    if ($issue.Count -ne 1 -or $issue[0].source -ne $message.Source) { throw "Wrong diagnostic source: $($message.Text)" }
}
foreach ($group in ($sourceMessages | Group-Object -Property { $_.Source })) {
    $count = @($sourceSummary.diagnosticSources | Where-Object { $_.source -eq $group.Name })
    if ($count.Count -ne 1 -or $count[0].warnings -ne $group.Count) { throw "Wrong diagnostic count: $($group.Name)" }
}
Write-Host 'PASS: dependency, WHAT, test, configuration, unknown and ARM64 warning classification.'

$cases = @(
    @{ Name='preflight failure'; Arguments=@{}; Failure='preflight'; Calls='' },
    @{ Name='clean x64'; Arguments=@{Architecture='x64'; Clean=$true}; Calls='tools:x64,signing,build:x64,package:x64' },
    @{ Name='default cross build'; Arguments=@{}; Calls='tools:x64,tools:arm64,signing,build:x64,package:x64,build:arm64,package:arm64' },
    @{ Name='explicit all'; Arguments=@{Architecture='all'}; Calls='tools:x64,tools:arm64,signing,build:x64,package:x64,build:arm64,package:arm64' },
    @{ Name='x64 only'; Arguments=@{Architecture='x64'}; Calls='tools:x64,signing,build:x64,package:x64' },
    @{ Name='ARM64 only'; Arguments=@{Architecture='arm64'}; Calls='tools:arm64,signing,build:arm64,package:arm64' },
    @{ Name='native ARM64'; Arguments=@{}; HostArchitecture='ARM64'; Calls='tools:x64,tools:arm64,signing,build:x64,package:x64,build:arm64,package:arm64' },
    @{ Name='emulated shell on ARM64'; Arguments=@{}; NativeArchitecture='ARM64'; Calls='tools:x64,tools:arm64,signing,build:x64,package:x64,build:arm64,package:arm64' },
    @{ Name='validate only'; Arguments=@{ValidateOnly=$true}; Calls='tools:x64,tools:arm64,signing' },
    @{ Name='missing ARM64 tools'; Arguments=@{}; Failure='tools:arm64'; Calls='tools:x64,tools:arm64' },
    @{ Name='signing failure'; Arguments=@{}; Failure='signing'; Calls='tools:x64,tools:arm64,signing' },
    @{ Name='x64 build failure'; Arguments=@{}; Failure='build:x64'; Calls='tools:x64,tools:arm64,signing,build:x64' },
    @{ Name='x64 packaging failure'; Arguments=@{}; Failure='package:x64'; Calls='tools:x64,tools:arm64,signing,build:x64,package:x64' },
    @{ Name='ARM64 build failure'; Arguments=@{}; Failure='build:arm64'; Calls='tools:x64,tools:arm64,signing,build:x64,package:x64,build:arm64' },
    @{ Name='ARM64 packaging failure'; Arguments=@{}; Failure='package:arm64'; Calls='tools:x64,tools:arm64,signing,build:x64,package:x64,build:arm64,package:arm64' },
    @{ Name='stale installer'; Arguments=@{}; Failure='stale-version'; Calls='tools:x64,tools:arm64,signing,build:x64,package:x64' }
)
$previousEnvironment = @{}
foreach ($name in @('PROCESSOR_ARCHITECTURE', 'PROCESSOR_ARCHITEW6432', 'WHAT_PFX_PASSWORD', 'OPENSSL_CONF')) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
try {
    foreach ($case in $cases) {
        $env:PROCESSOR_ARCHITECTURE = if ($case.HostArchitecture) { $case.HostArchitecture } else { 'AMD64' }
        $env:PROCESSOR_ARCHITEW6432 = $case.NativeArchitecture
        $env:WHAT_PFX_PASSWORD = 'original fixture value'
        $env:OPENSSL_CONF = 'original fixture config'
        $fixtureCalls = [Collections.Generic.List[string]]::new()
        $fixtureFailure = $case.Failure
        $arguments = $case.Arguments
        $fixtureValidateOnly = [bool]$arguments.ValidateOnly
        $fixtureClean = [bool]$arguments.Clean
        $failure = $null
        try { & "$fixture/scripts/Build-All.ps1" -BuildWorker @arguments 6>$null 3>$null }
        catch { $failure = $_ }
        $expectedCalls = $case.Calls
        if (!$fixtureValidateOnly) { $expectedCalls = (@('preflight', $expectedCalls) | Where-Object { $_ }) -join ',' }
        if (($fixtureCalls -join ',') -ne $expectedCalls) {
            throw "Wrong build order for $($case.Name): $($fixtureCalls -join ','). $failure"
        }
        if ([bool]$failure -ne [bool]$case.Failure) { throw "Wrong outcome for $($case.Name): $failure" }
        if (Get-ChildItem "$fixture/build" -Filter 'build-all-*.json') { throw "Temporary build result was not removed: $($case.Name)" }
        if ($env:WHAT_PFX_PASSWORD -ne 'original fixture value' -or $env:OPENSSL_CONF -ne 'original fixture config') {
            throw "Build-All did not restore the environment: $($case.Name)"
        }
        $savedReport = Get-Content -LiteralPath "$fixture/build/compilation-report.json" -Raw | ConvertFrom-Json
        $expectedStatus = if ($case.Failure) { 'Failed' } else { 'Passed' }
        if ($savedReport.status -ne $expectedStatus) { throw "Wrong report result: $($case.Name)" }
        if ($case.Failure -and $savedReport.errors -lt 1) { throw 'Build failure missing from report.' }
        if ($case.Name -eq 'default cross build') {
            $warnings = @($savedReport.diagnostics | Where-Object { $_.stage -eq 'x64 Build' -and $_.message -match 'C4805' })
            if ($warnings.Count -ne 1 -or $warnings[0].mentions -ne 2) { throw 'Repeated compiler warnings were not grouped.' }
            if (!($savedReport.diagnostics | Where-Object { $_.message -match 'C4996' })) { throw 'Native compiler output missing from nested transcript.' }
            if (!($savedReport.diagnostics | Where-Object { $_.message -match 'C4997' })) { throw 'Native compiler stderr missing.' }
            if (!($savedReport.diagnostics | Where-Object { $_.message -match 'hidden from live output' })) { throw 'Full Qt log warning missing.' }
            if (($savedReport.outputs | Where-Object architecture -eq 'arm64').runtimeTests -ne 'pending') { throw 'Untested ARM64 build incorrectly reported.' }
            if (($savedReport.outputs | Where-Object architecture -eq 'x64').runtimeTests -ne 'passed') { throw 'x64 runtime tests missing.' }
            if (!$savedReport.testTotals.PSObject.Properties.Count) { throw 'Test totals missing.' }
        }
        if ($case.Name -eq 'x64 build failure') {
            if (($savedReport.stages | Where-Object name -eq 'x64 Build').status -ne 'Failed') { throw 'Failed stage missing.' }
            if (($savedReport.stages | Where-Object name -eq 'arm64 Build').status -ne 'Not run') { throw 'Unstarted build incorrectly reported.' }
            if (!($savedReport.diagnostics | Where-Object { $_.message -match 'C2039' })) { throw 'Compiler error missing.' }
        }
        if ($savedReport.diagnostics | Where-Object { $_.message -match '^0 errors' }) { throw 'Zero error count was classified as a diagnostic.' }
        Write-Host "PASS: $($case.Name) and compilation report"
    }
} finally {
    foreach ($name in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process')
    }
}

# Run the real installer validation, stopping before hashing or invoking build tools.
Copy-Item -LiteralPath "$root/scripts/Build-Installers.ps1" -Destination "$fixture/scripts" -Force
Add-Content -LiteralPath "$fixture/scripts/Release.Common.ps1" -Value 'function Get-SourceHashes { throw "PACKAGING_GATE_ACCEPTED" }'
Set-Content -LiteralPath "$fixture/scripts/Test-RuntimeArchitecture.ps1" -Value 'param($Runtime, $Architecture)'
$gateCases = @(
    @{ Architecture='x64'; Tests='passed'; Accepted=$true },
    @{ Architecture='x64'; Tests='pending'; Accepted=$false },
    @{ Architecture='x64'; Tests='failed'; Accepted=$false },
    @{ Architecture='arm64'; Tests='passed'; Accepted=$true },
    @{ Architecture='arm64'; Tests='pending'; Accepted=$true },
    @{ Architecture='arm64'; Tests='failed'; Accepted=$false },
    @{ Architecture='arm64'; Tests='pending'; Version='0.0.0'; Accepted=$false },
    @{ Architecture='arm64'; Tests='pending'; StateArchitecture='x64'; Accepted=$false }
)
$fixtureValidateOnly = $true
$oldRefType = $env:GITHUB_REF_TYPE
try {
    $env:GITHUB_REF_TYPE = $null
    foreach ($case in $gateCases) {
        @{
            version = if ($case.Version) { $case.Version } else { '0.1.0' }
            architecture = if ($case.StateArchitecture) { $case.StateArchitecture } else { $case.Architecture }
            tests = $case.Tests
            runtime = "$fixture/build/portable"
        } | ConvertTo-Json | Set-Content -LiteralPath "$fixture/build/release.json" -Encoding UTF8
        $failure = ''
        try { & "$fixture/scripts/Build-Installers.ps1" -Architecture $case.Architecture -ReleaseDirectory "$fixture/build" 3>$null }
        catch { $failure = $_.Exception.Message }
        if (($failure -eq 'PACKAGING_GATE_ACCEPTED') -ne $case.Accepted) {
            throw "Wrong installer gate result for $($case | ConvertTo-Json -Compress): $failure"
        }
    }
    Write-Host "PASS: $($gateCases.Count) installer runtime-test gate cases."
} finally {
    $env:GITHUB_REF_TYPE = $oldRefType
}

# Exercise dependency recovery with real file checks and a local preparation stub.
foreach ($name in @('Get-Configured', 'Get-Arm64OpenSSLRoot')) {
    $definition = $commonAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $false)
    Add-Content -LiteralPath "$fixture/scripts/Release.Common.ps1" -Value $definition.Extent.Text
}
{
    $preparationCalls.Add('prepare')
    if ($dependencyCase.Mode -eq 'failed') { throw 'fixture dependency build failed' }
    if ($dependencyCase.Mode -ne 'incomplete') {
        foreach ($file in $opensslFiles) {
            $path = Join-Path "$ProjectRoot/build/dependencies/openssl-arm64" $file
            New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
            Set-Content -LiteralPath $path -Value 'fixture'
        }
    }
    'Preparation output must not become part of the returned OpenSSL path.'
}.ToString() | Set-Content -LiteralPath "$fixture/scripts/Prepare-Arm64OpenSSL.ps1" -Encoding UTF8
& {
    . "$fixture/scripts/Release.Common.ps1"
    $opensslFiles = @('include/openssl/ssl.h', 'lib/libcrypto.lib', 'lib/libssl.lib',
        'bin/libcrypto-3-arm64.dll', 'bin/libssl-3-arm64.dll')
    $dependencyCases = @(
        @{ Name='missing default'; Prepare=$true; Calls=1 },
        @{ Name='partial default'; Prepare=$true; Partial=$true; Calls=1 },
        @{ Name='existing default'; Prepare=$true; Complete=$true; Calls=0 },
        @{ Name='check only'; Prepare=$false; Calls=0; Error='ARM64 OpenSSL missing' },
        @{ Name='missing override'; Prepare=$true; Custom=$true; Calls=0; Error='Configured Arm64OpenSSLRoot is incomplete' },
        @{ Name='existing override'; Prepare=$true; Custom=$true; Complete=$true; Calls=0 },
        @{ Name='preparation failure'; Prepare=$true; Mode='failed'; Calls=1; Error='fixture dependency build failed' },
        @{ Name='incomplete preparation'; Prepare=$true; Mode='incomplete'; Calls=1; Error='ARM64 OpenSSL missing' }
    )
    foreach ($dependencyCase in $dependencyCases) {
        $script:ProjectRoot = Join-Path $OutputDirectory $dependencyCase.Name
        $config = @{}
        $expected = "$ProjectRoot/build/dependencies/openssl-arm64"
        if ($dependencyCase.Custom) {
            $expected = "$ProjectRoot/custom openssl"
            $config.Arm64OpenSSLRoot = $expected
        }
        if ($dependencyCase.Complete -or $dependencyCase.Partial) {
            $files = if ($dependencyCase.Complete) { $opensslFiles } else { @($opensslFiles[0]) }
            foreach ($file in $files) {
                $path = Join-Path $expected $file
                New-Item -ItemType Directory -Path (Split-Path $path) -Force | Out-Null
                Set-Content -LiteralPath $path -Value 'fixture'
            }
        }
        $preparationCalls = [Collections.Generic.List[string]]::new()
        $failure = ''
        $result = $null
        try { $result = Get-Arm64OpenSSLRoot $config -Prepare:$dependencyCase.Prepare 6>$null }
        catch { $failure = $_.Exception.Message }
        if ($preparationCalls.Count -ne $dependencyCase.Calls) { throw "Wrong preparation count: $($dependencyCase.Name). $failure" }
        if ($dependencyCase.Error) {
            if ($failure -notlike "$($dependencyCase.Error)*") { throw "Wrong dependency error: $($dependencyCase.Name). $failure" }
        } elseif ($failure -or $result -isnot [string] -or $result -ne $expected) {
            throw "Wrong dependency path: $($dependencyCase.Name). $failure"
        }
        Write-Host "PASS: ARM64 OpenSSL $($dependencyCase.Name)"
    }
}

# Use the real Windows archive tool, including a locked GCC file like the failed
# Strawberry extraction. None of the fixture executables contain runnable code.
$extractSource = $prepareAst.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Expand-PerlRuntime' }, $false).Extent.Text
. ([scriptblock]::Create($extractSource))
$archiveFixture = Join-Path $OutputDirectory 'perl archive with spaces'
New-Item -ItemType Directory -Path "$archiveFixture/source/perl/bin", "$archiveFixture/source/perl/lib", "$archiveFixture/source/c/bin" -Force | Out-Null
Set-Content -LiteralPath "$archiveFixture/source/perl/bin/perl.exe" -Value 'complete fixture'
Set-Content -LiteralPath "$archiveFixture/source/perl/lib/Config.pm" -Value 'fixture module'
Set-Content -LiteralPath "$archiveFixture/source/c/bin/gendef.exe" -Value 'unused compiler'
$archive = "$archiveFixture/perl.zip"
Invoke-Checked 'tar.exe' @('-a', '-cf', $archive, '-C', "$archiveFixture/source", 'perl', 'c')
$destination = "$archiveFixture/runtime"
New-Item -ItemType Directory -Path "$destination/c/bin", "$destination/perl/bin" -Force | Out-Null
Set-Content -LiteralPath "$destination/c/bin/gendef.exe" -Value 'locked file'
Set-Content -LiteralPath "$destination/perl/bin/perl.exe" -Value 'partial fixture'
$locked = [IO.File]::Open("$destination/c/bin/gendef.exe", 'Open', 'ReadWrite', 'None')
try { Expand-PerlRuntime $archive $destination }
finally { $locked.Dispose() }
if (!(Test-Path -LiteralPath "$destination/.what-perl-ready") -or
    (Get-Content -LiteralPath "$destination/perl/bin/perl.exe" -Raw).Trim() -ne 'complete fixture' -or
    !(Test-Path -LiteralPath "$destination/perl/lib/Config.pm") -or
    (Get-Content -LiteralPath "$destination/c/bin/gendef.exe" -Raw).Trim() -ne 'locked file') {
    throw 'Perl runtime recovery did not skip the locked GCC tools.'
}
Write-Host 'PASS: partial Perl extraction recovered while unused GCC file was locked.'
Set-Content -LiteralPath "$destination/perl/bin/perl.exe" -Value 'cached fixture'
Expand-PerlRuntime $archive $destination
if ((Get-Content -LiteralPath "$destination/perl/bin/perl.exe" -Raw).Trim() -ne 'cached fixture') {
    throw 'Completed Perl runtime was unnecessarily extracted again.'
}
Write-Host 'PASS: completed Perl extraction reused.'

# A stale completion marker must not survive an extraction failure.
Remove-Item -LiteralPath "$destination/perl/bin/perl.exe" -Force
Set-Content -LiteralPath "$archiveFixture/invalid.zip" -Value 'invalid archive'
$failed = $false
try { Expand-PerlRuntime "$archiveFixture/invalid.zip" $destination 6>$null }
catch { $failed = $_.Exception.Message -match 'tar.exe failed with exit code' }
if (!$failed -or (Test-Path -LiteralPath "$destination/.what-perl-ready")) {
    throw 'Failed extraction did not preserve its error or cleared completion state.'
}
Expand-PerlRuntime $archive $destination
if (!(Test-Path -LiteralPath "$destination/.what-perl-ready") -or
    !(Test-Path -LiteralPath "$destination/perl/bin/perl.exe")) { throw 'Retry did not recover Perl.' }
Write-Host 'PASS: native extraction failure retained and retry succeeded.'

Invoke-Checked 'tar.exe' @('-a', '-cf', "$archiveFixture/no-executable.zip", '-C', "$archiveFixture/source", 'perl/lib')
$failed = $false
try { Expand-PerlRuntime "$archiveFixture/no-executable.zip" "$archiveFixture/no-executable" 6>$null }
catch { $failed = $_.Exception.Message -match 'Perl extraction did not produce' }
if (!$failed -or (Test-Path -LiteralPath "$archiveFixture/no-executable/.what-perl-ready")) {
    throw 'Perl runtime without an executable was accepted.'
}
Write-Host 'PASS: extracted archive must contain the Perl runtime executable.'
