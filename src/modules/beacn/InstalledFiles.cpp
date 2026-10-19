#include "InstalledFiles.h"
#include <QFile>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QSettings>
#include <QCryptographicHash>
#include <cstring>
#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#endif
namespace {
const QByteArray appHash("5a25d53f418fcfe613e89422151fdc426668977245f69565240f44d7dfc6aee7");
const QByteArray driverHash("d0851c4508cc7aa48e112594c49ef3771652766bad8440f5ecc269c491504e7f");

bool compatibleAppLayout(const QString &path, QString *reason) {
    const auto fail = [reason](const QString &message) {
        if (reason)
            *reason = message;
        return false;
    };
#ifdef Q_OS_WIN
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail("The executable could not be read.");
    const auto readAt = [&file](qint64 offset, qint64 size) {
        if (offset < 0 || size < 0 || offset > file.size() - size || !file.seek(offset))
            return QByteArray{};
        return file.read(size);
    };
    IMAGE_DOS_HEADER dos{};
    auto bytes = readAt(0, sizeof(dos));
    if (bytes.size() != sizeof(dos))
        return fail("The executable has an incomplete header.");
    std::memcpy(&dos, bytes.constData(), sizeof(dos));
    IMAGE_NT_HEADERS64 pe{};
    bytes = readAt(dos.e_lfanew, sizeof(pe));
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || bytes.size() != sizeof(pe))
        return fail("The executable is not a valid Windows application.");
    std::memcpy(&pe, bytes.constData(), sizeof(pe));
    if (pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        pe.OptionalHeader.ImageBase != 0x140000000ULL || pe.FileHeader.NumberOfSections > 32 ||
        pe.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
        return fail("The executable does not match BEACN's supported x64 layout.");
    QList<IMAGE_SECTION_HEADER> sections;
    const qint64 sectionOffset =
        qint64(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + pe.FileHeader.SizeOfOptionalHeader;
    for (int i = 0; i < pe.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        bytes = readAt(sectionOffset + i * sizeof(section), sizeof(section));
        if (bytes.size() != sizeof(section))
            return fail("The executable's section table is incomplete.");
        std::memcpy(&section, bytes.constData(), sizeof(section));
        sections.append(section);
    }
    const auto imageBytes = [&sections, &readAt](quintptr rva, qint64 size) {
        for (const auto &section : sections) {
            const auto relative = qint64(rva) - section.VirtualAddress;
            if (relative >= 0 && relative + size <= section.SizeOfRawData)
                return readAt(qint64(section.PointerToRawData) + relative, size);
        }
        return QByteArray{};
    };
    // Try unrecognised builds without dispatching guessed function addresses.
    // Resource/signing changes can be compatible; changed executable code needs
    // a new bridge. Include all read-only data because native callbacks can
    // follow tables beyond the object that WHAT validates directly.
    bytes = imageBytes(0x1000, 0x1589600);
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() !=
        "efa399ae10d8c37b49c8f00d9675faab098f91dfa571e51371410ee853d2fa16")
        return fail("Native code differs from the validated 1.4.2.12 interface.");
    bytes = imageBytes(0x158b000, 0x228ba00);
    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() !=
        "9624f6e26ed085051e1339b9fe885e6f012b149f673dbfb8a47d868778f1f06d")
        return fail("Native data tables differ from the validated 1.4.2.12 interface.");
    return true;
#else
    return fail("The BEACN native interface requires Windows.");
#endif
}

QByteArray digest(const QString &path) {
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result().toHex() : QByteArray{};
}

void add(QStringList &list, const QString &path) {
    if (path.isEmpty())
        return;
    const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(path));
    for (const auto &item : list)
        if (item.compare(clean, Qt::CaseInsensitive) == 0)
            return;
    list.append(clean);
}
} // namespace

bool verifiedDriverFile(const QString &path) {
    return digest(path) == driverHash;
}

bool compatibleBeacnAppFile(const QString &path, QString *reason) {
    if (reason)
        reason->clear();
    return digest(path) == appHash || compatibleAppLayout(path, reason);
}

QString beacnFileVersion(const QString &path) {
#ifdef Q_OS_WIN
    const auto nativePath = path.toStdWString();
    DWORD ignored = 0;
    const auto size = GetFileVersionInfoSizeW(nativePath.c_str(), &ignored);
    if (!size || size > 1024 * 1024)
        return {};
    QByteArray data(size, '\0');
    if (!GetFileVersionInfoW(nativePath.c_str(), 0, size, data.data()))
        return {};
    VS_FIXEDFILEINFO *info = nullptr;
    UINT length = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void **>(&info), &length) &&
        length >= sizeof(VS_FIXEDFILEINFO) && info->dwSignature == 0xfeef04bd) {
        return QString("%1.%2.%3.%4")
            .arg(HIWORD(info->dwFileVersionMS))
            .arg(LOWORD(info->dwFileVersionMS))
            .arg(HIWORD(info->dwFileVersionLS))
            .arg(LOWORD(info->dwFileVersionLS));
    }
#endif
    return {};
}

QVariantMap discoverInstalledFiles(QString appPath, QString driverPath, bool search) {
    QStringList apps, drivers, roots;
    add(apps, appPath);
    add(drivers, driverPath);
    if (search) {
#ifdef Q_OS_WIN
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (snapshot != INVALID_HANDLE_VALUE) {
            if (Process32FirstW(snapshot, &entry))
                do {
                    if (_wcsicmp(entry.szExeFile, L"BEACN.exe"))
                        continue;
                    HANDLE process =
                        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
                    if (process) {
                        wchar_t name[32768];
                        DWORD n = 32768;
                        if (QueryFullProcessImageNameW(process, 0, name, &n))
                            add(apps, QString::fromWCharArray(name, n));
                        CloseHandle(process);
                    }
                } while (Process32NextW(snapshot, &entry));
            CloseHandle(snapshot);
        }
        for (const auto &key :
             {"HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
              "HKEY_LOCAL_MACHINE\\SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
              "HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"}) {
            QSettings registry(QString::fromLatin1(key), QSettings::NativeFormat);
            for (const auto &group : registry.childGroups()) {
                registry.beginGroup(group);
                if (registry.value("DisplayName").toString().contains("BEACN", Qt::CaseInsensitive))
                    add(roots, registry.value("InstallLocation").toString());
                registry.endGroup();
            }
        }
#endif
        for (const auto &env : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
            const auto folder = qEnvironmentVariable(env);
            if (!folder.isEmpty())
                add(roots, folder + "/BEACN");
        }
        for (const auto &path : apps + drivers)
            if (QFileInfo::exists(path)) {
                QDir folder = QFileInfo(path).absoluteDir();
                add(roots, folder.absolutePath());
                if (folder.cdUp() && folder.dirName().contains("BEACN", Qt::CaseInsensitive))
                    add(roots, folder.absolutePath());
            }
        for (const auto &root : roots) {
            // Search only identified installation directories; never walk an entire drive.
            QDir directory(root);
            if (!directory.exists() || directory.isRoot())
                continue;
            QDirIterator files(root, {"BEACN.exe", "BEACNUsbAudioapi_x64.dll"}, QDir::Files,
                               QDirIterator::Subdirectories);
            int count = 0;
            while (files.hasNext() && ++count <= 4096) {
                const auto path = files.next();
                if (QFileInfo(path).fileName().compare("BEACN.exe", Qt::CaseInsensitive) == 0)
                    add(apps, path);
                else
                    add(drivers, path);
            }
        }
    }
    auto pick = [search](const QStringList &paths, const QByteArray &expected, bool preferFirst) {
        QVariantMap result{{"path", paths.value(0)},
                           {"valid", false},
                           {"sha256", QString()},
                           {"exists", false},
                           {"version", QString()}};
        for (const auto &path : paths) {
            const auto hash = digest(path);
            const QVariantMap candidate{{"path", path},
                                        {"valid", hash == expected},
                                        {"sha256", QString::fromLatin1(hash)},
                                        {"exists", QFileInfo(path).isFile()},
                                        {"version", beacnFileVersion(path)}};
            if (path == paths.value(0))
                result = candidate;
            if (hash == expected || (preferFirst && candidate["exists"].toBool())) {
                result = candidate;
                break;
            }
            if (!search)
                break;
        }
        return result;
    };
    // Prefer the running app even when it is newer than the bridge. Selecting an
    // older installation elsewhere would report the wrong version and process.
    auto app = pick(apps, appHash, true);
    const auto driver = pick(drivers, driverHash, false);
    const bool appVerified = app["valid"].toBool();
    QString compatibilityError;
    if (!appVerified && app["exists"].toBool())
        app["valid"] = compatibleAppLayout(app["path"].toString(), &compatibilityError);
    QStringList problems;
    const auto describe = [&problems](const QVariantMap &file, const QString &label) {
        if (file["valid"].toBool())
            return;
        if (!file["exists"].toBool())
            problems.append(label + " was not found. Use Auto-detect or choose its installed file.");
        else if (file["sha256"].toString().isEmpty())
            problems.append(label + " could not be read. Check file access and retry.");
        else {
            const auto version = file["version"].toString();
            problems.append(label + (version.isEmpty() ? QString() : " " + version) +
                            " is not supported by this WHAT bridge.");
        }
    };
    describe(app, "BEACN app");
    describe(driver, "BEACN driver API");
    if (!app["valid"].toBool() && !app["sha256"].toString().isEmpty())
        problems.append("Compatibility check failed: " + compatibilityError +
                        " Supported interface: BEACN 1.4.2.12. Update WHAT for this build; "
                        "driver readings remain independent of app controls.");
    return {{"appPath", app["path"]},
            {"driverPath", driver["path"]},
            {"appValid", app["valid"]},
            {"appVerified", appVerified},
            {"driverValid", driver["valid"]},
            {"appSha256", app["sha256"]},
            {"driverSha256", driver["sha256"]},
            {"appVersion", app["version"]},
            {"driverVersion", driver["version"]},
            {"errors", problems},
            {"appCandidates", apps},
            {"driverCandidates", drivers},
            {"status",
             !problems.isEmpty() ? problems.join('\n')
             : appVerified
                 ? "BEACN application and driver verified."
                 : "Unrecognised BEACN build. Native compatibility checks passed; checking the live mixer."}};
}

QString prepareBeacnHook(const QString &source, const QString &directory, QString *error) {
    if (error)
        error->clear();
    auto fail = [&](const QString &message) {
        if (error)
            *error = message;
        return QString{};
    };
    QFile input(source);
    if (!input.open(QIODevice::ReadOnly) || input.size() <= 0 || input.size() > 16 * 1024 * 1024)
        return fail("The bundled BEACN bridge is missing or unreadable. Reinstall WHAT.");
    const auto bytes = input.readAll();
    if (bytes.size() != input.size())
        return fail("Could not read the bundled BEACN bridge.");
    const auto expected = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    const auto folder = directory + "/" + QString::fromLatin1(expected.toHex());
    const auto destination = folder + "/beacn-volume-hook.dll";
    if (!QDir().mkpath(folder))
        return fail("Cannot create the per-user BEACN bridge directory.");
    if (digest(destination) != expected.toHex()) {
        QSaveFile output(destination);
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit())
            return fail(
                "Cannot prepare the per-user BEACN bridge. Check file permissions or security software.");
    }
    if (digest(destination) != expected.toHex())
        return fail("BEACN bridge verification failed. No command sent.");
    return destination;
}
