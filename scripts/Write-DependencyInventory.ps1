param([Parameter(Mandatory)][string]$Runtime, [Parameter(Mandatory)]$Tools)
$ErrorActionPreference='Stop'
$libraries=@(Get-ChildItem -LiteralPath $Runtime -Recurse -File -Filter '*.dll' | ForEach-Object {
    [ordered]@{file=$_.FullName.Substring($Runtime.Length).TrimStart('\','/').Replace('\','/'); version=$_.VersionInfo.FileVersion; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash}
})
$qt=(& $Tools.Qmake -query QT_VERSION).Trim()
if ($LASTEXITCODE -ne 0 -or $qt -notmatch '^\d+\.\d+\.\d+$') { throw 'Could not determine deployed Qt version.' }
# Read the target headers; the signing executable is a host tool in cross builds.
$opensslHeader = Get-Content "$($Tools.OpenSSLRoot)/include/openssl/opensslv.h" -Raw
if ($opensslHeader -notmatch 'OPENSSL_VERSION_TEXT\s+"([^"\r\n]+)"') { throw 'Missing target OpenSSL version.' }
$openssl = $Matches[1]
$compiler="MSVC " + (Get-Item -LiteralPath $Tools.Compiler).VersionInfo.FileVersion.Split(' ')[0]
[ordered]@{schema=1;qt=$qt;openssl=$openssl;compiler=$compiler;libraries=$libraries} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$Runtime/licenses/DEPENDENCIES.json" -Encoding UTF8
# The template is shared by source and runtime distributions; runtime text must match its kit.
$notice=Get-Content -LiteralPath "$Runtime/licenses/THIRD-PARTY.md" -Raw
$minor=($qt.Split('.')[0..1] -join '.')
$notice=$notice.Replace('6.11.2',$qt).Replace('/qt/6.11/','/qt/'+$minor+'/')
Set-Content -LiteralPath "$Runtime/licenses/THIRD-PARTY.md" -Value $notice -Encoding UTF8
