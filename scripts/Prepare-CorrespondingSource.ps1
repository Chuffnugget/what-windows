param([Parameter(Mandatory)][string]$Runtime, [Parameter(Mandatory)][string]$Destination)
$ErrorActionPreference='Stop'
$inventory=Get-Content -LiteralPath "$Runtime/licenses/DEPENDENCIES.json" -Raw | ConvertFrom-Json
$version=[string]$inventory.qt
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid Qt source version.' }
$minor=($version.Split('.')[0..1] -join '.')
$cache=Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dependencies/qt-source'
New-Item -ItemType Directory -Path $cache,$Destination -Force | Out-Null
$hashes=[ordered]@{}
foreach($module in @('qtbase','qtdeclarative','qtsvg','qtwebsockets')) {
    $name="$module-everywhere-src-$version.tar.xz"
    $url="https://download.qt.io/archive/qt/$minor/$version/submodules/$name"
    $checksum=(Invoke-WebRequest -Uri "$url.sha256" -UseBasicParsing).Content
    if ($checksum -is [byte[]]) { $checksum=[Text.Encoding]::UTF8.GetString($checksum) }
    if ([string]$checksum -notmatch '(?i)\b([a-f0-9]{64})\b') { throw "Missing upstream SHA-256 for $name" }
    $expected=$Matches[1]
    $archive=Join-Path $cache $name
    if (!(Test-Path -LiteralPath $archive) -or (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $expected) {
        $download="$archive.download"
        Invoke-WebRequest -Uri $url -OutFile $download -UseBasicParsing
        if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $expected) { throw "Source checksum mismatch: $name" }
        Move-Item -LiteralPath $download -Destination $archive -Force
    }
    Copy-Item -LiteralPath $archive -Destination $Destination
    $hashes[$name]=$expected
}
$hashes | ConvertTo-Json | Set-Content -LiteralPath "$Destination/Qt-source-SHA256SUMS.json" -Encoding UTF8
