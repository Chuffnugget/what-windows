# Deterministic CIM fixtures for the specification collector. Extra private fields
# ensure the report only emits its explicit list of approved properties.
function Get-CimInstance($ClassName, $Namespace, $OperationTimeoutSec) {
    if ($script:unavailable) { throw 'Fixture query denied' }
    $private = 'PRIVATE-IDENTIFIER-DO-NOT-PRINT'
    switch ($ClassName) {
        'Win32_ComputerSystem' { [pscustomobject]@{Manufacturer='Board Co'; Model='Tower'; TotalPhysicalMemory=32GB; UserName=$private; Name=$private} }
        'Win32_BaseBoard' { throw 'Fixture query denied' }
        'Win32_BIOS' { [pscustomobject]@{Manufacturer='Firmware Co'; SMBIOSBIOSVersion='1.2'; ReleaseDate=[datetime]'2026-01-02'; SerialNumber=$private} }
        'Win32_Processor' { [pscustomobject]@{Name='Test CPU'; NumberOfCores=8; NumberOfLogicalProcessors=16; ProcessorId=$private} }
        'Win32_PhysicalMemoryArray' { [pscustomobject]@{Use=3; MemoryDevices=4} }
        'Win32_PhysicalMemory' {
            foreach ($slot in @('DIMM A2', 'DIMM B2')) {
                [pscustomobject]@{Manufacturer='Memory Co'; PartNumber='RAM-KIT'; DeviceLocator=$slot; Capacity=16GB; SMBIOSMemoryType=34; ConfiguredClockSpeed=6800; SerialNumber=$private}
            }
        }
        'MSFT_PhysicalDisk' { [pscustomobject]@{Model='Test NVMe'; Size=8TB; MediaType=4; BusType=17; FirmwareVersion='FW1'; SerialNumber=$private; FriendlyName=$private} }
        'Win32_OperatingSystem' { [pscustomobject]@{Caption='Windows 11 Pro'; OSArchitecture='64-bit'; BuildNumber='26100'; RegisteredUser=$private} }
        'MSFT_NetAdapter' {
            [pscustomobject]@{HardwareInterface=$true; InterfaceDescription='Test Ethernet'; InterfaceType=6; MediaConnectState=1; ReceiveLinkSpeed=2500000000; TransmitLinkSpeed=2500000000; NetworkAddresses=$private; Name=$private}
            [pscustomobject]@{HardwareInterface=$true; InterfaceDescription='Test Wi-Fi'; InterfaceType=71; MediaConnectState=2; ReceiveLinkSpeed=99000000000; TransmitLinkSpeed=99000000000; PermanentAddress=$private}
            [pscustomobject]@{HardwareInterface=$false; InterfaceDescription=$private; InterfaceType=6; MediaConnectState=1}
        }
        'Win32_Battery' { if (!$script:desktop) { [pscustomobject]@{Name='Pack'; DeviceID=$private} } }
        'BatteryStaticData' { [pscustomobject]@{ManufactureName='Battery Co'; DeviceName='Pack'; DesignedCapacity=50000; InstanceName=$private} }
        'BatteryFullChargedCapacity' { [pscustomobject]@{FullChargedCapacity=45000; InstanceName=$private} }
        default { throw "Unexpected fixture query: $ClassName" }
    }
}
function Get-ItemProperty($Path) {
    [pscustomobject]@{DisplayVersion='24H2'; UBR=1234; ProductId='PRIVATE-IDENTIFIER-DO-NOT-PRINT'}
}
