param([Parameter(Mandatory)][string]$Destination)
. "$PSScriptRoot/Release.Common.ps1"
Add-Type -AssemblyName System.IO.Compression.FileSystem
# Explicit public source roots, including uncommitted build inputs. Never archive
# the working directory wholesale: it contains signing keys and local config.
$files=@(Get-SourceHashes).Keys
$files+=@('README.md','.clang-format','.cmake-format.json','.editorconfig','.gitattributes','LICENSE','COPYING.GPL-3.0','COPYRIGHT','release.local.example.json','.gitignore')
$files+=@(Get-ChildItem -LiteralPath "$ProjectRoot/.github" -File -Recurse | ForEach-Object { $_.FullName.Substring($ProjectRoot.Length+1) })
$zip=[IO.Compression.ZipFile]::Open($Destination,[IO.Compression.ZipArchiveMode]::Create)
try {
    foreach($relative in ($files | Sort-Object -Unique)) {
        $path=Join-Path $ProjectRoot $relative
        if(Test-Path -LiteralPath $path -PathType Leaf){[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$path,$relative.Replace('\','/'),[IO.Compression.CompressionLevel]::Optimal) | Out-Null}
    }
} finally { $zip.Dispose() }
