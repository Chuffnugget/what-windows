# Keep the display responsive even while a build tool produces no output. The
# worker still owns compilation, signing, transcripts and the final report.
function Invoke-BuildProgress {
    param(
        [Parameter(Mandatory)][string]$ScriptPath,
        [ValidateSet('all', 'x64', 'arm64')][string]$Architecture = 'all',
        [switch]$ValidateOnly,
        [ValidateRange(1, 300)][int]$HeartbeatSeconds = 15
    )

    if (!('WhatBuildConsole' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WhatBuildConsole
{
    [DllImport("kernel32.dll")] static extern IntPtr GetStdHandle(int handle);
    [DllImport("kernel32.dll")] static extern bool GetConsoleMode(IntPtr handle, out uint mode);
    [DllImport("kernel32.dll")] static extern bool SetConsoleMode(IntPtr handle, uint mode);
    public static int DisableSelectionPause()
    {
        uint mode;
        IntPtr input = GetStdHandle(-10);
        if (!GetConsoleMode(input, out mode)) return -1;
        // Quick Edit can suspend output until Enter/Escape ends selection.
        if (!SetConsoleMode(input, (mode | 0x80u) & ~0x40u)) return -1;
        return (int)mode;
    }
    public static void Restore(int mode)
    {
        if (mode >= 0) SetConsoleMode(GetStdHandle(-10), (uint)mode);
    }
}
'@
    }

    $stages = @('Prerequisites', 'Signing')
    if (!$ValidateOnly) {
        $targets = if ($Architecture -eq 'all') { @('x64', 'arm64') } else { @($Architecture) }
        foreach ($target in $targets) { $stages += "$target Build", "$target Packaging" }
    }
    $stage = 'Starting'
    $stageNumber = 0
    $task = 'Starting build worker'
    $taskPercent = -1
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $lastOutput = 0.0
    $lastHeartbeat = 0.0
    $lastPaint = -1.0
    $lastProgressKey = ''
    $exitObserved = -1.0
    $consoleMode = [WhatBuildConsole]::DisableSelectionPause()
    $process = [Diagnostics.Process]::new()
    $started = $false
    try {
        $shell = Join-Path $PSHOME 'powershell.exe'
        if (!(Test-Path -LiteralPath $shell)) { $shell = Join-Path $PSHOME 'pwsh.exe' }
        $process.StartInfo.FileName = $shell
        # Only fixed switches and the validated architecture enter this command.
        $process.StartInfo.Arguments = '-NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -OutputFormat Text -File "' +
            (Resolve-Path -LiteralPath $ScriptPath).Path + '" -BuildWorker -Architecture ' + $Architecture
        if ($ValidateOnly) { $process.StartInfo.Arguments += ' -ValidateOnly' }
        $process.StartInfo.WorkingDirectory = (Get-Location).ProviderPath
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        $process.StartInfo.RedirectStandardInput = $true
        $process.StartInfo.StandardOutputEncoding = [Text.UTF8Encoding]::new($false)
        $process.StartInfo.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
        $started = $process.Start()
        # Build tools must not wait for a keystroke from the progress console.
        $process.StandardInput.Close()
        $streams = @(
            @{ reader=$process.StandardOutput; read=$process.StandardOutput.ReadLineAsync(); done=$false },
            @{ reader=$process.StandardError; read=$process.StandardError.ReadLineAsync(); done=$false }
        )
        Write-Host 'Build progress is live. Quiet stages show elapsed time; Ctrl+C cancels the build.'
        while (!$process.HasExited -or !$streams[0].done -or !$streams[1].done) {
            foreach ($stream in $streams) {
                # Drain both pipes without blocking on either or letting a noisy
                # compiler starve the progress display and cancellation checks.
                for ($n = 0; $n -lt 100 -and !$stream.done -and $stream.read.IsCompleted; $n++) {
                    $line = $stream.read.GetAwaiter().GetResult()
                    if ($null -eq $line) { $stream.done = $true; break }
                    $lastOutput = $clock.Elapsed.TotalSeconds
                    Write-Host $line
                    if ($line -match '^WHAT build stage: (.+)$') {
                        $stage = $Matches[1]
                        $stageNumber = [Array]::IndexOf($stages, $stage) + 1
                        $task = $stage
                        $taskPercent = -1
                    } elseif ($line -match '^WHAT build task: (.+)$') {
                        $task = $Matches[1]
                        $taskPercent = -1
                    } elseif ($line -match '^\[(\d+)/(\d+)[^\]]*\]') {
                        $finished = [double]$Matches[1]
                        $total = [double]$Matches[2]
                        if ($total -gt 0) { $taskPercent = [math]::Min(100, [int](100 * $finished / $total)) }
                    } elseif ($line -match '^Running (.+)\. Full log:') {
                        $task = "Testing $($Matches[1])"
                        $taskPercent = -1
                    } elseif ($line -match '^Downloading .+') {
                        $task = $line
                        $taskPercent = -1
                    }
                    $stream.read = $stream.reader.ReadLineAsync()
                }
            }
            $seconds = $clock.Elapsed.TotalSeconds
            if ($process.HasExited) {
                if ($exitObserved -lt 0) { $exitObserved = $seconds }
                # A launched application can inherit a pipe handle. Do not wait
                # forever for its EOF after the build worker has already exited.
                if ($seconds - [math]::Max($exitObserved, $lastOutput) -ge 5) {
                    Write-Warning 'The build worker exited, but another process kept its output pipe open. See the compilation report for the build result.'
                    break
                }
            }
            if ($seconds - $lastPaint -ge 1) {
                $elapsed = $clock.Elapsed.ToString('hh\:mm\:ss')
                $quiet = [int]($seconds - $lastOutput)
                $filled = if ($taskPercent -lt 0) { -1 } else { [int][math]::Floor($taskPercent / 5) }
                $progressKey = "$stageNumber|$task|$filled"
                if ($progressKey -ne $lastProgressKey) {
                    $taskDisplay = $task
                    if ($filled -ge 0) {
                        $bar = ('#' * $filled) + ('-' * (20 - $filled))
                        $taskDisplay += " [$bar] $taskPercent%"
                    }
                    # Windows PowerShell hides its blue progress overlay on each
                    # output line, then redraws it. Plain milestone lines avoid
                    # that flashing while keeping compiler progress in the log.
                    Write-Host "[Build $stageNumber/$($stages.Count): $stage | $elapsed] $taskDisplay"
                    $lastProgressKey = $progressKey
                }
                $lastPaint = $seconds
                # Quiet tools still get a periodic status in terminals and CI.
                if ($seconds - $lastOutput -ge $HeartbeatSeconds -and $seconds - $lastHeartbeat -ge $HeartbeatSeconds) {
                    Write-Host "[Build active: $elapsed] $stage / $task; no tool output for $quiet s (PID $($process.Id))."
                    $lastHeartbeat = $seconds
                }
            }
            Start-Sleep -Milliseconds 50
        }
        $process.WaitForExit()
        return $process.ExitCode
    } finally {
        try {
            if ($started -and !$process.HasExited) {
                # Cancel only this worker and its compiler/test descendants.
                & "$env:SystemRoot/System32/taskkill.exe" /PID $process.Id /T /F 2>&1 | Out-Null
            }
        } finally {
            $process.Dispose()
            [WhatBuildConsole]::Restore($consoleMode)
        }
    }
}
