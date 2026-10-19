[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [string]$ReleaseDirectory
)
. "$PSScriptRoot/Release.Common.ps1"
. "$PSScriptRoot/Signing.Common.ps1"
$config = Read-Configuration
if ($env:GITHUB_REF_TYPE -eq 'tag' -and $env:GITHUB_REF_NAME -ne ('v' + (Get-ProjectVersion))) { throw 'GitHub tag does not match the CMake project version.' }
$suffix = if ($Architecture -eq 'x64') { '' } else { '-arm64' }
$stateFile = Join-Path $ProjectRoot "build/latest-release$suffix.json"
if ($ReleaseDirectory) { $stateFile = Join-Path $ReleaseDirectory 'release.json' }
if (!(Test-Path -LiteralPath $stateFile)) { throw 'Run Build-Release.ps1 first.' }
$state = Get-Content -LiteralPath $stateFile -Raw | ConvertFrom-Json
if ($ReleaseDirectory) { $state.runtime = Join-Path $ReleaseDirectory 'portable' }
if ($state.version -ne (Get-ProjectVersion)) { throw 'Release state is outdated. Run Build-Release.ps1 again.' }
if ($state.architecture -ne $Architecture) { throw 'Release state architecture differs from the requested installer.' }
# Cross-compiled ARM64 packages can be built before native runtime validation.
# Failed tests and untested x64 builds still block packaging.
$untestedArm64 = $Architecture -eq 'arm64' -and $state.tests -eq 'pending'
if ($state.tests -ne 'passed' -and !$untestedArm64) { throw 'Release runtime tests have not passed. Run Build-Release.ps1 again.' }
if ($untestedArm64) { Write-Warning 'Packaging ARM64 without runtime testing. Compilation and architecture checks do not verify that WHAT runs correctly on ARM64.' }
& "$PSScriptRoot/Test-RuntimeArchitecture.ps1" -Runtime $state.runtime -Architecture $Architecture
$currentSources = Get-SourceHashes
foreach ($property in $state.sources.PSObject.Properties) {
    if (!$currentSources.Contains($property.Name) -or $currentSources[$property.Name] -ne $property.Value) { throw "Source changed since Release build: $($property.Name). Run Build-Release.ps1 again." }
}
if ($currentSources.Count -ne @($state.sources.PSObject.Properties).Count) { throw 'Source files changed. Run Build-Release.ps1 again.' }
$runtime = [string]$state.runtime
$runtimeHashes = Get-Content -LiteralPath "$runtime/SHA256SUMS.json" -Raw | ConvertFrom-Json
if (@(Get-ChildItem -LiteralPath $runtime -File -Recurse | Where-Object Name -ne 'SHA256SUMS.json').Count -ne @($runtimeHashes.PSObject.Properties).Count) { throw 'Unexpected files in the release runtime. Run Build-Release.ps1 again.' }
foreach ($entry in $runtimeHashes.PSObject.Properties) {
    $path = Join-Path $runtime $entry.Name
    if (!(Test-Path -LiteralPath $path) -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.Value) { throw "Release runtime changed: $($entry.Name). Rebuild before packaging." }
}
$packageVersion = Get-PackageVersion ([string]$state.version)
$runId = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$output = Join-Path $ProjectRoot "build/installers/$($state.version)/$runId-$Architecture"
$bundle = Join-Path $output 'MSIX'
$stage = Join-Path $ProjectRoot "build/staging/$($state.version)/installer-$runId"
$logs = Join-Path $ProjectRoot "build/logs/$($state.version)/installer-$runId"
foreach ($dir in @($output,$bundle,$stage,$logs)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
Start-Transcript -Path "$logs/Build-Installers.log" | Out-Null
$oldPassword = $env:WHAT_PFX_PASSWORD
$oldOpenSSLConfig = $env:OPENSSL_CONF
try {
    $env:OPENSSL_CONF = 'NUL'
    $openssl = [string]$state.tools.OpenSSL
    $sdk = [string]$state.tools.Sdk
    $private = Join-Path $ProjectRoot '_local/signing'
    $identity = Initialize-SigningIdentity $config $openssl $private
    $pfx = $identity.Pfx; $cer = $identity.Cer; $subject = $identity.Publisher
    $tsa = if ($config.ContainsKey('TimestampUrl')) { $config.TimestampUrl } else { 'http://timestamp.digicert.com' }
    if (!$tsa -and (Get-Configured $config 'AllowUntimestamped' 'false') -ne 'true') { throw 'TimestampUrl is required unless AllowUntimestamped is explicitly true for local testing.' }
    if (!$tsa) { Write-Warning 'Explicit local-test build without a trusted timestamp. Do not use this mode for public releases.' }
    # Keep the compiled executable bytes unchanged. Only the enclosing MSIX is signed.
    Copy-Item -Path "$runtime/*" -Destination $stage -Recurse
    Copy-Item -LiteralPath "$ProjectRoot/packaging/AppxManifest.xml" -Destination $stage
    $manifestPath = Join-Path $stage 'AppxManifest.xml'
    [xml]$manifest = Get-Content -LiteralPath $manifestPath -Raw
    $manifest.Package.Identity.SetAttribute('Version',$packageVersion)
    $manifest.Package.Identity.SetAttribute('ProcessorArchitecture',$Architecture)
    if ($Architecture -eq 'arm64') {
        $manifest.Package.Dependencies.TargetDeviceFamily.SetAttribute('MinVersion','10.0.22621.0')
    }
    $manifest.Package.Identity.SetAttribute('Publisher',$subject)
    if ((Get-Configured $config 'DevelopmentBuild' 'false') -eq 'true') {
        $manifest.Package.Identity.SetAttribute('Name','WHAT.Development')
        $manifest.Package.Properties.DisplayName='WHAT Development'
    }
    $manifest.Package.Properties.PublisherDisplayName = Get-Configured $config 'PublisherDisplayName' 'WHAT'
    $manifest.Save($manifestPath)
    New-Item -ItemType Directory -Path "$stage/Assets" | Out-Null
    foreach ($logo in @('Logo150.png','Logo44.png','StoreLogo.png')) { Copy-Item -LiteralPath "$ProjectRoot/assets/$logo" -Destination "$stage/Assets" }
    Copy-Item -Path "$ProjectRoot/assets/Logo44.targetsize-*.png" -Destination "$stage/Assets"
    # Index the qualified Shell icons so Windows can select transparent light/dark variants.
    $iconStage = Join-Path (Split-Path $stage) "icons-$runId"
    New-Item -ItemType Directory -Path $iconStage | Out-Null
    Copy-Item -LiteralPath "$stage/Assets" -Destination $iconStage -Recurse
    Invoke-Checked "$sdk/makepri.exe" @('new','/pr',$iconStage,'/cf',"$ProjectRoot/packaging/IconResources.xml",'/mn',$manifestPath,'/of',"$stage/resources.pri",'/o')
    $unsigned = Join-Path (Split-Path $stage) "WHAT-unsigned-$runId.msix"
    $signed = Join-Path $bundle 'WHAT-Notifications.msix'
    Write-Host "WHAT build task: Creating $Architecture MSIX package"
    Invoke-Checked "$sdk/makeappx.exe" @('pack','/o','/d',$stage,'/p',$unsigned)
    Write-Host 'WHAT build task: Signing, timestamping and verifying the package'
    $osslsigncode = Get-Configured $config 'OsslSigncode' ''
    if ($osslsigncode) {
        $signArgs = @('sign','-pkcs12',$pfx,'-readpass','-','-h','sha256','-in',$unsigned,'-out',$signed)
        if ($tsa) { $signArgs += @('-ts',$tsa) }
        $env:WHAT_PFX_PASSWORD | & $osslsigncode @signArgs
        if ($LASTEXITCODE -ne 0) { throw 'MSIX signing/timestamping failed.' }
        $verifyPem = Join-Path $logs 'public-signing-certificate.pem'
        Invoke-Checked $openssl @('x509','-inform','DER','-in',$cer,'-out',$verifyPem)
        Invoke-Checked $osslsigncode @('verify','-CAfile',$verifyPem,'-in',$signed)
    } else {
        Copy-Item -LiteralPath $unsigned -Destination $signed
        $signArgs = @('sign','/fd','SHA256','/f',$pfx,'/p',$env:WHAT_PFX_PASSWORD)
        if ($tsa) { $signArgs += @('/tr',$tsa,'/td','SHA256') }
        & "$sdk/signtool.exe" @signArgs $signed
        if ($LASTEXITCODE -ne 0) { throw 'SignTool signing/timestamping failed.' }
        $expectedSigner = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cer)
        try {
            $signature = Get-AuthenticodeSignature -LiteralPath $signed
            if (!$signature.SignerCertificate -or $signature.SignerCertificate.Thumbprint -ne $expectedSigner.Thumbprint) {
                throw 'The signed package does not match the configured public signing certificate.'
            }
        } finally { $expectedSigner.Dispose() }
        & {
            & "$sdk/signtool.exe" verify /pa /v $signed
            if ($LASTEXITCODE -ne 0) { throw 'Windows could not verify the publisher signature. Check the SignTool output above for trust-chain, timestamp or certificate errors. CI must trust the configured self-signed certificate before this step.' }
        }
    }
    $updatePackage = Join-Path $output "WHAT-$($state.version)-Windows-$Architecture-UpdatePayload.msix"
    Copy-Item -LiteralPath $signed -Destination $updatePackage
    $updateInfo = [ordered]@{ schema=1; minimumUpdaterVersion=1; version=$state.version; architecture=$Architecture; installer=(Split-Path $updatePackage -Leaf); sha256=(Get-FileHash -LiteralPath $updatePackage -Algorithm SHA256).Hash }
    if ($state.version -like '*-beta.*') {
        $updateInfo.schema = 2
        $updateInfo.minimumUpdaterVersion = 2
        $updateInfo.packageVersion = $packageVersion
    }
    $updateManifest = if ($Architecture -eq 'x64') { 'update.json' } else { 'update-arm64.json' }
    $updateInfo | ConvertTo-Json | Set-Content -LiteralPath "$output/$updateManifest" -Encoding UTF8
    Copy-Item -LiteralPath $cer -Destination "$bundle/WHAT.cer"
    foreach ($name in @('Install-Notifications.ps1','Uninstall-Notifications.ps1','Trust-Certificate.ps1')) { Copy-Item -LiteralPath "$ProjectRoot/packaging/$name" -Destination $bundle }
    '@echo off', 'powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-Notifications.ps1"', 'set "WHAT_EXIT=%errorlevel%"', 'pause', 'exit /b %WHAT_EXIT%' | Set-Content -LiteralPath "$bundle/Install.cmd" -Encoding ASCII
    '@echo off', 'powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Uninstall-Notifications.ps1"', 'pause' | Set-Content -LiteralPath "$bundle/Uninstall.cmd" -Encoding ASCII
    @"
# WHAT $($state.version)

Use the matching WHAT-<version>-Windows-<architecture>-Setup.exe to install WHAT.
This folder is the installer payload. Advanced users can run Install.cmd from this
complete folder instead; it asks before trusting the certificate and installing.
Do not open the MSIX directly for a first installation: it skips certificate trust.
Certificate-chain errors such as 0x800B010A require the trust step. Packaging uses your explicitly
configured identity. A self-signed certificate is not publicly trusted. The
installer displays its fingerprint and requests confirmation before trusting it
and installing. Build scripts never change Windows certificate trust.

Start WHAT from the Start menu after installation. Your app
profile is retained. The portable ZIP works without installing the MSIX.
Never distribute _local/ or release.local.json.
"@ | Set-Content -LiteralPath "$bundle/README.md" -Encoding UTF8
    Write-FileHashes $bundle
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $portableZip = Join-Path $output "WHAT-$($state.version)-Windows-$Architecture-Portable.zip"
    Write-Host "WHAT build task: Compressing $Architecture portable ZIP"
    [IO.Compression.ZipFile]::CreateFromDirectory($runtime,$portableZip)
    Write-Host 'WHAT build task: Creating source archives'
    & "$PSScriptRoot/Write-SourceArchive.ps1" -Destination "$output/WHAT-$($state.version)-Source.zip"
    & "$PSScriptRoot/Prepare-CorrespondingSource.ps1" -Runtime $runtime -Destination $output
    # Compile the single-file wizard with Windows' built-in .NET Framework toolchain.
    $compiler = Find-Tool 'csc.exe' @("$env:WINDIR/Microsoft.NET/Framework64/v4.0.30319/csc.exe")
    Write-Host 'WHAT build task: Testing installer layouts'
    & "$ProjectRoot/tests/setup-layout-tests.ps1" -OutputDirectory "$stage/setup-layout-tests"
    $setupExe = Join-Path $output "WHAT-$($state.version)-Windows-$Architecture-Setup.exe"
    $setupMetadata = Join-Path $stage 'SetupAssemblyInfo.cs'
    $setupHashes = Join-Path $stage 'PayloadHashes.txt'
    @"
using System.Reflection;
[assembly: AssemblyTitle("WHAT Setup")]
[assembly: AssemblyProduct("WHAT")]
[assembly: AssemblyCompany("Chuffnugget")]
[assembly: AssemblyVersion("$packageVersion")]
[assembly: AssemblyFileVersion("$packageVersion")]
"@ | Set-Content -LiteralPath $setupMetadata -Encoding UTF8
    $payloadNames = @('WHAT-Notifications.msix','WHAT.cer','Install-Notifications.ps1','Trust-Certificate.ps1')
    $payloadNames | ForEach-Object { ((Get-FileHash -LiteralPath "$bundle/$_" -Algorithm SHA256).Hash + ' ' + $_) } | Set-Content -LiteralPath $setupHashes -Encoding ASCII
    $setupPlatform = if ($Architecture -eq 'arm64') { 'anycpu' } else { 'x64' }
    $setupManifest = Join-Path $stage 'Setup.manifest'
    [xml]$setupXml = Get-Content "$ProjectRoot/packaging/Setup.manifest" -Raw
    if ($Architecture -eq 'arm64') { $setupXml.assembly.assemblyIdentity.SetAttribute('processorArchitecture','*') }
    $setupXml.Save($setupManifest)
    $compileArgs = @('/nologo','/target:winexe',"/platform:$setupPlatform",'/optimize+','/codepage:65001',"/out:$setupExe",'/reference:System.Windows.Forms.dll','/reference:System.Drawing.dll','/reference:System.Core.dll','/reference:System.IO.Compression.dll','/reference:System.Xml.dll',"/win32manifest:$setupManifest","/win32icon:$ProjectRoot/assets/app-icon.ico","/resource:$ProjectRoot/assets/app-icon.ico,app.ico","/resource:$ProjectRoot/assets/app-icon.png,app.png","/resource:$setupHashes,PayloadHashes.txt")
    foreach ($name in $payloadNames) { $compileArgs += "/resource:$bundle/$name,$name" }
    $compileArgs += @("$ProjectRoot/packaging/Setup.cs",$setupMetadata)
    # Framework csc misparses mixed-separator source paths; normalize paths while
    # retaining the leading slash on compiler options.
    $compileArgs = @($compileArgs | ForEach-Object {
        if ($_.StartsWith('/')) { '/' + $_.Substring(1).Replace('/','\') }
        else { $_.Replace('/','\') }
    })
    Write-Host "WHAT build task: Compiling and verifying $Architecture Setup.exe"
    Invoke-Checked $compiler $compileArgs
    $verifySetup = Start-Process -FilePath $setupExe -ArgumentList @('--verify-payload', $Architecture) -WindowStyle Hidden -Wait -PassThru
    if ($verifySetup.ExitCode -ne 0) { throw 'Setup EXE embedded payload validation failed.' }
    $report = [ordered]@{ runtimeTests=$state.tests; version=$state.version; installer=$setupExe; portable=$portableZip; installerSha256=(Get-FileHash $setupExe).Hash; portableSha256=(Get-FileHash $portableZip).Hash; publisher=$subject; updatePackage=$updatePackage; updateManifest="$output/$updateManifest" }
    $report | ConvertTo-Json | Set-Content -LiteralPath "$output/artifacts.json" -Encoding UTF8
    $report | ConvertTo-Json | Set-Content -LiteralPath "$ProjectRoot/build/latest-installers$suffix.json" -Encoding UTF8
    Write-Host "Installers ready: $output" -ForegroundColor Green
    Write-Host "Share $setupExe or $portableZip"
} finally {
    $env:WHAT_PFX_PASSWORD = $oldPassword; $env:OPENSSL_CONF = $oldOpenSSLConfig
    Stop-Transcript | Out-Null
}

