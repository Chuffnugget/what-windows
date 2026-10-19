[CmdletBinding()]
param(
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [string]$ResultPath
)
. "$PSScriptRoot/Release.Common.ps1"
$config = Read-Configuration
$tools = Get-ReleaseTools $config $Architecture
$version = Get-ProjectVersion
$runId = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$release = Join-Path $ProjectRoot "build/staging/$version/$runId"
$runtime = Join-Path $release 'portable'
$logs = Join-Path $ProjectRoot "build/logs/$version/$runId"
$kitName = "qt-6.11.2-msvc-$Architecture-" + $tools.CompilerVersion
$build = Join-Path $ProjectRoot "build/release/$kitName"
foreach ($folder in @($runtime,$logs,$build)) { New-Item -ItemType Directory -Path $folder -Force | Out-Null }
$originalEnvironment = @{}
Get-ChildItem Env: | ForEach-Object { $originalEnvironment[$_.Name] = $_.Value }
Start-Transcript -Path (Join-Path $logs 'Build-Release.log') | Out-Null
try {
    $buildEnvironment = Get-MsvcEnvironment $tools.VcVars $Architecture
    foreach ($key in $buildEnvironment.Keys) { [Environment]::SetEnvironmentVariable($key, $buildEnvironment[$key], 'Process') }
    $env:PATH = "$($tools.HostQt)/bin;$(Split-Path $tools.OpenSSL);" + $env:PATH
    $env:QT_PLUGIN_PATH = "$($tools.Qt)/plugins"
    Write-Host "Building WHAT $version (Release $Architecture)" -ForegroundColor Cyan
    Write-Host "Qt: $($tools.Qt) | Compiler: $($tools.Compiler)"
    Write-Host 'WHAT build task: Configuring CMake'
    $targetOptions = @()
    if ($Architecture -eq 'arm64') {
        $toolchain = [IO.Path]::GetFullPath("$($tools.Qt)/lib/cmake/Qt6/qt.toolchain.cmake")
        $targetOptions = @("-DQT_HOST_PATH=$($tools.HostQt)")
        # CMake loads the cached toolchain on later runs. Passing it again is
        # ignored and produces an unused-variable warning on incremental builds.
        if (Test-Path -LiteralPath "$build/CMakeCache.txt") {
            $cached = @(Select-String -LiteralPath "$build/CMakeCache.txt" -Pattern '^CMAKE_TOOLCHAIN_FILE:[^=]+=(.+)$')
            if ($cached.Count -ne 1 -or [IO.Path]::GetFullPath($cached[0].Matches[0].Groups[1].Value) -ne $toolchain) {
                throw "Cached ARM64 toolchain differs. Clear $build before switching Qt toolchains."
            }
        } else {
            $targetOptions += "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
        }
    }
    Invoke-Checked $tools.CMake (@('-S',$ProjectRoot,'-B',$build,'-G','Ninja',"-DCMAKE_MAKE_PROGRAM=$($tools.Ninja.Replace('\','/'))","-DCMAKE_CXX_COMPILER=$($tools.Compiler.Replace('\','/'))","-DCMAKE_C_COMPILER=$($tools.Compiler.Replace('\','/'))","-DCMAKE_PREFIX_PATH=$($tools.Qt.Replace('\','/'))","-DOPENSSL_ROOT_DIR=$($tools.OpenSSLRoot.Replace('\','/'))",'-DCMAKE_BUILD_TYPE=Release','-DBUILD_TESTING=ON','-DQT_ENABLE_QML_DEBUG=OFF') + $targetOptions)
    if ((Get-Content -LiteralPath "$build/what-version.txt" -Raw).Trim() -ne $version) { throw 'CMake and release script versions disagree.' }
    # Recreate all icon sizes from the current artwork before the executable build.
    Write-Host 'WHAT build task: Preparing application icons'
    if ($Architecture -eq 'x64') {
        Invoke-Checked $tools.CMake @('--build',$build,'--target','icon_builder','--parallel','4')
        Invoke-Checked (Join-Path $build 'icon_builder.exe') @((Join-Path $ProjectRoot 'assets/app-icon.png'),(Join-Path $ProjectRoot 'assets'))
    }
    Write-Host "WHAT build task: Compiling $Architecture"
    Invoke-Checked $tools.CMake @('--build',$build,'--parallel','4')
    Copy-Item -LiteralPath (Join-Path $build 'WHAT.exe') -Destination $runtime
    if ($Architecture -eq 'x64') { Copy-Item -LiteralPath (Join-Path $build 'beacn-volume-hook.dll') -Destination $runtime }
    New-Item -ItemType Directory -Path "$runtime/helpers" -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $build 'WHAT.WindowsHelper.exe') -Destination "$runtime/helpers"
    if ($Architecture -eq 'x64') {
        & "$PSScriptRoot/Prepare-iCueSdk.ps1" -Destination "$runtime/helpers"
        Invoke-Checked $tools.CMake @('--build',$build,'--target','icue_runtime_probe','--parallel','4')
        Invoke-Checked (Join-Path $build 'icue_runtime_probe.exe') @("$runtime/helpers")
    }
    Write-Host "WHAT build task: Deploying $Architecture runtime and Qt libraries"
    $deployOptions = @()
    if ($Architecture -eq 'arm64') { $deployOptions = @('--qtpaths', "$($tools.Qt)/bin/host-qtpaths.bat", '--no-system-dxc-compiler') }
    Invoke-Checked "$($tools.HostQt)/bin/windeployqt.exe" (@('--release','--no-compiler-runtime','--no-translations','--no-opengl-sw','--no-system-d3d-compiler','--skip-plugin-types','generic,platforminputcontexts,qmltooling,vectorimageformats','--force-openssl','--qmldir',"$ProjectRoot/qml","$runtime/WHAT.exe") + $deployOptions)
    # App-local release CRT works in portable and MSIX without a redist installer.
    Get-ChildItem -LiteralPath $tools.Crt -Filter '*.dll' -File | ForEach-Object {
        # The ARM redist also carries an x64-only exception helper for emulated
        # code. Native ARM64 binaries use vcruntime140.dll instead.
        if ($Architecture -ne 'arm64' -or $_.Name -ne 'vcruntime140_1.dll') {
            Copy-Item -LiteralPath $_.FullName -Destination $runtime -Force
        }
    }
    foreach ($dll in @("libcrypto-3-$Architecture.dll","libssl-3-$Architecture.dll")) { Copy-Item -LiteralPath "$($tools.OpenSSLRoot)/bin/$dll" -Destination $runtime -Force }
    foreach ($name in @('LICENSE','COPYING.GPL-3.0','COPYRIGHT')) { Copy-Item -LiteralPath "$ProjectRoot/$name" -Destination $runtime }
    Copy-Item -LiteralPath "$ProjectRoot/licenses" -Destination $runtime -Recurse
    if ($Architecture -eq 'x64') { Copy-Item -LiteralPath "$ProjectRoot/build/dependencies/icue-4.0.84/iCUESDK/doc/iCUE SDK - Overview and Reference.pdf" -Destination "$runtime/licenses/Corsair-SDK-Overview-and-EULA.pdf" }
    Copy-Item -LiteralPath "$ProjectRoot/docs/REBUILDING.md" -Destination "$runtime/licenses/REBUILDING.md"
    Copy-Item -LiteralPath "$build/_deps/webview2-src/LICENSE.txt" -Destination "$runtime/licenses/WebView2-SDK-LICENSE.txt"
    Copy-Item -LiteralPath "$build/_deps/webview2-src/NOTICE.txt" -Destination "$runtime/licenses/WebView2-SDK-NOTICE.txt"
    & "$PSScriptRoot/Write-DependencyInventory.ps1" -Runtime $runtime -Tools $tools
    # Run tests beside exactly the deployed libraries, without leaving test EXEs in portable.
    $tests = Join-Path $release 'test-runtime'
    Copy-Item -LiteralPath $runtime -Destination $tests -Recurse
    foreach ($name in @('controller_tests.exe','reader_tests.exe','update_tests.exe','settings_tests.exe','diagnostic_crash_fixture.exe','icue_sdk_fixture.dll')) { Copy-Item -LiteralPath "$build/$name" -Destination $tests }
    Copy-Item -LiteralPath "$ProjectRoot/tests/fixtures" -Destination "$tests/fixtures" -Recurse
    Copy-Item -LiteralPath "$($tools.Qt)/bin/Qt6Test.dll" -Destination $tests
    Write-Host 'WHAT build task: Verifying runtime architecture and file hashes'
    & "$PSScriptRoot/Test-RuntimeArchitecture.ps1" -Runtime $runtime -Architecture $Architecture
    Write-FileHashes $runtime
    Write-FileHashes $tests
    $state = [ordered]@{ version=$version; architecture=$Architecture; release=$release; runtime=$runtime; tools=$tools; sources=(Get-SourceHashes); builtAt=(Get-Date).ToString('o'); tests='pending' }
    $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$release/release.json" -Encoding UTF8
    $nativeArchitecture = if ($env:PROCESSOR_ARCHITEW6432) { $env:PROCESSOR_ARCHITEW6432 } else { $env:PROCESSOR_ARCHITECTURE }
    if ($Architecture -eq 'arm64' -and $nativeArchitecture -ne 'ARM64') {
        $state | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$ProjectRoot/build/latest-release-arm64.json" -Encoding UTF8
        Write-Warning "ARM64 build staged, not runtime-tested: $release. Installer packaging is allowed; runtime validation remains pending."
    } else {
        Write-Host "WHAT build task: Validating $Architecture runtime"
        & "$PSScriptRoot/Test-ReleaseRuntime.ps1" -ReleaseDirectory $release
        Write-Host "Release ready: $runtime" -ForegroundColor Green
        Write-Host "Next: run Build-Installers.ps1 -Architecture $Architecture"
    }
    if ($ResultPath) {
        @{ release=$release } | ConvertTo-Json | Set-Content -LiteralPath $ResultPath -Encoding UTF8
    }

} finally {
    foreach ($entry in @(Get-ChildItem Env:)) {
        if (!$originalEnvironment.ContainsKey($entry.Name)) { [Environment]::SetEnvironmentVariable($entry.Name, $null, 'Process') }
    }
    foreach ($key in $originalEnvironment.Keys) { [Environment]::SetEnvironmentVariable($key, $originalEnvironment[$key], 'Process') }
    Stop-Transcript | Out-Null
}

