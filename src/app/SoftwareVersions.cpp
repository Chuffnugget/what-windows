#include "SoftwareVersions.h"
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSettings>
#include <iterator>
#include <windows.h>
#include <tlhelp32.h>

namespace {
QString fileVersion(const QString &path) {
    if (path.isEmpty())
        return {};
    const auto native = path.toStdWString();
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(native.c_str(), &ignored);
    if (!size || size > 1024 * 1024)
        return {};
    QByteArray bytes(size, '\0');
    if (!GetFileVersionInfoW(native.c_str(), 0, size, bytes.data()))
        return {};
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void **>(&info), &length) ||
        length < sizeof(*info) || info->dwSignature != 0xfeef04bd)
        return {};
    return QString("%1.%2.%3.%4")
        .arg(HIWORD(info->dwFileVersionMS))
        .arg(LOWORD(info->dwFileVersionMS))
        .arg(HIWORD(info->dwFileVersionLS))
        .arg(LOWORD(info->dwFileVersionLS));
}
} // namespace

QJsonObject SoftwareVersions::installed(const QMap<QString, QString> &knownPaths) {
    // Each persistent diagnostics worker has its own cache. No GUI thread waits on a scan or mutex.
    thread_local QElapsedTimer age;
    thread_local QMap<QString, QString> previousPaths;
    thread_local QJsonObject cached;
    if (age.isValid() && age.elapsed() < 60000 && previousPaths == knownPaths)
        return cached;
    QMap<QString, QString> paths = knownPaths;
    const QMap<QString, QString> processes{{"beacn.exe", "BEACN app"},
                                           {"core temp.exe", "Core Temp app"},
                                           {"coretemp.exe", "Core Temp app"},
                                           {"hwinfo64.exe", "HWiNFO app"},
                                           {"hwinfo32.exe", "HWiNFO app"},
                                           {"icue.exe", "iCUE app"},
                                           {"wallpaper32.exe", "Wallpaper Engine app"},
                                           {"wallpaper64.exe", "Wallpaper Engine app"}};
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (bool more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
            const auto label = processes.value(QString::fromWCharArray(entry.szExeFile).toLower());
            if (label.isEmpty())
                continue;
            const auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, entry.th32ProcessID);
            if (!process)
                continue;
            wchar_t path[32768];
            DWORD length = DWORD(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &length))
                paths[label] = QString::fromWCharArray(path, length);
            CloseHandle(process);
        }
        CloseHandle(snapshot);
    }
    QJsonObject result;
    for (auto it = paths.cbegin(); it != paths.cend(); ++it) {
        const auto version = fileVersion(it.value());
        if (!version.isEmpty())
            result[it.key()] = version;
    }
    // Closed applications may still publish their installed version in the uninstall registry.
    const QMap<QString, QString> products{{"BEACN", "BEACN app"},
                                          {"Core Temp", "Core Temp app"},
                                          {"HWiNFO", "HWiNFO app"},
                                          {"CORSAIR iCUE", "iCUE app"},
                                          {"Wallpaper Engine", "Wallpaper Engine app"}};
    const QRegularExpression numericVersion("^[0-9]+(?:\\.[0-9]+){1,3}$");
    for (const auto &root :
         {"HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
          "HKEY_LOCAL_MACHINE\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
          "HKEY_LOCAL_MACHINE\\Software\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall"}) {
        QSettings registry(QString::fromLatin1(root), QSettings::NativeFormat);
        for (const auto &key : registry.childGroups()) {
            const auto name = registry.value(key + "/DisplayName").toString();
            for (auto product = products.cbegin(); product != products.cend(); ++product) {
                if (!name.startsWith(product.key(), Qt::CaseInsensitive) || result.contains(product.value()))
                    continue;
                const auto version = registry.value(key + "/DisplayVersion").toString();
                if (numericVersion.match(version).hasMatch())
                    result[product.value()] = version;
            }
        }
    }
    previousPaths = knownPaths;
    cached = result;
    age.restart();
    return result;
}
