[CmdletBinding()]
param([Parameter(Mandatory)][string]$Executable)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$targetPath = (Resolve-Path -LiteralPath $Executable).Path
if ([IO.Path]::GetFileName($targetPath) -ine 'WHAT.exe') {
    throw 'Specify the WHAT.exe that needs to be rebuilt.'
}

if (-not ('WhatBuildShutdown' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WhatBuildShutdown {
    private delegate bool WindowCallback(IntPtr window, IntPtr parameter);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr GetProp(IntPtr window, string name);
    [DllImport("user32.dll")] private static extern bool PostThreadMessage(uint thread, uint message, UIntPtr wParam, IntPtr lParam);

    public static bool RequestQuit(uint processId) {
        bool requested = false;
        EnumWindows((window, parameter) => {
            uint owner;
            uint thread = GetWindowThreadProcessId(window, out owner);
            if (owner == processId && GetProp(window, "WHAT.MainWindow") != IntPtr.Zero) {
                // Qt handles WM_QUIT through its normal application shutdown.
                requested = PostThreadMessage(thread, 0x0012, UIntPtr.Zero, IntPtr.Zero);
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return requested;
    }
}
'@
}

foreach ($appProcess in @(Get-Process -Name WHAT -ErrorAction SilentlyContinue)) {
    try {
        # Match the complete executable path, including when several builds exist.
        if ($appProcess.Path -ine $targetPath) { continue }
        $null = $appProcess.Handle
        if ($appProcess.HasExited) { continue }
        Write-Output "Closing WHAT process $($appProcess.Id): $targetPath"
        $requested = [WhatBuildShutdown]::RequestQuit($appProcess.Id)
        if ($requested -and $appProcess.WaitForExit(15000)) { continue }
        if (!$appProcess.HasExited) {
            Write-Warning 'WHAT did not finish shutting down; stopping this build process.'
            $appProcess.Kill()
            if (!$appProcess.WaitForExit(5000)) { throw 'WHAT is still running.' }
        }
    } finally {
        $appProcess.Dispose()
    }
}
