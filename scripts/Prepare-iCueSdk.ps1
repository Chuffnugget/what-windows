[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference='Stop'
# Qt Creator/Ninja can inherit PowerShell 7's module search path before starting
# Windows PowerShell. Put this host's built-in modules first for hashing/archive.
$env:PSModulePath=(Join-Path $PSHOME 'Modules')+[IO.Path]::PathSeparator+$env:PSModulePath
$root=Split-Path $PSScriptRoot -Parent
$cache=Join-Path $root 'build/dependencies/icue-4.0.84'
$archive=Join-Path $cache 'sdk.zip'
New-Item -ItemType Directory -Path $cache -Force | Out-Null
$expected='EB4414CF505145F3E507DC839AD587BF7CA684D64BE3D9A834F416A136736D5D'
if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
    $download="$archive.download"
    $url='https://github.com/CorsairOfficial/cue-sdk/releases/download/v4.0.84/iCUESDK_4.0.84.zip'
    try { Invoke-WebRequest -Uri $url -OutFile $download -UseBasicParsing }
    catch {
        # Python uses a separate verified TLS implementation on developer hosts
        # where Windows' HTTP stack is unavailable; never disable verification.
        $python=Get-Command python.exe -ErrorAction SilentlyContinue
        if (!$python) { throw }
        & $python.Source -c 'import sys,urllib.request; urllib.request.urlretrieve(sys.argv[1],sys.argv[2])' $url $download
        if ($LASTEXITCODE -ne 0) { throw 'Could not download the iCUE SDK over verified HTTPS.' }
    }
    if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $expected) { throw 'Downloaded iCUE SDK checksum mismatch.' }
    Move-Item -LiteralPath $download -Destination $archive -Force
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) { throw 'iCUE SDK checksum mismatch. Remove build/dependencies/icue-4.0.84/sdk.zip and retry.' }
Expand-Archive -LiteralPath $archive -DestinationPath $cache -Force
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
function Copy-Changed([string]$Source) {
    $target=Join-Path $Destination (Split-Path $Source -Leaf)
    if (!(Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $Source).Hash -ne (Get-FileHash -LiteralPath $target).Hash) {
        Copy-Item -LiteralPath $Source -Destination $target -Force
    }
}
Copy-Changed (Join-Path $cache 'iCUESDK/redist/x64/iCUESDK.x64_2019.dll')

# WHAT and the SDK use MSVC. Distribute the
# supported app-local CRT from the builder's VS redist directory, not System32.
# These files travel inside portable/MSIX/Setup payloads and need no separate
# installation or UAC prompt on the user's PC.
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio C++ build tools are required to prepare the iCUE runtime (vswhere.exe missing).' }
$installations=@(& $vswhere -all -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if ($LASTEXITCODE -ne 0) { throw 'Could not locate the Visual Studio C++ redistributable files.' }
$candidates=@(foreach ($installation in $installations) {
    Get-ChildItem -Path (Join-Path $installation 'VC/Redist/MSVC/*/x64/Microsoft.VC*.CRT') -Directory -ErrorAction SilentlyContinue
})
$crt=$candidates | Where-Object {
    (Test-Path -LiteralPath (Join-Path $_.FullName 'msvcp140.dll')) -and
    (Test-Path -LiteralPath (Join-Path $_.FullName 'vcruntime140.dll')) -and
    (Test-Path -LiteralPath (Join-Path $_.FullName 'vcruntime140_1.dll'))
} | Sort-Object { [version](Get-Item -LiteralPath (Join-Path $_.FullName 'msvcp140.dll')).VersionInfo.FileVersion.Split(' ')[0] } -Descending | Select-Object -First 1
if (!$crt) { throw 'Visual Studio is missing its x64 C++ redistributable files. Add the Desktop development with C++ workload in Visual Studio Installer.' }
foreach ($file in Get-ChildItem -LiteralPath $crt.FullName -Filter '*.dll' -File) { Copy-Changed $file.FullName }
foreach ($name in @('iCUESDK.x64_2019.dll','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    if (!(Test-Path -LiteralPath (Join-Path $Destination $name))) { throw "iCUE deployment incomplete: $name missing." }
}
Write-Host "iCUE SDK 4.0.84 and x64 Microsoft runtime prepared: $Destination"
