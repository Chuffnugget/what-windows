Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:ProjectRoot = Split-Path $PSScriptRoot -Parent
function Find-Tool([string]$Name, [string[]]$Candidates) {
    foreach ($candidate in $Candidates) { if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) { return (Resolve-Path -LiteralPath $candidate).Path } }
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    throw "Cannot find $Name. See docs/RELEASE.md for prerequisites or set its path in release.local.json."
}
function Read-Configuration {
    $config = @{}
    $file = Join-Path $script:ProjectRoot 'release.local.json'
    if (Test-Path -LiteralPath $file) {
        $json = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
        foreach ($property in $json.PSObject.Properties) { $config[$property.Name] = [string]$property.Value }
    }
    # Launchers can run from any working directory. Resolve local overrides from
    # the project that owns the configuration, before invoking external tools.
    $pathKeys = @('Qmake', 'CMake', 'Ninja', 'OpenSSLRoot', 'WindowsSdkBin',
        'Arm64Qt', 'Arm64OpenSSLRoot', 'Compiler', 'SigningPfx', 'SigningCertificate', 'SigningPasswordFile')
    foreach ($key in $pathKeys) {
        if ($config.ContainsKey($key) -and $config[$key] -and ![IO.Path]::IsPathRooted($config[$key])) {
            $config[$key] = [IO.Path]::GetFullPath((Join-Path $script:ProjectRoot $config[$key]))
        }
    }
    return $config
}
function Get-Configured($Config, [string]$Name, [string]$Default) {
    if ($Config.ContainsKey($Name) -and $Config[$Name]) { return $Config[$Name] }
    return $Default
}
function Resolve-ConfiguredTool($Config, [string]$Key, [string]$Name, [string[]]$Candidates) {
    if ($Config.ContainsKey($Key) -and $Config[$Key]) {
        if (!(Test-Path -LiteralPath $Config[$Key])) { throw "Configured $Key does not exist: $($Config[$Key])" }
        return $Config[$Key]
    }
    return Find-Tool $Name $Candidates
}
function Get-MsvcEnvironment([string]$VcVars, [string]$Architecture = 'x64') {
    $arguments = if ($Architecture -eq 'arm64') { 'amd64_arm64 -vcvars_ver=14.44' } else { '' }
    $lines = & $env:ComSpec /d /c "call `"$VcVars`" $arguments >nul && set"
    if ($LASTEXITCODE -ne 0) { throw "Could not initialize the Visual Studio $Architecture build environment." }
    $result = @{}
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') { $result[$Matches[1]] = $Matches[2] }
    }
    if (!$result.ContainsKey('VCToolsInstallDir') -or $result['VSCMD_ARG_TGT_ARCH'] -ne $Architecture) {
        throw "Visual Studio did not provide a $Architecture C++ toolchain."
    }
    return $result
}
function Get-Arm64OpenSSLRoot($Config, [switch]$Prepare) {
    $root = Get-Configured $Config 'Arm64OpenSSLRoot' "$script:ProjectRoot/build/dependencies/openssl-arm64"
    $requiredFiles = @('include/openssl/ssl.h', 'lib/libcrypto.lib', 'lib/libssl.lib',
        'bin/libcrypto-3-arm64.dll', 'bin/libssl-3-arm64.dll')
    $missing = @($requiredFiles | Where-Object { !(Test-Path -LiteralPath "$root/$_" -PathType Leaf) })
    $customRoot = $Config.ContainsKey('Arm64OpenSSLRoot') -and $Config.Arm64OpenSSLRoot
    # Only prepare the checkout's own dependency. Never overwrite a supplied SDK.
    if ($missing.Count -and $Prepare -and !$customRoot) {
        Write-Host "Preparing missing ARM64 OpenSSL dependency: $root" -ForegroundColor Cyan
        Write-Host 'The first run downloads verified build dependencies and compiles OpenSSL. Later builds reuse it.'
        & "$PSScriptRoot/Prepare-Arm64OpenSSL.ps1" | Out-Host
    }
    foreach ($file in $requiredFiles) {
        if (!(Test-Path -LiteralPath "$root/$file" -PathType Leaf)) {
            if ($customRoot) { throw "Configured Arm64OpenSSLRoot is incomplete: $root/$file. Correct the path or remove the override to use the project-local dependency." }
            throw "ARM64 OpenSSL missing: $root/$file. Run Build-All without -ValidateOnly or run scripts/Prepare-Arm64OpenSSL.ps1."
        }
    }
    return $root
}
function Get-ReleaseTools($Config, [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64', [switch]$PrepareDependencies) {
    if ($Architecture -eq 'arm64') {
        # Host tools (signing, deployment, icon generation) still run on x64.
        $tools = Get-ReleaseTools $Config 'x64'
        $tools.HostQt = $tools.Qt
        $tools.Qt = Get-Configured $Config 'Arm64Qt' (Join-Path (Split-Path $tools.Qt) 'msvc2022_arm64')
        $tools.Qmake = Join-Path $tools.Qt 'bin/host-qmake.bat'
        $qconfig = Get-Content -LiteralPath "$($tools.Qt)/mkspecs/qconfig.pri" -Raw
        if ($qconfig -notmatch 'QT_ARCH\s*=\s*arm64' -or (& $tools.Qmake -query QT_VERSION).Trim() -ne '6.11.2') {
            throw 'Arm64Qt must point to Qt 6.11.2 MSVC ARM64, with working host tools.'
        }
        $tools.VcVars = Join-Path (Split-Path $tools.VcVars) 'vcvarsall.bat'
        $environment = Get-MsvcEnvironment $tools.VcVars 'arm64'
        $tools.Compiler = Join-Path $environment.VCToolsInstallDir 'bin/Hostx64/arm64/cl.exe'
        $tools.CompilerVersion = (Get-Item -LiteralPath $tools.Compiler).VersionInfo.FileVersion.Split(' ')[0]
        # vcvars may report the newest x64-only redist when using an older
        # side-by-side ARM compiler. Select the matching v143 ARM runtime.
        $vcRoot = Split-Path (Split-Path (Split-Path $tools.VcVars))
        $crt = @(Get-ChildItem -Path "$vcRoot/Redist/MSVC/14.44.*/arm64/Microsoft.VC*.CRT" -Directory | Sort-Object FullName -Descending | Select-Object -First 1)
        if (!$crt.Count) { throw 'Visual Studio ARM64 redistributable DLLs are missing.' }
        $tools.Crt = $crt[0].FullName
        $tools.OpenSSLRoot = Get-Arm64OpenSSLRoot $Config -Prepare:$PrepareDependencies
        $tools.Architecture = 'arm64'
        return $tools
    }
    $qtRoots = @('D:\Qt','C:\Qt',(Join-Path $env:USERPROFILE 'Qt'))
    $qtCandidates = @(); $ninjaCandidates = @(); $cmakeCandidates = @(); $opensslCandidates = @()
    foreach ($root in $qtRoots) {
        $qtCandidates += "$root/6.11.2/msvc2022_64/bin/qmake.exe"
        $ninjaCandidates += "$root/Tools/Ninja/ninja.exe"
        $cmakeCandidates += "$root/Tools/CMake_64/bin/cmake.exe"
        $opensslCandidates += "$root/Tools/OpenSSLv3/Win_x64"
    }
    $qmake = Resolve-ConfiguredTool $Config 'Qmake' 'qmake.exe' $qtCandidates
    $qtVersion = (& $qmake -query QT_VERSION).Trim()
    if ($LASTEXITCODE -ne 0 -or $qtVersion -ne '6.11.2') { throw "WHAT requires Qt 6.11.2; selected kit reports $qtVersion." }
    $qt = Split-Path (Split-Path $qmake -Parent) -Parent
    $qconfig = Get-Content -LiteralPath "$qt/mkspecs/qconfig.pri" -Raw
    if ($qconfig -notmatch 'QT_MSVC_MAJOR_VERSION' -or $qconfig -notmatch 'QT_ARCH\s*=\s*x86_64') {
        throw 'Release builds require the Qt 6.11.2 MSVC2022 x64 kit. Update Qmake in release.local.json.'
    }
    $vswhere = Find-Tool 'vswhere.exe' @('C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe')
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio 2022 17.14+ or Visual Studio 2026 with Desktop development with C++ is required.' }
    $vcvars = Join-Path $vs 'VC/Auxiliary/Build/vcvars64.bat'
    $environment = Get-MsvcEnvironment $vcvars
    $compiler = Join-Path $environment.VCToolsInstallDir 'bin/Hostx64/x64/cl.exe'
    if ($Config.ContainsKey('Compiler') -and $Config.Compiler -and
        [IO.Path]::GetFullPath($Config.Compiler) -ne [IO.Path]::GetFullPath($compiler)) {
        throw 'Remove the Compiler override from release.local.json; the compiler is selected with its matching Visual Studio environment.'
    }
    $compilerVersion = (Get-Item -LiteralPath $compiler).VersionInfo.FileVersion.Split(' ')[0]
    if ([version]$compilerVersion -lt [version]'19.44') { throw "Qt 6.11.2 requires MSVC 19.44 or newer; found $compilerVersion. Update Visual Studio C++ tools." }
    $crt = @(Get-ChildItem -Path (Join-Path $environment.VCToolsRedistDir 'x64/Microsoft.VC*.CRT') -Directory | Select-Object -First 1)
    if (!$crt.Count) { throw 'Visual Studio x64 redistributable DLLs are missing.' }
    $cmake = Resolve-ConfiguredTool $Config 'CMake' 'cmake.exe' ($cmakeCandidates + @('C:\Program Files\CMake\bin\cmake.exe'))
    $ninja = Resolve-ConfiguredTool $Config 'Ninja' 'ninja.exe' $ninjaCandidates
    $opensslRoot = Get-Configured $Config 'OpenSSLRoot' ''
    if (!$opensslRoot) {
        foreach ($root in ($opensslCandidates + @("$script:ProjectRoot/build/qt-tools/Tools/OpenSSLv3/Win_x64", "$script:ProjectRoot/_local/openssl", 'C:/Program Files/OpenSSL-Win64'))) {
            if (Test-Path -LiteralPath "$root/lib/libcrypto.lib") { $opensslRoot = $root; break }
        }
    }
    foreach ($file in @('include/openssl/ssl.h','lib/libcrypto.lib','lib/libssl.lib','bin/libcrypto-3-x64.dll','bin/libssl-3-x64.dll','bin/openssl.exe')) {
        if (!$opensslRoot -or !(Test-Path -LiteralPath "$opensslRoot/$file")) {
            throw "MSVC OpenSSL 3 development files missing ($file). Install Qt's OpenSSL toolkit or set OpenSSLRoot to an x64 MSVC build."
        }
    }
    $openssl = Join-Path $opensslRoot 'bin/openssl.exe'
    $sdk = Get-Configured $Config 'WindowsSdkBin' ''
    if (!$sdk) {
        $sdk = Join-Path $environment.WindowsSdkDir "bin/$($environment.WindowsSDKVersion.TrimEnd('\'))/x64"
    }
    foreach ($tool in @('makeappx.exe','signtool.exe','rc.exe')) {
        if (!(Test-Path -LiteralPath (Join-Path $sdk $tool))) { throw "Windows SDK tool missing: $sdk/$tool" }
    }
    return @{ Architecture='x64'; Qmake=$qmake; HostQt=$qt; Qt=$qt; Compiler=$compiler; CompilerVersion=$compilerVersion; CMake=$cmake; Ninja=$ninja; OpenSSLRoot=$opensslRoot; OpenSSL=$openssl; VcVars=$vcvars; Crt=$crt[0].FullName; Sdk=$sdk }
}
function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    # Windows and Git may both provide the tool. Invoke only the first PATH match.
    $command = Get-Command -Name $Executable -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $previousPreference = $ErrorActionPreference
    try {
        # Windows PowerShell treats redirected native stderr as ErrorRecord objects,
        # even for warnings. Capture that text, but let the exit code decide failure.
        $ErrorActionPreference = 'Continue'
        & $command.Source @Arguments 2>&1 | ForEach-Object { $_.ToString() }
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0) { throw "$(Split-Path $Executable -Leaf) failed with exit code $exitCode. See the build log." }
}
function Get-ProjectVersion {
    $text = Get-Content -LiteralPath (Join-Path $script:ProjectRoot 'CMakeLists.txt') -Raw
    if ($text -notmatch 'project\(\s*WHAT\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') { throw 'Cannot read project version from CMakeLists.txt.' }
    $base = $Matches[1]
    # Only a single active declaration is valid; comments are never version metadata.
    $declarations = [regex]::Matches($text, '(?m)^[ \t]*set\([ \t]*WHAT_PRERELEASE[ \t]+"([^"\r\n]*)"[ \t]*\)[ \t]*(?:#[^\r\n]*)?\r?$')
    if ($declarations.Count -ne 1) { throw 'Expected exactly one active WHAT_PRERELEASE declaration in CMakeLists.txt.' }
    $suffix = $declarations[0].Groups[1].Value
    if ($suffix -and ($suffix -notmatch '^beta\.([1-9][0-9]{0,4})$' -or [int]$Matches[1] -gt 65535)) { throw 'WHAT_PRERELEASE must be empty or beta.N (1-65535).' }
    if ($suffix) { return "$base-$suffix" }
    return $base
}
function Get-PackageVersion([string]$ReleaseVersion) {
    if ($ReleaseVersion -notmatch '^(\d+\.\d+\.\d+)(?:-beta\.([1-9][0-9]{0,4}))?$') { throw 'Invalid release version.' }
    $base = $Matches[1]
    $revision = if ($Matches.ContainsKey(2)) { [int]$Matches[2] } else { 0 }
    if ($revision -gt 65535) { throw 'Beta number exceeds Windows version limit.' }
    return "$base.$revision"
}
function Get-SourceHashes {
    $result = [ordered]@{}
    # FileInfo expands Windows short names such as RUNNER~1. Use the same full
    # path for the root before removing it from each file's path.
    $sourceRoot = (Get-Item -LiteralPath $script:ProjectRoot).FullName.TrimEnd('\','/')
    $files = @(Get-Item (Join-Path $sourceRoot 'CMakeLists.txt'))
    foreach ($dir in @('src','qml','assets','tests','tools','scripts','packaging','licenses','docs')) { $files += @(Get-ChildItem -LiteralPath (Join-Path $sourceRoot $dir) -File -Recurse) }
    foreach ($file in ($files | Sort-Object FullName)) {
        $relative = $file.FullName.Substring($sourceRoot.Length + 1).Replace('\','/')
        # Python imports may populate these after compilation. They are neither
        # build inputs nor corresponding source for the release archive.
        if ($relative -match '(^|/)__pycache__/|\.(pyc|pyo)$') { continue }
        $result[$relative] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    return $result
}
function Write-FileHashes([string]$Directory) {
    $result = [ordered]@{}
    foreach ($file in (Get-ChildItem -LiteralPath $Directory -File -Recurse | Where-Object Name -ne 'SHA256SUMS.json' | Sort-Object FullName)) {
        $result[$file.FullName.Substring($Directory.Length+1).Replace('\','/')] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $Directory 'SHA256SUMS.json') -Encoding UTF8
}

function Invoke-QtTests([string]$Executable, [string]$LogPath) {
    $previousConsole = $env:QT_ASSUME_STDERR_HAS_CONSOLE
    try {
        # Qt GUI tests otherwise send console logging to the Windows debugger.
        # Keep the complete file log, with concise live progress during long suites.
        $env:QT_ASSUME_STDERR_HAS_CONSOLE = '1'
        Write-Host "Running $(Split-Path $Executable -Leaf). Full log: $LogPath"
        # Qt counts informational messages towards its default warning cap too.
        # Keep later warnings visible even when the suite logs many module starts.
        Invoke-Checked $Executable @('-v1', '-maxwarnings', '0', '-o', "$LogPath,txt", '-o', '-,txt') | ForEach-Object {
            if ($_ -match '^INFO.*entering$|^PASS\s|^FAIL!|^QFATAL|^SKIP\s|^Totals:') { Write-Host $_ }
        }
    } catch {
        # Include assertion details and source locations when a test fails.
        if (Test-Path -LiteralPath $LogPath) {
            $lines = @(Get-Content -LiteralPath $LogPath)
            $failures = @($lines | Select-String -Pattern '^FAIL!|^QFATAL|^Totals:|not hittable after layout/scroll' -Context 0,3)
            if ($failures.Count) {
                foreach ($failure in $failures) { Write-Host ($failure.ToString()) }
            } else {
                $lines | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
            }
        }
        throw
    } finally {
        $env:QT_ASSUME_STDERR_HAS_CONSOLE = $previousConsole
    }
}
