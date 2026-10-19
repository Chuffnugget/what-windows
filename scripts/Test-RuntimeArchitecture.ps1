[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Runtime,
    [Parameter(Mandatory)][ValidateSet('x64', 'arm64')][string]$Architecture
)
$ErrorActionPreference = 'Stop'
$expected = if ($Architecture -eq 'arm64') { @(0xAA64, 0xA64E) } else { @(0x8664) }
# Inspect PE headers without executing code. ARM64X CRT binaries are native
# ARM64-compatible; x64 vendor DLLs must never sneak into this distribution.
foreach ($file in (Get-ChildItem -LiteralPath $Runtime -Recurse -File | Where-Object Extension -in @('.exe', '.dll'))) {
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead($file.FullName))
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "Not a Windows executable: $($file.FullName)" }
        $reader.BaseStream.Position = 60
        $offset = $reader.ReadInt32()
        if ($offset -lt 64 -or $offset -gt $reader.BaseStream.Length - 6) { throw "Invalid PE header: $($file.FullName)" }
        $reader.BaseStream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550) { throw "Invalid PE signature: $($file.FullName)" }
        $machine = $reader.ReadUInt16()
        if ($machine -notin $expected) {
            throw ('Wrong architecture for {0}: {1} has PE machine 0x{2:X4}' -f $Architecture, $file.FullName, $machine)
        }
    } finally { $reader.Dispose() }
}
Write-Host "Runtime architecture verified: $Architecture"
