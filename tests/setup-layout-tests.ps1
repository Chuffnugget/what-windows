param([string]$OutputDirectory = (Join-Path $env:TEMP ('WHAT-setup-layout-' + [guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$compiler = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$executable = Join-Path $OutputDirectory 'SetupLayoutTests.exe'
# Compile the real wizard, with a test entry point that never installs anything.
$arguments = @('/nologo', '/target:exe', '/platform:x64', '/codepage:65001', '/main:SetupLayoutTests',
    "/out:$executable", '/reference:System.Windows.Forms.dll', '/reference:System.Drawing.dll', '/reference:System.Core.dll', '/reference:System.IO.Compression.dll', '/reference:System.Xml.dll',
    "/win32manifest:$root\packaging\Setup.manifest", "/resource:$root\assets\app-icon.ico,app.ico",
    "/resource:$root\assets\app-icon.png,app.png", "$root\packaging\Setup.cs", "$PSScriptRoot\SetupLayoutTests.cs")
& $compiler @arguments
if ($LASTEXITCODE -ne 0) { throw 'Installer layout test compilation failed.' }
& $executable $OutputDirectory "$root/qml/controls/Theme.qml"
if ($LASTEXITCODE -ne 0) { throw 'Installer layout test failed.' }
