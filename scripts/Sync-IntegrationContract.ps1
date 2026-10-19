[CmdletBinding()]
param([string]$IntegrationRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '../what-integration'))
$ErrorActionPreference = 'Stop'
$IntegrationRoot = [IO.Path]::GetFullPath($IntegrationRoot)
$destination = Join-Path (Split-Path $PSScriptRoot -Parent) 'tests/integration_contract'
$files = @(
    'custom_components/what/api.py',
    'custom_components/what/protocol.py',
    'custom_components/what/custom_protocol.py',
    'custom_components/what/beacn_protocol.py',
    'custom_components/what/displays_protocol.py',
    'custom_components/what/dynamic_lighting.py',
    'custom_components/what/icue_protocol.py',
    'custom_components/what/icue_automation_protocol.py',
    'tests/test_protocol.py',
    'tests/test_live_app.py'
)
foreach ($file in $files) {
    if (!(Test-Path -LiteralPath "$IntegrationRoot/$file" -PathType Leaf)) { throw "Missing integration source: $file" }
}
$hashes = [ordered]@{}
foreach ($file in $files) {
    New-Item -ItemType Directory -Path (Split-Path "$destination/$file" -Parent) -Force | Out-Null
    Copy-Item -LiteralPath "$IntegrationRoot/$file" -Destination "$destination/$file" -Force
    $hashes[$file] = (Get-FileHash -LiteralPath "$destination/$file" -Algorithm SHA256).Hash
}
@{ source = 'what-integration'; files = $hashes } | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 "$destination/source.json"
Write-Host 'Integration contract synchronized. Review and commit the bundled files with the Windows changes.'
