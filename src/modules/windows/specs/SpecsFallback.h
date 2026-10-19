#pragma once
#include <windows.h>
#include <QSettings>
#include <QStorageInfo>
#include <QSysInfo>
#include <QSet>
#include <QtEndian>

inline QString nativeSpecifications() {
    auto size = [](quint64 n) {
        return QString::number(double(n) / (n >= (1ULL << 40) ? double(1ULL << 40) : double(1ULL << 30)), 'f',
                               2) +
               (n >= (1ULL << 40) ? " TiB" : " GiB");
    };
    QSettings bios("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\BIOS", QSettings::NativeFormat);
    QSettings cpu("HKEY_LOCAL_MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                  QSettings::NativeFormat);
    auto value = [](QSettings &s, const char *key) { return s.value(key, "Unknown").toString().trimmed(); };
    QStringList lines{"PC specifications",
                      "",
                      "System: " + value(bios, "SystemManufacturer") + " " + value(bios, "SystemProductName"),
                      "Motherboard: " + value(bios, "BaseBoardManufacturer") + " " +
                          value(bios, "BaseBoardProduct"),
                      "BIOS: " + value(bios, "BIOSVendor") + " " + value(bios, "BIOSVersion"),
                      "",
                      "CPU",
                      value(cpu, "ProcessorNameString")};
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    lines << QString("Logical processors: %1").arg(info.dwNumberOfProcessors);
    ULONGLONG ram = 0;
    if (GetPhysicallyInstalledSystemMemory(&ram))
        lines << "" << "Installed memory: " + size(ram * 1024);
    lines << "Memory modules";
    bool modules = false;
    const DWORD count = GetSystemFirmwareTable(0x52534d42, 0, nullptr, 0);
    if (count >= 8 && count < 16 * 1024 * 1024) {
        QByteArray bytes(count, 0);
        if (GetSystemFirmwareTable(0x52534d42, 0, bytes.data(), count) == count) {
            int at = 8;
            while (at + 4 <= bytes.size()) {
                const auto *p = reinterpret_cast<const unsigned char *>(bytes.constData() + at);
                const int len = p[1];
                if (len < 4 || at + len > bytes.size())
                    break;
                int end = at + len;
                QStringList strings;
                while (end + 1 < bytes.size() && !(bytes[end] == 0 && bytes[end + 1] == 0)) {
                    int stop = bytes.indexOf('\0', end);
                    if (stop < 0)
                        break;
                    strings << QString::fromLatin1(bytes.constData() + end, stop - end).trimmed();
                    end = stop + 1;
                    if (end < bytes.size() && bytes[end] == 0) {
                        --end;
                        break;
                    }
                }
                auto str = [&](int index) {
                    return index > 0 && index <= strings.size() ? strings[index - 1] : QString("Unknown");
                };
                if (p[0] == 17 && len >= 27) {
                    const quint16 raw = qFromLittleEndian<quint16>(p + 12);
                    quint64 capacity = 0;
                    if (raw == 0x7fff && len >= 32)
                        capacity = quint64(qFromLittleEndian<quint32>(p + 28) & 0x7fffffff) * 1024 * 1024;
                    else if (raw && raw != 0xffff)
                        capacity = quint64(raw & 0x7fff) * (raw & 0x8000 ? 1024 : 1024 * 1024);
                    if (capacity) {
                        lines << QString("%1 %2 | %3 | slot %4")
                                     .arg(str(p[23]), str(p[26]), size(capacity), str(p[16]));
                        modules = true;
                    }
                }
                if (p[0] == 127)
                    break;
                if (end + 2 <= at)
                    break;
                at = end + 2;
            }
        }
    }
    if (!modules)
        lines << "Module details unavailable";
    lines << "" << "Graphics";
    QSet<QString> names;
    for (DWORD i = 0;; ++i) {
        DISPLAY_DEVICEW d{};
        d.cb = sizeof(d);
        if (!EnumDisplayDevicesW(nullptr, i, &d, 0))
            break;
        const auto name = QString::fromWCharArray(d.DeviceString);
        if (!names.contains(name)) {
            lines << name;
            names.insert(name);
        }
    }
    lines << "" << "Mounted volumes (logical capacity; physical-drive details unavailable)";
    for (const auto &drive : QStorageInfo::mountedVolumes())
        if (drive.isValid() && drive.isReady() && drive.bytesTotal() > 0 &&
            !drive.rootPath().startsWith("//") && !drive.rootPath().startsWith("\\\\"))
            lines << drive.rootPath() + " | " + size(drive.bytesTotal()) + " | " +
                         QString::fromLatin1(drive.fileSystemType());
    lines << "" << "Operating system"
          << QSysInfo::prettyProductName() + " | " + QSysInfo::currentCpuArchitecture() + " | kernel " +
                 QSysInfo::kernelVersion();
    return lines.join('\n');
}
