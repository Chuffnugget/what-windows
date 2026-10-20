[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateRange(1, 2147483647)][int]$ProcessId,
    [ValidateRange(1, 240)][int]$Minutes = 60,
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../build/diagnostics/hangs')
)

# Watch one existing WHAT process. Nothing is installed, uploaded or terminated.
# A small thread-stack dump is saved only after repeated failed UI probes.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$appProcess = Get-Process -Id $ProcessId -ErrorAction Stop
$log = $null
try {
    $executable = $appProcess.Path
    if ([IO.Path]::GetFileName($executable) -ine 'WHAT.exe') {
        throw 'ProcessId must identify WHAT.exe.'
    }
    # Keep this process handle open so a reused PID cannot target a different app.
    $null = $appProcess.Handle
    $started = $appProcess.StartTime.ToUniversalTime()
    $output = [IO.Path]::GetFullPath($OutputDirectory)
    $null = New-Item -ItemType Directory -Path $output -Force
    $captureId = '{0}-{1}-{2}' -f (Get-Date -Format 'yyyyMMdd-HHmmss'), $ProcessId, ([guid]::NewGuid().ToString('N').Substring(0, 8))
    $log = Join-Path $output "$captureId.log"
    @("Executable: $executable", "PID: $ProcessId", "Started UTC: $($started.ToString('o'))",
      'UTC,UI,GDI,USER,Handles,WorkingSetBytes,TotalCpuSeconds') | Set-Content -LiteralPath $log -Encoding UTF8

    if (-not ('WhatHangCapture' -as [type])) {
        Add-Type @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;

public static class WhatHangCapture {
    private delegate bool WindowCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetProp(IntPtr window, string name);
    [DllImport("user32.dll", SetLastError = true)] private static extern IntPtr SendMessageTimeout(
        IntPtr window, uint message, UIntPtr wParam, IntPtr lParam, uint flags, uint timeout, out UIntPtr result);
    [DllImport("user32.dll")] public static extern uint GetGuiResources(IntPtr process, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, uint id);
    [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
    [DllImport("dbghelp.dll", SetLastError = true)] private static extern bool MiniDumpWriteDump(
        IntPtr process, uint id, IntPtr file, uint type, IntPtr exception, IntPtr streams, IntPtr callback);

    public static int Probe(uint id) {
        IntPtr main = IntPtr.Zero;
        EnumWindows((window, parameter) => {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == id && GetProp(window, "WHAT.MainWindow") != IntPtr.Zero) {
                main = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        if (main == IntPtr.Zero) return -1; // Starting or closing, not evidence of a hang.
        UIntPtr result;
        // WM_NULL checks the GUI thread even when WHAT is hidden in the tray.
        return SendMessageTimeout(main, 0, UIntPtr.Zero, IntPtr.Zero, 0x23, 1000, out result) != IntPtr.Zero ? 1 : 0;
    }

    public static void Capture(uint id, string path) {
        IntPtr process = OpenProcess(0x450, false, id); // Query, read memory and duplicate handles.
        if (process == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
        string partial = path + ".partial";
        bool created = false;
        try {
            using (var file = new FileStream(partial, FileMode.CreateNew, FileAccess.Write, FileShare.Read)) {
                created = true;
                // Thread stacks, thread metadata and unloaded modules; no full-memory dump.
                if (!MiniDumpWriteDump(process, id, file.SafeFileHandle.DangerousGetHandle(),
                                       0x1020, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero))
                    throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            File.Move(partial, path);
        } finally {
            try {
                if (created && File.Exists(partial)) File.Delete(partial);
            } finally {
                CloseHandle(process);
            }
        }
    }
}
'@
    }

    Write-Output "Watching WHAT PID $ProcessId for up to $Minutes minutes. Log: $log"
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $unresponsiveSince = $null
    while ($clock.Elapsed.TotalMinutes -lt $Minutes) {
        $appProcess.Refresh()
        if ($appProcess.HasExited) {
            Add-Content -LiteralPath $log -Value 'Process exited; capture stopped.'
            return
        }
        $ui = [WhatHangCapture]::Probe([uint32]$ProcessId)
        $line = '{0},{1},{2},{3},{4},{5},{6}' -f [DateTime]::UtcNow.ToString('o'), $ui,
            [WhatHangCapture]::GetGuiResources($appProcess.Handle, 0),
            [WhatHangCapture]::GetGuiResources($appProcess.Handle, 1),
            $appProcess.HandleCount, $appProcess.WorkingSet64,
            $appProcess.TotalProcessorTime.TotalSeconds.ToString('F3', [Globalization.CultureInfo]::InvariantCulture)
        Add-Content -LiteralPath $log -Value $line
        if ($ui -eq 0) {
            if ($null -eq $unresponsiveSince) { $unresponsiveSince = $clock.Elapsed.TotalSeconds }
            if ($clock.Elapsed.TotalSeconds - $unresponsiveSince -ge 10) {
                $dump = Join-Path $output "$captureId.dmp"
                [WhatHangCapture]::Capture([uint32]$ProcessId, $dump)
                Add-Content -LiteralPath $log -Value "Hang captured: $dump"
                Write-Output "Hang captured: $dump. WHAT has been left running."
                return
            }
        } else {
            $unresponsiveSince = $null
        }
        Start-Sleep -Seconds 2
    }
    Add-Content -LiteralPath $log -Value 'Monitoring time elapsed without a sustained UI hang.'
} catch {
    if ($log) { Add-Content -LiteralPath $log -Value "Capture stopped: $($_.Exception.Message)" }
    throw
} finally {
    $appProcess.Dispose()
}
