[CmdletBinding()]
param([string]$Perl)
. "$PSScriptRoot/Release.Common.ps1"
$root = Join-Path $ProjectRoot 'build/dependencies'
$prefix = Join-Path $root 'openssl-arm64'
$source = Join-Path $root 'openssl-3.5.9'
New-Item -ItemType Directory -Path $root -Force | Out-Null

function Get-VerifiedArchive([string]$Url, [string]$Path, [string]$Hash) {
    if (!(Test-Path -LiteralPath $Path)) {
        Write-Host "Downloading $(Split-Path $Path -Leaf)..."
        Invoke-WebRequest $Url -OutFile $Path -UseBasicParsing
    }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Hash) {
        throw "Archive checksum differs: $Path. Remove it and retry."
    }
}

function Expand-PerlRuntime([string]$Archive, [string]$Destination) {
    $executable = Join-Path $Destination 'perl/bin/perl.exe'
    $completed = Join-Path $Destination '.what-perl-ready'
    if ((Test-Path -LiteralPath $completed -PathType Leaf) -and
        (Test-Path -LiteralPath $executable -PathType Leaf)) { return }

    if (Test-Path -LiteralPath $completed -PathType Leaf) { Remove-Item -LiteralPath $completed -Force }
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Write-Host 'Extracting the portable Perl runtime...'
    # MSVC supplies the compiler. Skip Strawberry's GCC tools and avoid
    # Expand-Archive's rollback, which can hide the original extraction error.
    Invoke-Checked 'tar.exe' @('-xf', $Archive, '-C', $Destination, 'perl/') | ForEach-Object { Write-Host $_ }
    if (!(Test-Path -LiteralPath $executable -PathType Leaf)) {
        throw "Perl extraction did not produce $executable."
    }
    # A partial extraction can already contain perl.exe. Only reuse a directory
    # after the complete runtime has been extracted successfully.
    Set-Content -LiteralPath $completed -Value '5.40.0.1' -Encoding ASCII
}

# A portable build tool, installed only under this checkout. MSYS Perl cannot
# generate a native MSVC build, so do not pick Git's perl.exe from PATH.
if (!$Perl) {
    $perlRoot = Join-Path $root 'strawberry-perl-5.40.0.1'
    $archive = Join-Path $root 'strawberry-perl-5.40.0.1.zip'
    Get-VerifiedArchive 'https://github.com/StrawberryPerl/Perl-Dist-Strawberry/releases/download/SP_54001_64bit_UCRT/strawberry-perl-5.40.0.1-64bit-portable.zip' $archive '754f3e2a8e473dc68d1540c7802fb166a025f35ef18960c4564a31f8b5933907'
    Expand-PerlRuntime $archive $perlRoot
    $Perl = "$perlRoot/perl/bin/perl.exe"
}
$archive = Join-Path $root 'openssl-3.5.9.tar.gz'
Get-VerifiedArchive 'https://github.com/openssl/openssl/releases/download/openssl-3.5.9/openssl-3.5.9.tar.gz' $archive '603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a'
if (!(Test-Path -LiteralPath "$source/Configure")) {
    Invoke-Checked 'tar.exe' @('-xf', $archive, '-C', $root)
}
$vswhere = Find-Tool 'vswhere.exe' @('C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe')
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs -or !(Test-Path "$vs/VC/Tools/MSVC/14.44.*/bin/Hostx64/arm64/cl.exe")) {
    throw 'Install MSVC v143 14.44 ARM64 build tools first.'
}
$command = Join-Path $root 'build-openssl-arm64.cmd'
@"
@echo off
call "$vs\VC\Auxiliary\Build\vcvarsall.bat" amd64_arm64 -vcvars_ver=14.44 >nul
if errorlevel 1 exit /b 1
set "PATH=$(Split-Path $Perl);%PATH%"
rem Native Windows Perl does not recognize Unix UTF-8 locale names.
set "LC_ALL=C"
cd /d "$source"
"$Perl" Configure VC-WIN64-ARM shared no-tests no-makedepend --prefix="$prefix" --openssldir="$prefix/ssl" --libdir=lib
if errorlevel 1 exit /b 1
nmake /nologo
if errorlevel 1 exit /b 1
nmake /nologo install_sw
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $command -Encoding ASCII
# Use the host stream so Build-All records this output during tool discovery.
Invoke-Checked $env:ComSpec @('/d', '/c', "`"$command`"") | ForEach-Object { Write-Host $_ }
Write-Host "ARM64 OpenSSL: $prefix"
