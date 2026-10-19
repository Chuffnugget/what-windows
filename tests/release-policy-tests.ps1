param([string]$OutputDirectory = (Join-Path $env:TEMP ('what-release-tests-'+[guid]::NewGuid().ToString('N'))))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
& "$PSScriptRoot/build-all-tests.ps1" -OutputDirectory (Join-Path $OutputDirectory 'build-all')
& "$PSScriptRoot/build-progress-tests.ps1" -OutputDirectory (Join-Path $OutputDirectory 'build-progress')
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
foreach($file in Get-ChildItem "$root/scripts","$root/packaging" -Filter '*.ps1' -File){
    $errors=$null;$tokens=$null
    [Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors) | Out-Null
    if($errors.Count){throw ($errors | Out-String)}
}
. "$root/scripts/Release.Common.ps1"
. "$root/scripts/Signing.Common.ps1"

# CMake consumes the toolchain argument only during the initial configuration.
& {
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile("$root/scripts/Build-Release.ps1", [ref]$tokens, [ref]$errors)
    $armOptions = $ast.Find({ param($node)
        $node -is [Management.Automation.Language.IfStatementAst] -and
        $node.Extent.Text.StartsWith('if ($Architecture -eq ''arm64'')')
    }, $true).Extent.Text
    $build = Join-Path $OutputDirectory 'toolchain cache'
    New-Item -ItemType Directory -Path $build -Force | Out-Null
    $tools = @{ Qt="$build/target Qt"; HostQt="$build/host Qt" }
    $Architecture = 'arm64'
    . ([scriptblock]::Create($armOptions))
    if (@($targetOptions | Where-Object { $_ -like '-DCMAKE_TOOLCHAIN_FILE=*' }).Count -ne 1) { throw 'Initial ARM64 configure must select its toolchain.' }
    Set-Content -LiteralPath "$build/CMakeCache.txt" -Value "CMAKE_TOOLCHAIN_FILE:FILEPATH=$toolchain"
    . ([scriptblock]::Create($armOptions))
    if ($targetOptions.Count -ne 1 -or $targetOptions[0] -ne "-DQT_HOST_PATH=$($tools.HostQt)") { throw 'Incremental ARM64 configure must reuse its cached toolchain.' }
    foreach ($cache in @("CMAKE_TOOLCHAIN_FILE:FILEPATH=$build/wrong-toolchain.cmake", 'CMAKE_BUILD_TYPE:STRING=Release')) {
        Set-Content -LiteralPath "$build/CMakeCache.txt" -Value $cache
        $rejected = $false
        try { . ([scriptblock]::Create($armOptions)) }
        catch { $rejected = $_.Exception.Message -like 'Cached ARM64 toolchain differs.*' }
        if (!$rejected) { throw 'An incompatible ARM64 toolchain cache was accepted.' }
    }
}
Write-Output 'PASS: initial and incremental ARM64 toolchain selection, incompatible cache rejection.'

# Relative overrides must work when a clone or its launcher has a different path.
$originalProjectRoot = $script:ProjectRoot
$configurationFixture = Join-Path $OutputDirectory 'clone with spaces'
New-Item -ItemType Directory -Path $configurationFixture -Force | Out-Null
Push-Location $OutputDirectory
try {
    $script:ProjectRoot = $configurationFixture
    if ((Read-Configuration).Count -ne 0) { throw 'A fresh clone should use tool detection.' }
    $fixtureConfig = @{ Publisher = 'CN=Fixture'; PublisherDisplayName = 'WHAT' }
    foreach ($key in @('Qmake', 'CMake', 'Ninja', 'OpenSSLRoot', 'WindowsSdkBin',
        'Compiler', 'SigningPfx', 'SigningCertificate', 'SigningPasswordFile')) {
        $fixtureConfig[$key] = "local tools/$key"
    }
    $fixtureConfig | ConvertTo-Json | Set-Content "$configurationFixture/release.local.json"
    $resolved = Read-Configuration
    foreach ($key in $fixtureConfig.Keys) {
        $expected = if ($key -in @('Publisher', 'PublisherDisplayName')) {
            $fixtureConfig[$key]
        } else {
            [IO.Path]::GetFullPath((Join-Path $configurationFixture $fixtureConfig[$key]))
        }
        if ($resolved[$key] -ne $expected) { throw "Configuration path resolution failed for $key." }
    }
    $fixtureConfig.Qmake = Join-Path $OutputDirectory 'external Qt/qmake.exe'
    $fixtureConfig.CMake = ''
    $fixtureConfig | ConvertTo-Json | Set-Content "$configurationFixture/release.local.json"
    $resolved = Read-Configuration
    if ($resolved.Qmake -ne $fixtureConfig.Qmake -or $resolved.CMake -ne '') {
        throw 'Absolute and empty tool overrides must remain unchanged.'
    }
} finally {
    Pop-Location
    $script:ProjectRoot = $originalProjectRoot
}
Write-Output 'PASS: project-relative configuration, absolute overrides and default tool detection.'

# A clean CI checkout imports the bundled integration during runtime tests.
# Those imports must not change the release fingerprint or enter Source.zip.
$sourceFixture = Join-Path $OutputDirectory 'source-fixture'
foreach ($name in @('src','qml','assets','tests','tools','scripts','packaging','licenses','documentation','.github')) {
    New-Item -ItemType Directory -Path "$sourceFixture/$name" -Force | Out-Null
}
Set-Content "$sourceFixture/CMakeLists.txt" 'project(WHAT VERSION 0.1.0)'
Set-Content "$sourceFixture/src/app.cpp" 'int main() { return 0; }'
Set-Content "$sourceFixture/tests/contract.py" 'VALUE = 1'
Set-Content "$sourceFixture/documentation/GUIDE.md" '# Release guide'
Copy-Item -LiteralPath "$root/scripts/Release.Common.ps1","$root/scripts/Write-SourceArchive.ps1" -Destination "$sourceFixture/scripts"
try {
    $script:ProjectRoot = $sourceFixture
    $before = Get-SourceHashes
    if (!$before.Contains('src/app.cpp')) { throw 'Source paths must be relative to the fixture root.' }
    if (!$before.Contains('documentation/GUIDE.md')) { throw 'Documentation is missing from the release fingerprint.' }
    New-Item -ItemType Directory -Path "$sourceFixture/tests/__pycache__" | Out-Null
    foreach ($name in @('tests/__pycache__/contract.cpython-312.pyc', 'tests/__pycache__/cache-metadata',
        'tests/contract.pyc', 'tests/contract.pyo')) {
        Set-Content -LiteralPath "$sourceFixture/$name" 'generated Python cache'
    }
    $after = Get-SourceHashes
    if ($after.Count -ne $before.Count) { throw 'Python caches changed the source file set.' }
    foreach ($name in $before.Keys) {
        if ($after[$name] -ne $before[$name]) { throw "Python caches changed the source hash: $name" }
    }

    $archivePath = Join-Path $sourceFixture 'Source.zip'
    & "$sourceFixture/scripts/Write-SourceArchive.ps1" -Destination $archivePath
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $names = @($archive.Entries | ForEach-Object FullName)
        if ($names -notcontains 'tests/contract.py' -or $names -notcontains 'src/app.cpp') {
            throw 'Source archive omitted actual source files.'
        }
        if ($names -notcontains 'documentation/GUIDE.md') {
            throw 'Source archive omitted the documentation directory.'
        }
        if ($names | Where-Object { $_ -match '__pycache__|\.(pyc|pyo)$' }) {
            throw 'Source archive contains Python caches.'
        }
    } finally { $archive.Dispose() }

    Add-Content "$sourceFixture/src/app.cpp" '// changed'
    $editedHash = (Get-FileHash -LiteralPath "$sourceFixture/src/app.cpp" -Algorithm SHA256).Hash
    $sourceRoots = @($sourceFixture, "$sourceFixture\")
    # GitHub's TEMP can use an 8.3 alias; Resolve-Path leaves that alias intact.
    $shortRoot = (New-Object -ComObject Scripting.FileSystemObject).GetFolder($sourceFixture).ShortPath
    if ($shortRoot -ne $sourceFixture) { $sourceRoots += $shortRoot }
    foreach ($sourceRoot in $sourceRoots) {
        $script:ProjectRoot = $sourceRoot
        $edited = Get-SourceHashes
        if (!$edited.Contains('src/app.cpp') -or $edited['src/app.cpp'] -ne $editedHash -or
            $edited['src/app.cpp'] -eq $before['src/app.cpp']) {
            throw "An edited source file was ignored or misnamed under root: $sourceRoot"
        }
    }
    $script:ProjectRoot = $sourceFixture
    Set-Content "$sourceFixture/src/added.cpp" '// new source'
    if (!(Get-SourceHashes).Contains('src/added.cpp')) { throw 'An added source file was ignored.' }
    Remove-Item -LiteralPath "$sourceFixture/tests/contract.py"
    if ((Get-SourceHashes).Contains('tests/contract.py')) { throw 'A deleted source file remains in the fingerprint.' }
} finally { $script:ProjectRoot = $originalProjectRoot }
Write-Output 'PASS: Python caches excluded from release hashes and source archive; real source changes retained.'

# Documentation moves must not remove third-party notices from portable/MSIX builds.
& {
    $ProjectRoot = $root
    $runtime = Join-Path $OutputDirectory 'licence staging'
    New-Item -ItemType Directory -Path $runtime -Force | Out-Null
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile("$root/scripts/Build-Release.ps1", [ref]$tokens, [ref]$errors)
    $copySteps = @($ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.PipelineAst] -and
        ($node.Extent.Text.StartsWith('Copy-Item -LiteralPath "$ProjectRoot/licenses"') -or
         $node.Extent.Text.StartsWith('Get-ChildItem -LiteralPath "$ProjectRoot/documentation/licenses"'))
    }, $true))
    if ($copySteps.Count -ne 2) { throw 'Expected both source licence directories in runtime staging.' }
    foreach ($step in $copySteps) { & ([scriptblock]::Create($step.Extent.Text)) }
    foreach ($sourceRoot in @("$root/licenses", "$root/documentation/licenses")) {
        foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -Recurse -File) {
            $relative = $file.FullName.Substring($sourceRoot.Length + 1)
            $deployed = Join-Path "$runtime/licenses" $relative
            if (!(Test-Path -LiteralPath $deployed) -or
                (Get-FileHash -LiteralPath $deployed).Hash -ne (Get-FileHash -LiteralPath $file.FullName).Hash) {
                throw "Runtime licence notice missing or changed: $relative"
            }
        }
    }
}
Write-Output 'PASS: all licence notices retained when staging from documentation and licenses.'

function Invoke-Checked { throw 'A missing identity must fail before invoking a signing tool.' }
foreach($config in @(@{},@{Publisher='CN=Fixture';SigningPfx="$OutputDirectory/missing.pfx";SigningCertificate="$OutputDirectory/missing.cer"})){
    $rejected=$false
    try { Initialize-SigningIdentity $config 'not-a-tool' "$OutputDirectory/private" | Out-Null }
    catch { if($_.Exception.Message -notmatch 'Configure Publisher|Signing identity missing'){throw};$rejected=$true }
    if(!$rejected){throw 'Missing signing identity was accepted.'}
}
# Exercise the real installation script with mocked Windows installation/trust
# boundaries. No package is installed and no certificate store is changed.
$rsa=[Security.Cryptography.RSA]::Create(2048)
$request=[Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=WHAT',$rsa,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)
$global:whatAuditFixtureCertificate=$request.CreateSelfSigned([DateTimeOffset]::UtcNow.AddDays(-1),[DateTimeOffset]::UtcNow.AddDays(1))
[IO.File]::WriteAllBytes("$OutputDirectory/WHAT.cer",$global:whatAuditFixtureCertificate.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
Copy-Item -LiteralPath "$root/packaging/Install-Notifications.ps1" -Destination $OutputDirectory
Add-Type -AssemblyName System.IO.Compression,System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::Open("$OutputDirectory/WHAT-Notifications.msix",[IO.Compression.ZipArchiveMode]::Create)
try {
    $writer=[IO.StreamWriter]::new($zip.CreateEntry('AppxManifest.xml').Open())
    try {$writer.Write('<Package><Identity Name="WHAT.Desktop" Publisher="CN=WHAT" Version="1.2.3.0" ProcessorArchitecture="x64" /></Package>')}finally{$writer.Dispose()}
} finally {$zip.Dispose()}
function Get-AuthenticodeSignature { [pscustomobject]@{SignerCertificate=$global:whatAuditFixtureCertificate;Status=$global:whatAuditSignatureStatus} }
$global:whatAuditSignatureStatus='Valid'
function Test-Path { param([string]$LiteralPath); if($LiteralPath -like 'Cert:*'){return $true};Microsoft.PowerShell.Management\Test-Path -LiteralPath $LiteralPath }
function Add-AppxPackage { param($Path,$ErrorAction,[switch]$ForceUpdateFromAnyVersion);$global:whatAuditDowngrade=[bool]$ForceUpdateFromAnyVersion;$global:whatAuditInstallationCount++;$global:whatAuditPackages[0].Version=[version]'1.2.3.0' }
$global:whatAuditInstallationCount=0
$global:whatAuditPackages=@(
    [pscustomobject]@{Publisher='CN=WHAT';Version=[version]'1.2.3.0';Architecture='X64';PackageFamilyName='WHAT.Desktop_expected';InstallLocation='C:\Program Files\WindowsApps\WHAT.Desktop_fixture'}
)
function Get-AppxPackage { param($Name);$global:whatAuditPackages }
& "$OutputDirectory/Install-Notifications.ps1" -FromSetup -ExpectedInstalledVersion '1.2.3.0' -FamilyOutputPath "$OutputDirectory/family.txt"
if([IO.File]::ReadAllText("$OutputDirectory/family.txt") -ne 'WHAT.Desktop_expected' -or $global:whatAuditInstallationCount -ne 1){throw 'Exact publisher selection failed.'}
if ([IO.File]::ReadAllText("$OutputDirectory/family.txt.location") -ne 'C:\Program Files\WindowsApps\WHAT.Desktop_fixture') { throw 'Installed location was not returned to Setup.' }
$global:whatAuditPackages=@([pscustomobject]@{Publisher='CN=Other';Version=[version]'1.2.3.0';Architecture='X64';PackageFamilyName='WHAT.Desktop_wrong'})
$rejected=$false
try { & "$OutputDirectory/Install-Notifications.ps1" -FromSetup -ExpectedInstalledVersion '1.2.3.0' -FamilyOutputPath "$OutputDirectory/family.txt" } catch {if($_.Exception.Message -notmatch 'different WHAT publisher'){throw};$rejected=$true}
if(!$rejected){throw 'An unrelated publisher was accepted.'}
$global:whatAuditPackages=@([pscustomobject]@{Publisher='CN=WHAT';Version=[version]'1.2.3.0';Architecture='X64';PackageFamilyName='WHAT.Desktop_expected';InstallLocation='C:\Program Files\WindowsApps\WHAT.Desktop_fixture'})
# Inspect installed versions without invoking trust or installation.
foreach ($case in @(@{Version='1.2.2.0';Mode='update'},@{Version='1.2.3.0';Mode='reinstall'},@{Version='1.2.4.0';Mode='downgrade'})) {
    $global:whatAuditPackages[0].Version=[version]$case.Version
    $before=$global:whatAuditInstallationCount
    & "$OutputDirectory/Install-Notifications.ps1" -InspectOnly -InspectionOutputPath "$OutputDirectory/inspection.txt"
    if ((Get-Content "$OutputDirectory/inspection.txt")[0] -ne $case.Mode -or $global:whatAuditInstallationCount -ne $before) { throw 'Incorrect installation inspection result.' }
    if ([IO.File]::ReadAllText("$OutputDirectory/inspection.txt.location") -ne 'C:\Program Files\WindowsApps\WHAT.Desktop_fixture') { throw 'Existing installation location was not returned.' }
}
$global:whatAuditPackages[0].Version=[version]'1.2.4.0'
$before=$global:whatAuditInstallationCount
$rejected=$false
try { & "$OutputDirectory/Install-Notifications.ps1" -FromSetup -ExpectedInstalledVersion '1.2.4.0' }
catch { if ($_.Exception.Message -notmatch 'explicit confirmation') { throw }; $rejected=$true }
if (!$rejected -or $global:whatAuditInstallationCount -ne $before) { throw 'Unconfirmed downgrade was accepted.' }
& "$OutputDirectory/Install-Notifications.ps1" -FromSetup -ExpectedInstalledVersion '1.2.4.0' -AllowDowngrade
if (!$global:whatAuditDowngrade -or $global:whatAuditInstallationCount -ne $before+1) { throw 'Confirmed downgrade did not use the Windows downgrade flag.' }
# Supplying consent for an ordinary reinstall must not force a downgrade.
& "$OutputDirectory/Install-Notifications.ps1" -FromSetup -ExpectedInstalledVersion '1.2.3.0' -AllowDowngrade
if ($global:whatAuditDowngrade) { throw 'Downgrade flag used for a reinstall.' }
Write-Host 'PASS: downgrade confirmation and conditional Windows downgrade flag.'
$global:whatAuditPackages[0].Version=[version]'1.2.3.0'
$rejected=$false
try { & "$OutputDirectory/Install-Notifications.ps1" -FromSetup -ExpectedInstalledVersion '1.2.2.0' }
catch { if ($_.Exception.Message -notmatch 'changed since confirmation') { throw }; $rejected=$true }
if (!$rejected) { throw 'Stale installation confirmation was accepted.' }
# Matching signer alone must not allow tampered or still-untrusted payloads.
foreach ($status in @('HashMismatch','NotSigned','UnknownError','NotTrusted')) {
    $global:whatAuditSignatureStatus=$status
    $before=$global:whatAuditInstallationCount
    $rejected=$false
    try { & "$OutputDirectory/Install-Notifications.ps1" -FromSetup -ExpectedInstalledVersion '1.2.3.0' }
    catch { if ($_.Exception.Message -notmatch 'signature') { throw }; $rejected=$true }
    if (!$rejected -or $global:whatAuditInstallationCount -ne $before) { throw "Invalid package signature accepted: $status" }
}
$global:whatAuditSignatureStatus='Valid'
# Prevent accidental reintroduction of EXE signing while retaining package signing.
$packaging = Get-Content "$root/scripts/Build-Installers.ps1" -Raw
if ($packaging -match 'Sign-ApplicationExecutables|setupSignArgs|signtool.exe.*setupExe') { throw 'EXE signing must remain disabled.' }
if ($packaging -notmatch 'MSIX signing/timestamping failed' -or $packaging -notmatch 'SignTool signing/timestamping failed') { throw 'MSIX signing is required.' }
Write-Host 'PASS: script syntax, missing identity rejection, exact publisher/version selection, MSIX-only signing policy. No installation or trust changes performed.'

$originalProjectRoot = $script:ProjectRoot
$versionFixture = Join-Path $OutputDirectory 'version-fixture'
New-Item -ItemType Directory -Path $versionFixture -Force | Out-Null
try {
    $script:ProjectRoot = $versionFixture
    foreach ($case in @(@{Suffix=''; Release='0.7.5'; Package='0.7.5.0'}, @{Suffix='beta.12'; Release='0.7.5-beta.12'; Package='0.7.5.12'})) {
        Set-Content -LiteralPath "$versionFixture/CMakeLists.txt" -Value ('project(WHAT VERSION 0.7.5 LANGUAGES C CXX RC)' + "`n" + 'set(WHAT_PRERELEASE "' + $case.Suffix + '")')
        if ((Get-ProjectVersion) -ne $case.Release -or (Get-PackageVersion $case.Release) -ne $case.Package) { throw 'Release/package version mapping failed.' }
    }
    $formattedCMake = @('project(', '    WHAT', '    VERSION 0.7.5', '    LANGUAGES C CXX RC', ')', 'set(WHAT_PRERELEASE "beta.12" )')
    Set-Content -LiteralPath "$versionFixture/CMakeLists.txt" -Value $formattedCMake
    if ((Get-ProjectVersion) -ne '0.7.5-beta.12') { throw 'Multiline CMake version parsing failed.' }
    Set-Content -LiteralPath "$versionFixture/CMakeLists.txt" -Value @('project(WHAT VERSION 0.7.5 LANGUAGES C CXX RC)', '#set(WHAT_PRERELEASE "")', '  # set(WHAT_PRERELEASE "beta.2")', 'set(WHAT_PRERELEASE "beta.12") # release channel')
    if ((Get-ProjectVersion) -ne '0.7.5-beta.12') { throw 'Commented prerelease setting was treated as active.' }
    Add-Content -LiteralPath "$versionFixture/CMakeLists.txt" -Value 'set(WHAT_PRERELEASE "")'
    $rejected = $false
    try { Get-ProjectVersion | Out-Null } catch { $rejected = $true }
    if (!$rejected) { throw 'Ambiguous version declarations were accepted.' }
    foreach ($suffix in @('beta.0','beta.65536','7.5b')) {
        Set-Content -LiteralPath "$versionFixture/CMakeLists.txt" -Value ('project(WHAT VERSION 0.7.5 LANGUAGES C CXX RC)' + "`n" + 'set(WHAT_PRERELEASE "' + $suffix + '")')
        $rejected = $false
        try { Get-ProjectVersion | Out-Null } catch { $rejected = $true }
        if (!$rejected) { throw "Invalid prerelease suffix accepted: $suffix" }
    }
} finally { $script:ProjectRoot = $originalProjectRoot }
Write-Output 'PASS: stable/beta release names, numeric package mapping and invalid beta rejection.'

# A test failure must remain fatal and its assertion must appear in the Actions console.
$fixtureLog = Join-Path $OutputDirectory 'qt-failure.txt'
Set-Content -LiteralPath $fixtureLog -Value @('FAIL! : Fixture::permissionTest() assertion failed', 'tests/controller_tests.cpp(42) : failure location', 'Totals: 1 passed, 1 failed')
$captured = (& {
    function Invoke-Checked([string]$Executable, [string[]]$Arguments) { throw 'fixture test failure' }
    try { Invoke-QtTests 'fixture.exe' $fixtureLog; throw 'failure was swallowed' }
    catch {
        if ($_.Exception.Message -ne 'fixture test failure') { throw }
        Write-Output 'original exception retained'
    }
} 6>&1 | Out-String)
if ($captured -notmatch 'FAIL!.*permissionTest' -or $captured -notmatch 'failure location' -or $captured -notmatch 'original exception retained') { throw 'Qt failure summary was not surfaced correctly.' }
Write-Output 'PASS: Qt assertions reach the console and test failure still stops packaging.'

$previousConsole = $env:QT_ASSUME_STDERR_HAS_CONSOLE
try {
    $env:QT_ASSUME_STDERR_HAS_CONSOLE = '0'
    $captured = (& {
        function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
            if ($env:QT_ASSUME_STDERR_HAS_CONSOLE -ne '1' -or
                ($Arguments -join '|') -ne "-v1|-maxwarnings|0|-o|$fixtureLog,txt|-o|-,txt") {
                throw 'Qt tests must retain their file log and enable live console output.'
            }
            Write-Output 'INFO   : Fixture::permissionTest() entering'
            Write-Output 'QINFO  : Fixture::permissionTest() detailed module log'
            Write-Output 'PASS   : Fixture::permissionTest()'
            Write-Output 'Totals: 1 passed, 0 failed'
        }
        Invoke-QtTests 'fixture.exe' $fixtureLog
    } 6>&1 | Out-String)
    if ($captured -notmatch 'permissionTest\(\) entering' -or $captured -notmatch 'PASS.*permissionTest' -or
        $captured -notmatch 'Totals:' -or $captured -match 'detailed module log' -or
        $env:QT_ASSUME_STDERR_HAS_CONSOLE -ne '0') {
        throw 'Qt live progress filtering or console environment restoration failed.'
    }
    & {
        function Invoke-Checked { throw 'fixture test failure' }
        try { Invoke-QtTests 'fixture.exe' $fixtureLog 6>$null; throw 'failure was swallowed' }
        catch { if ($_.Exception.Message -ne 'fixture test failure') { throw } }
    }
    if ($env:QT_ASSUME_STDERR_HAS_CONSOLE -ne '0') { throw 'Failed Qt test did not restore the console environment.' }
} finally {
    $env:QT_ASSUME_STDERR_HAS_CONSOLE = $previousConsole
}
Write-Output 'PASS: live Qt test progress, full file logging and console environment restoration.'

