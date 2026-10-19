[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ArtifactDirectory,
    [Parameter(Mandatory)][string]$Tag,
    [Parameter(Mandatory)][string]$Repository
)
$ErrorActionPreference = 'Stop'
if ($Tag -notmatch '^v[0-9]+\.[0-9]+\.[0-9]+(?:-beta\.[0-9]+)?$') { throw 'Invalid release tag.' }
if ($Repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'Invalid repository.' }
$files = @(Get-ChildItem -LiteralPath $ArtifactDirectory -File)
$manifests = @($files | Where-Object Name -in @('update.json', 'update-arm64.json'))
if (!$manifests.Count) { throw 'No supported update manifest found.' }
foreach ($manifest in $manifests) {
    $update = Get-Content -LiteralPath $manifest.FullName -Raw | ConvertFrom-Json
    $architecture = if ($manifest.Name -eq 'update.json') { 'x64' } else { 'arm64' }
    if ($update.architecture -ne $architecture -or ('v' + $update.version) -ne $Tag) {
        throw "Update manifest version or architecture differs: $($manifest.Name)"
    }
    foreach ($ending in @('Setup.exe', 'UpdatePayload.msix', 'Portable.zip')) {
        $name = "WHAT-$($update.version)-Windows-$architecture-$ending"
        if (@($files | Where-Object Name -ceq $name).Count -ne 1) { throw "Missing release asset: $name" }
    }
}
foreach ($pattern in @('*Setup.exe', '*UpdatePayload.msix', '*Portable.zip')) {
    if (@($files | Where-Object Name -like $pattern).Count -ne $manifests.Count) {
        throw "Release assets do not match the supplied architectures: $pattern"
    }
}
if (@($files | Where-Object Name -like '*Source.zip').Count -ne 1) { throw 'Expected one WHAT source archive.' }
if ($files | Where-Object { $_.Length -eq 0 -or $_.Extension -in @('.pfx', '.key', '.pem') }) { throw 'Empty or private release asset.' }
function Invoke-ReleaseGh([string[]]$Arguments) {
    $output = & gh @Arguments
    if ($LASTEXITCODE -ne 0) { throw "GitHub release operation failed: $($Arguments[0]). The release remains unpublished." }
    return $output
}
# A rerun can resume a draft. Never overwrite a published release.
$existing = & gh release view $Tag --repo $Repository --json isDraft 2>$null
if ($LASTEXITCODE -eq 0) {
    if (($existing | ConvertFrom-Json).isDraft -ne $true) { throw 'Release already published; refusing to replace assets.' }
} else {
    Invoke-ReleaseGh @('release', 'create', $Tag, '--repo', $Repository, '--verify-tag', '--draft', '--title', $Tag,
        '--notes', 'For Windows installation, download and run the Setup.exe asset. UpdatePayload.msix and update.json are for the in-app updater.', '--generate-notes') | Out-Host
}
Invoke-ReleaseGh (@('release', 'upload', $Tag, '--repo', $Repository, '--clobber') + @($files.FullName)) | Out-Host
# Verify the exact remote set and content before making the draft public. This
# deliberately fails closed on missing digests, interrupted uploads or stale assets.
# The REST release-by-tag endpoint only returns published releases. The CLI
# can find drafts and supplies the numeric ID needed to list their assets.
$release = (Invoke-ReleaseGh @('release', 'view', $Tag, '--repo', $Repository,
    '--json', 'databaseId,isDraft,tagName')) | ConvertFrom-Json
if ($release.isDraft -ne $true -or $release.tagName -cne $Tag) { throw 'Release changed during upload; refusing publication.' }
$assets = @(Invoke-ReleaseGh @('api', "repos/$Repository/releases/$($release.databaseId)/assets?per_page=100", '--paginate', '--jq', '.[] | @json') |
    ForEach-Object { $_ | ConvertFrom-Json })
if ($assets.Count -ne $files.Count) { throw 'Draft assets do not match this build; refusing publication.' }
foreach ($file in $files) {
    $asset = @($assets | Where-Object name -ceq $file.Name)
    $digest = 'sha256:' + (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($asset.Count -ne 1 -or $asset[0].state -ne 'uploaded' -or $asset[0].size -ne $file.Length -or $asset[0].digest -cne $digest) {
        throw "Draft asset verification failed: $($file.Name)."
    }
}
$flags = if ($Tag -like '*-beta.*') { @('--prerelease', '--latest=false') } else { @('--prerelease=false', '--latest') }
Invoke-ReleaseGh (@('release', 'edit', $Tag, '--repo', $Repository, '--draft=false') + $flags) | Out-Host
