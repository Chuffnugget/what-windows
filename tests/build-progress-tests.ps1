param([string]$OutputDirectory = (Join-Path $env:TEMP ('what-build-progress-' + [guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
. "$root/scripts/Build.Progress.ps1"
$fixture = Join-Path $OutputDirectory 'worker with spaces.ps1'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
@'
param([switch]$BuildWorker, [string]$Architecture, [switch]$ValidateOnly)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
if (!$BuildWorker) { throw 'Worker flag missing' }
if ([Console]::In.ReadToEnd() -ne '') { throw 'Unexpected standard input' }
Write-Host 'WHAT build stage: Prerequisites'
Write-Host ('Architecture=' + $Architecture + '; ValidateOnly=' + [bool]$ValidateOnly)
Write-Host ('Unicode=' + [char]0x00e9)
if ($Architecture -eq 'all') {
    $shell = Join-Path $PSHOME 'powershell.exe'
    if (!(Test-Path -LiteralPath $shell)) { $shell = Join-Path $PSHOME 'pwsh.exe' }
    $child = Start-Process -FilePath $shell -ArgumentList '-NoLogo -NoProfile -NonInteractive -Command "Start-Sleep -Seconds 60"' -WindowStyle Hidden -PassThru
    Set-Content -LiteralPath "$PSScriptRoot/child.pid" -Value $child.Id
    Write-Host 'WHAT build task: Cancel fixture'
    Start-Sleep -Seconds 60
    exit 0
}
if (!$ValidateOnly) {
    Write-Host 'WHAT build stage: x64 Build'
    Write-Host 'WHAT build task: Compiling x64'
    Write-Host '[2/4 1.0/sec] Compiling fixture.cpp'
}
Start-Sleep -Milliseconds 2300
for ($i = 0; $i -lt 1000; $i++) {
    [Console]::Out.WriteLine('stdout-' + $i + ': ' + ('x' * 80))
    [Console]::Error.WriteLine('stderr-' + $i + ': ' + ('y' * 80))
}
[Console]::Out.Write('last line without newline')
if ($Architecture -eq 'arm64') { exit 7 }
exit 0
'@ | Set-Content -LiteralPath $fixture -Encoding UTF8

# The native progress overlay flashes whenever compiler output scrolls.
function Write-Progress {
    param($Id, $ParentId, $Activity, $Status, $PercentComplete, [switch]$Completed)
    throw 'Build output must not use the flashing native progress overlay.'
}

foreach ($case in @(
    @{ architecture='x64'; validate=$false; exit=0 },
    @{ architecture='arm64'; validate=$true; exit=7 }
)) {
    $messages = @()
    $exitCode = Invoke-BuildProgress -ScriptPath $fixture -Architecture $case.architecture -ValidateOnly:$case.validate -HeartbeatSeconds 1 -InformationVariable messages 6>$null
    if ($exitCode -ne $case.exit) { throw 'Worker exit code was not preserved.' }
    $text = ($messages | ForEach-Object { $_.ToString() }) -join "`n"
    foreach ($expected in @('stdout-999:', 'stderr-999:', 'last line without newline', '[Build active:', ('Unicode=' + [char]0x00e9))) {
        if (!$text.Contains($expected)) { throw "Missing worker output: $expected" }
    }
    if (!$text.Contains("Architecture=$($case.architecture); ValidateOnly=$($case.validate)")) { throw 'Worker arguments changed.' }
    if (!$case.validate -and !$text.Contains('Compiling x64 [##########----------] 50%')) {
        throw 'Ninja completion did not reach the progress display.'
    }
    $milestones = @($messages | Where-Object { $_.ToString().Contains('] Compiling x64 [##########----------] 50%') })
    if ($milestones.Count -gt 1) { throw 'Unchanged compiler progress was repeatedly printed.' }
    Write-Host "PASS: $($case.architecture), quiet heartbeat, both output streams, arguments and exit $exitCode."
}

# Throw while the worker is alive to exercise the same cleanup as cancellation.
function Write-Host {
    param($Object)
    if ($Object -eq 'WHAT build task: Cancel fixture') { throw 'fixture cancellation' }
    Microsoft.PowerShell.Utility\Write-Host $Object
}
$cancelled = $false
try { Invoke-BuildProgress -ScriptPath $fixture -Architecture all 6>$null }
catch { $cancelled = $_.Exception.Message -match 'fixture cancellation' }
if (!$cancelled) { throw 'Cancellation fixture did not stop the monitor.' }
$childId = [int](Get-Content -LiteralPath "$OutputDirectory/child.pid")
if (Get-Process -Id $childId -ErrorAction SilentlyContinue) { throw 'Worker descendant survived cancellation.' }
Write-Host 'PASS: cancellation stops the worker and its child process.'
