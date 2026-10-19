# Read-only inventory. Output is deliberately limited to hardware specifications.
# Do not dump CIM objects: they also contain serial numbers and network addresses.
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
$ErrorActionPreference = 'Stop'
$script:missing = [System.Collections.Generic.List[string]]::new()
$script:queryIssues = [System.Collections.Generic.List[string]]::new()

function Query($class, $namespace = 'root/cimv2') {
    try {
        @(Get-CimInstance -ClassName $class -Namespace $namespace -OperationTimeoutSec 3)
    } catch {
        $script:missing.Add($class)
        # Error codes identify provider failures without exposing CIM object data.
        $script:queryIssues.Add(('Unavailable Windows query: {0}/{1} (CIM {2}; HRESULT 0x{3:X8})' -f
            $namespace, $class, $_.Exception.StatusCode, $_.Exception.HResult))
        @()
    }
}
function Text($value) {
    if ([string]::IsNullOrWhiteSpace([string]$value)) { 'Unavailable' }
    else { ([string]$value).Trim() }
}
function Size($bytes) {
    if (!$bytes) { 'Unavailable' }
    elseif ($bytes -ge 1TB) { '{0:N2} TiB' -f ($bytes / 1TB) }
    else { '{0:N2} GiB' -f ($bytes / 1GB) }
}
function Capacity($value, $relative = $false) {
    if (!$value -or $value -eq [uint32]::MaxValue) { 'Unavailable' }
    elseif ($relative) { '{0:N0} relative units' -f $value }
    else { '{0:N0} mWh' -f $value }
}
function LinkSpeed($value) {
    if (!$value) { 'Unavailable' }
    elseif ($value -ge 1000000000) { '{0:0.##} Gbit/s' -f ($value / 1000000000) }
    else { '{0:0.##} Mbit/s' -f ($value / 1000000) }
}

'PC specifications'
''
$system = @(Query 'Win32_ComputerSystem')
if (!$system) { 'System: unavailable' }
foreach ($s in $system) {
    'System: {0} {1}' -f (Text $s.Manufacturer), (Text $s.Model)
    'Physical memory available to Windows: {0}' -f (Size $s.TotalPhysicalMemory)
}
$board = @(Query 'Win32_BaseBoard')
if (!$board) { 'Motherboard: unavailable' }
foreach ($b in $board) { 'Motherboard: {0} {1}' -f (Text $b.Manufacturer), (Text $b.Product) }
$bios = @(Query 'Win32_BIOS')
if (!$bios) { 'BIOS: unavailable' }
foreach ($b in $bios) {
    $date = if ($b.ReleaseDate) { $b.ReleaseDate.ToString('yyyy-MM-dd') } else { 'Unavailable' }
    'BIOS: {0} {1} | released {2}' -f (Text $b.Manufacturer), (Text $b.SMBIOSBIOSVersion), $date
}

''; 'CPU'
$cpus = @(Query 'Win32_Processor')
if (!$cpus) { 'Unavailable' }
foreach ($c in $cpus) {
    '{0} | {1} cores / {2} threads' -f (Text $c.Name), $c.NumberOfCores, $c.NumberOfLogicalProcessors
}

''; 'Memory modules'
$memory = @(Query 'Win32_PhysicalMemory')
$arrays = @(Query 'Win32_PhysicalMemoryArray' | Where-Object Use -eq 3)
$slots = ($arrays | Measure-Object MemoryDevices -Sum).Sum
if (!$memory) { 'Unavailable' }
elseif ($slots -ge $memory.Count) {
    '{0} of {1} slots populated, {2} available (firmware-reported)' -f $memory.Count, $slots, ($slots - $memory.Count)
} else { 'Slot count: unavailable' }
$memoryTypes = @{20='DDR'; 21='DDR2'; 22='DDR2 FB-DIMM'; 24='DDR3'; 26='DDR4'; 27='LPDDR'; 28='LPDDR2'; 29='LPDDR3'; 30='LPDDR4'; 34='DDR5'; 35='LPDDR5'}
foreach ($m in $memory) {
    $kind = $memoryTypes[[int]$m.SMBIOSMemoryType]
    if (!$kind) { $kind = 'Type unavailable' }
    # This is the configured transfer rate from SMBIOS, not the DRAM oscillator clock.
    $speed = if ($m.ConfiguredClockSpeed -gt 0) { '{0} MT/s configured' -f $m.ConfiguredClockSpeed } else { 'Speed unavailable' }
    '{0}: {1} {2} | {3} | {4} | {5}' -f (Text $m.DeviceLocator), (Text $m.Manufacturer), (Text $m.PartNumber), (Size $m.Capacity), $kind, $speed
}

''; 'Physical drives (includes connected removable drives)'
$disks = @(Query 'MSFT_PhysicalDisk' 'root/Microsoft/Windows/Storage')
$busTypes = @{1='SCSI'; 2='ATAPI'; 3='ATA'; 4='IEEE 1394'; 6='Fibre Channel'; 7='USB'; 8='RAID'; 9='iSCSI'; 10='SAS'; 11='SATA'; 12='SD'; 13='MMC'; 15='File-backed virtual'; 16='Storage Spaces'; 17='NVMe'; 18='SCM'; 19='UFS'}
$mediaTypes = @{3='HDD'; 4='SSD'; 5='SCM'}
foreach ($d in $disks) {
    $media = $mediaTypes[[int]$d.MediaType]
    $bus = $busTypes[[int]$d.BusType]
    if (!$media) { $media = 'Media type unavailable' }
    if (!$bus) { $bus = 'Bus unavailable' }
    '{0} | {1} | {2} | {3} | firmware {4}' -f (Text $d.Model), (Size $d.Size), $media, $bus, (Text $d.FirmwareVersion)
}
if (!$disks) {
    $legacyDisks = @(Query 'Win32_DiskDrive')
    if (!$legacyDisks) { 'Unavailable' }
    foreach ($d in $legacyDisks) {
        '{0} | {1} | {2} (Windows interface) | media type unavailable | firmware {3}' -f (Text $d.Model), (Size $d.Size), (Text $d.InterfaceType), (Text $d.FirmwareRevision)
    }
}

''; 'Operating system'
$os = @(Query 'Win32_OperatingSystem')
try { $version = Get-ItemProperty 'HKLM:/SOFTWARE/Microsoft/Windows NT/CurrentVersion' }
catch { $version = $null }
if (!$os) { 'Unavailable' }
foreach ($o in $os) {
    $build = if ($null -ne $version.UBR) { '{0}.{1}' -f $o.BuildNumber, $version.UBR } else { '{0} (revision unavailable)' -f $o.BuildNumber }
    '{0} | {1} | feature version {2} | build {3}' -f (Text $o.Caption), (Text $o.OSArchitecture), (Text $version.DisplayVersion), $build
}

''; 'Network adapters'
$adapters = @(Query 'MSFT_NetAdapter' 'root/StandardCimv2' | Where-Object HardwareInterface)
if (!$adapters) {
    if ($script:missing.Contains('MSFT_NetAdapter')) { 'Unavailable' }
    else { 'No physical network adapters reported by Windows.' }
}
foreach ($a in $adapters) {
    $type = switch ($a.InterfaceType) { 6 { 'Ethernet' }; 71 { 'Wi-Fi' }; default { 'Other adapter' } }
    $link = switch ($a.MediaConnectState) {
        1 { 'Connected | receive {0}, transmit {1}' -f (LinkSpeed $a.ReceiveLinkSpeed), (LinkSpeed $a.TransmitLinkSpeed) }
        2 { 'Disconnected' }
        default { 'Link status unavailable' }
    }
    '{0} | {1} | {2}' -f (Text $a.InterfaceDescription), $type, $link
}

# Match battery instances internally, but never include their IDs in the report.
$installedBatteries = @(Query 'Win32_Battery')
$batteries = @()
if ($installedBatteries) { $batteries = @(Query 'BatteryStaticData' 'root/wmi') }
if ($batteries) {
    ''; 'Batteries'
    $charged = @(Query 'BatteryFullChargedCapacity' 'root/wmi')
    foreach ($b in $batteries) {
        $full = $charged | Where-Object InstanceName -eq $b.InstanceName | Select-Object -First 1
        $relative = ($b.Capabilities -band 0x40000000) -ne 0
        '{0} {1} | design {2} | full charge {3}' -f (Text $b.ManufactureName), (Text $b.DeviceName), (Capacity $b.DesignedCapacity $relative), (Capacity $full.FullChargedCapacity $relative)
    }
} elseif ($installedBatteries) {
    ''; 'Batteries'
    foreach ($b in $installedBatteries) { '{0} | design/full charge capacity unavailable' -f (Text $b.Name) }
} elseif ($script:missing.Contains('Win32_Battery')) {
    ''; 'Batteries'; 'Battery details unavailable.'
}
if ($script:missing.Count) {
    ''; 'Some Windows queries were unavailable: {0}.' -f ($script:missing -join ', ')
    $script:queryIssues
}
