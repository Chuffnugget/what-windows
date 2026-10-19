# Run deliberately from an Administrator PowerShell if Windows did not offer
# its normal Private-network firewall prompt. This script is never auto-run.
param(
    [Parameter(Mandatory)][string]$Executable,
    [ValidateRange(1024,65535)][int]$Port=48765,
    [switch]$Refresh
)
$ErrorActionPreference = 'Stop'
$taskExe = (Resolve-Path -LiteralPath $Executable).Path
if ([IO.Path]::GetFileName($taskExe) -ine 'WHAT.exe' -or !(Test-Path -LiteralPath $taskExe -PathType Leaf)) {
    throw 'Specify the full path to the WHAT.exe you want to allow.'
}
$taskName = 'WHAT-Private-TCP'
$taskExisting = Get-NetFirewallRule -Name $taskName -ErrorAction SilentlyContinue
if ($taskExisting) {
    if (!$Refresh) { throw 'The WHAT rule already exists. Use -Refresh with the current WHAT.exe path to update it after an installation or port change.' }
    if ($taskExisting.DisplayName -ne 'WHAT - private local network') {
        throw 'An unrelated rule uses this name. Review it in Windows Firewall; this script will not replace it.'
    }
    Set-NetFirewallRule -Name $taskName -Program $taskExe -LocalPort $Port -Direction Inbound -Action Allow -Protocol TCP -Profile Private -RemoteAddress LocalSubnet
} else {
    New-NetFirewallRule -Name $taskName -DisplayName 'WHAT - private local network' -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port -Program $taskExe -Profile Private -RemoteAddress LocalSubnet
}
Write-Output 'Allowed WHAT from the local subnet on Private networks only. Other subnets require an explicit firewall rule for your Home Assistant server.'
