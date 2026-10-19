param([string]$OutputDirectory = (Join-Path $env:TEMP ('what-release-upload-tests-' + [guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$root = Split-Path $PSScriptRoot -Parent
foreach ($name in @('WHAT-0.0.1-beta.5-Windows-x64-Setup.exe', 'WHAT-0.0.1-beta.5-Windows-x64-UpdatePayload.msix', 'WHAT-0.0.1-beta.5-Windows-x64-Portable.zip', 'WHAT-0.0.1-beta.5-Source.zip')) {
    Set-Content -LiteralPath (Join-Path $OutputDirectory $name) -Value "fixture:$name"
}
@{ architecture='x64'; version='0.0.1-beta.5' } | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'update.json')
$global:whatUploadFixtureAssets = @(Get-ChildItem -LiteralPath $OutputDirectory -File | ForEach-Object {
    @{
        name = $_.Name
        size = $_.Length
        state = 'uploaded'
        digest = 'sha256:' + (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()
    }
})
# Replace the CLI boundary before invoking the real publication script.
# These tests make no network requests and do not modify GitHub releases.
function global:gh {
    $call = @($args)
    $global:whatUploadFixtureCalls.Add(($call -join ' '))
    $global:LASTEXITCODE = 0
    if ($call[0] -eq 'release' -and $call[1] -eq 'view') {
        if ($global:whatUploadFixtureCase -eq 'new' -and !$global:whatUploadFixtureCreated) {
            $global:LASTEXITCODE = 1
            return
        }
        if ($global:whatUploadFixtureUploaded -and $global:whatUploadFixtureCase -eq 'lookup-failed') {
            $global:LASTEXITCODE = 1
            return
        }
        $draft = $global:whatUploadFixtureCase -ne 'published'
        $tag = 'v0.0.1-beta.5'
        if ($global:whatUploadFixtureUploaded) {
            if ($global:whatUploadFixtureCase -eq 'published-during-upload') { $draft = $false }
            if ($global:whatUploadFixtureCase -eq 'tag-changed') { $tag = 'v0.0.1-beta.6' }
        }
        return @{databaseId=42; isDraft=$draft; tagName=$tag} | ConvertTo-Json -Compress
    }
    if ($call[0] -eq 'release' -and $call[1] -eq 'create') {
        $global:whatUploadFixtureCreated = $true
        return
    }
    if ($call[0] -eq 'release' -and $call[1] -eq 'upload') {
        if ($global:whatUploadFixtureCase -eq 'upload-failed') {
            $global:LASTEXITCODE = 1
            return
        }
        $global:whatUploadFixtureUploaded = $true
        return
    }
    if ($call[0] -eq 'release' -and $call[1] -eq 'edit') { return }
    if ($call[0] -eq 'api') {
        # GitHub's release-by-tag endpoint does not return drafts.
        if ($call[1] -like '*/releases/tags/*') {
            $global:LASTEXITCODE = 1
            return
        }
        if ($call[1] -ne 'repos/fixture/what/releases/42/assets?per_page=100') {
            throw "Unexpected fixture API path: $($call[1])"
        }
        $assets = @($global:whatUploadFixtureAssets | ForEach-Object { $_.Clone() })
        if ($global:whatUploadFixtureCase -eq 'digest-mismatch') { $assets[0].digest = 'sha256:wrong' }
        if ($global:whatUploadFixtureCase -eq 'missing') { $assets = $assets[1..($assets.Count - 1)] }
        if ($global:whatUploadFixtureCase -eq 'extra') {
            $assets += @{name='stale.zip'; size=1; state='uploaded'; digest='sha256:wrong'}
        }
        return $assets | ForEach-Object { $_ | ConvertTo-Json -Compress }
    }
    throw "Unexpected fixture CLI call: $($call -join ' ')"
}
try {
    $cases = @('new', 'draft', 'published', 'upload-failed', 'digest-mismatch', 'missing', 'extra',
        'lookup-failed', 'published-during-upload', 'tag-changed')
    foreach ($case in $cases) {
        $global:whatUploadFixtureCase = $case
        $global:whatUploadFixtureCalls = [Collections.Generic.List[string]]::new()
        $global:whatUploadFixtureCreated = $false
        $global:whatUploadFixtureUploaded = $false
        $failure = $null
        try {
            & "$root/scripts/Publish-Release.ps1" -ArtifactDirectory $OutputDirectory -Tag 'v0.0.1-beta.5' -Repository 'fixture/what'
        } catch {
            $failure = $_
        }
        $published = @($global:whatUploadFixtureCalls | Where-Object { $_ -like 'release edit*--draft=false*' }).Count
        if ($case -in @('new', 'draft')) {
            if ($failure -or $published -ne 1) { throw "Complete fixture did not publish: $case. $failure" }
        } elseif (!$failure -or $published -ne 0) {
            throw "Unsafe fixture was published: $case"
        }
        if ($case -eq 'new' -and !$global:whatUploadFixtureCreated) { throw 'New release was not created.' }
        if ($case -eq 'draft' -and $global:whatUploadFixtureCreated) { throw 'Existing draft was recreated.' }
        if ($case -eq 'published' -and @($global:whatUploadFixtureCalls | Where-Object { $_ -like 'release upload*' }).Count) {
            throw 'Published release assets were replaced.'
        }
        Write-Host "PASS: release upload boundary $case"
    }
    # Add a second architecture to the same release, still using a mocked CLI.
    foreach ($ending in @('Setup.exe', 'UpdatePayload.msix', 'Portable.zip')) {
        Set-Content (Join-Path $OutputDirectory "WHAT-0.0.1-beta.5-Windows-arm64-$ending") 'ARM64 fixture'
    }
    @{ architecture='arm64'; version='0.0.1-beta.5' } | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'update-arm64.json')
    $global:whatUploadFixtureAssets = @(Get-ChildItem -LiteralPath $OutputDirectory -File | ForEach-Object {
        @{ name=$_.Name; size=$_.Length; state='uploaded'; digest=('sha256:' + (Get-FileHash -LiteralPath $_.FullName).Hash.ToLowerInvariant()) }
    })
    $global:whatUploadFixtureCase = 'draft'
    $global:whatUploadFixtureCalls.Clear()
    & "$root/scripts/Publish-Release.ps1" -ArtifactDirectory $OutputDirectory -Tag 'v0.0.1-beta.5' -Repository 'fixture/what'
    if (@($global:whatUploadFixtureCalls | Where-Object { $_ -like 'release edit*--draft=false*' }).Count -ne 1) { throw 'Combined architecture release was rejected.' }
    @{ architecture='x64'; version='0.0.1-beta.5' } | ConvertTo-Json | Set-Content (Join-Path $OutputDirectory 'update-arm64.json')
    $global:whatUploadFixtureCalls.Clear()
    $rejected = $false
    try { & "$root/scripts/Publish-Release.ps1" -ArtifactDirectory $OutputDirectory -Tag 'v0.0.1-beta.5' -Repository 'fixture/what' }
    catch { $rejected = $_.Exception.Message -match 'architecture differs' }
    if (!$rejected -or $global:whatUploadFixtureCalls.Count) { throw 'Mismatched ARM64 manifest reached GitHub.' }
    Write-Host 'PASS: combined architectures and mismatched manifest rejection'
} finally {
    Remove-Item Function:\gh
    Remove-Variable whatUploadFixtureAssets,whatUploadFixtureCase,whatUploadFixtureCalls,whatUploadFixtureCreated,whatUploadFixtureUploaded -Scope Global -ErrorAction SilentlyContinue
}
