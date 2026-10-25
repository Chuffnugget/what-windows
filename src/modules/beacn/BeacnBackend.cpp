#include "NativeLayout.h"
#include <QCoreApplication>
#include "InstalledFiles.h"
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include "BeacnBackend.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSet>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QXmlStreamReader>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QCryptographicHash>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "GlobalBridge.h"
#include <tlhelp32.h>
#include <setupapi.h>
#include <winusb.h>
#include <objbase.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <oleauto.h>
#if __has_include(<UIAutomation.h>)
#include <UIAutomation.h>
#elif __has_include(<uiautomation.h>)
#include <uiautomation.h>
#else
#error "Windows UI Automation headers are required for the BEACN app bridge"
#endif
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <limits>

namespace {
constexpr std::uint32_t kMixerFamily = 0x4D;
constexpr std::uint32_t kInterfaceVersion = 0x2A;
constexpr std::uint32_t kDeviceOutCount = 0x64;
constexpr std::uint32_t kDeviceInCount = 0x65;
constexpr std::uint32_t kPlaybackCount = 0x66;
constexpr std::uint32_t kRecordCount = 0x67;
constexpr std::uint32_t kVirtualPlaybackCount = 0x68;
constexpr std::uint32_t kVirtualRecordCount = 0x69;
constexpr std::uint32_t kMixerWeight = 0xC8;

constexpr int kPlaybackPin = 0;
constexpr int kDeviceInputPin = 1;
constexpr int kVirtualPlaybackPin = 2;
constexpr int kRecordPin = 4;
constexpr int kDeviceOutputPin = 5;
constexpr int kVirtualRecordPin = 6;

#pragma pack(push, 1)

struct InterfaceVersionRequest {
    std::uint32_t family = kMixerFamily;
    std::uint32_t property = kInterfaceVersion;
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
};

struct CountRequest {
    std::uint32_t family = kMixerFamily;
    std::uint32_t property = 0;
    std::uint32_t count = 0;
};

struct WeightRequest {
    std::uint32_t family = kMixerFamily;
    std::uint32_t property = kMixerWeight;
    std::uint8_t inputPin = 0;
    std::uint8_t inputIndex = 0;
    std::uint8_t outputPin = 0;
    std::uint8_t outputIndex = 0;
    std::int32_t rawGain = 0;
};

#pragma pack(pop)

static_assert(sizeof(InterfaceVersionRequest) == 16);
static_assert(sizeof(CountRequest) == 12);
static_assert(sizeof(WeightRequest) == 16);

#ifdef Q_OS_WIN
struct BeacnWindowSearch {
    HWND best = nullptr;
    qint64 bestScore = std::numeric_limits<qint64>::min();
    DWORD pid = 0;
};

QString winErrorText(DWORD error);

BOOL CALLBACK findBeacnWindowProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindow(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId())
        return TRUE;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process)
        return TRUE;

    wchar_t pathBuffer[32768]{};
    DWORD pathChars = static_cast<DWORD>(std::size(pathBuffer));
    const BOOL gotPath = QueryFullProcessImageNameW(process, 0, pathBuffer, &pathChars);
    CloseHandle(process);
    if (!gotPath)
        return TRUE;

    const QString path = QString::fromWCharArray(pathBuffer, static_cast<int>(pathChars));
    if (QFileInfo(path).fileName().compare(QStringLiteral("BEACN.exe"), Qt::CaseInsensitive) != 0)
        return TRUE;

    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const qint64 width = std::max<LONG>(0, rect.right - rect.left);
    const qint64 height = std::max<LONG>(0, rect.bottom - rect.top);
    const qint64 area = width * height;
    const bool visible = IsWindowVisible(hwnd) != FALSE;
    const int titleLength = GetWindowTextLengthW(hwnd);
    // Prefer the large visible mixer window, but keep a hidden BEACN top-level
    // window as a fallback so a scan can still produce a useful diagnostic.
    const qint64 score = area + (visible ? 1000000000LL : 0LL) + (titleLength > 0 ? 1000000LL : 0LL);

    auto *search = reinterpret_cast<BeacnWindowSearch *>(lParam);
    if (score > search->bestScore) {
        search->best = hwnd;
        search->bestScore = score;
        search->pid = pid;
    }
    return TRUE;
}

DWORD findBeacnProcessId() {
    BeacnWindowSearch windowSearch;
    EnumWindows(findBeacnWindowProc, reinterpret_cast<LPARAM>(&windowSearch));
    if (windowSearch.pid != 0)
        return windowSearch.pid;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD pid = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (QString::fromWCharArray(entry.szExeFile)
                    .compare(QStringLiteral("BEACN.exe"), Qt::CaseInsensitive) == 0) {
                pid = entry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return pid;
}

struct SystemHandleTableEntryInfoEx {
    PVOID object = nullptr;
    ULONG_PTR uniqueProcessId = 0;
    ULONG_PTR handleValue = 0;
    ULONG grantedAccess = 0;
    USHORT creatorBackTraceIndex = 0;
    USHORT objectTypeIndex = 0;
    ULONG handleAttributes = 0;
    ULONG reserved = 0;
};

struct SystemHandleInformationEx {
    ULONG_PTR numberOfHandles = 0;
    ULONG_PTR reserved = 0;
    SystemHandleTableEntryInfoEx handles[1];
};

using NtQuerySystemInformationFn = LONG(NTAPI *)(ULONG, PVOID, ULONG, PULONG);
constexpr ULONG kSystemExtendedHandleInformation = 64;
constexpr LONG kStatusInfoLengthMismatch = static_cast<LONG>(0xC0000004u);
constexpr LONG kStatusBufferTooSmall = static_cast<LONG>(0xC0000023u);

HANDLE duplicateBeacnWinUsbFileHandle(decltype(&WinUsb_Initialize) pInitialize, decltype(&WinUsb_Free) pFree,
                                      QStringList *details) {
    const DWORD beacnPid = findBeacnProcessId();
    if (beacnPid == 0) {
        if (details)
            details->append(QStringLiteral(
                "Could not find a running BEACN.exe process to borrow the exclusive device handle from."));
        return INVALID_HANDLE_VALUE;
    }

    HANDLE beacnProcess =
        OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, beacnPid);
    if (!beacnProcess) {
        if (details)
            details->append(QStringLiteral("OpenProcess(PROCESS_DUP_HANDLE) for BEACN.exe pid=%1 failed: %2")
                                .arg(beacnPid)
                                .arg(winErrorText(GetLastError())));
        return INVALID_HANDLE_VALUE;
    }

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    const auto pNtQuerySystemInformation =
        ntdll
            ? reinterpret_cast<NtQuerySystemInformationFn>(GetProcAddress(ntdll, "NtQuerySystemInformation"))
            : nullptr;
    if (!pNtQuerySystemInformation) {
        if (details)
            details->append(QStringLiteral(
                "NtQuerySystemInformation is unavailable; cannot enumerate BEACN's open handles."));
        CloseHandle(beacnProcess);
        return INVALID_HANDLE_VALUE;
    }

    QByteArray storage(1024 * 1024, 0);
    ULONG required = 0;
    LONG status = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        required = 0;
        status = pNtQuerySystemInformation(kSystemExtendedHandleInformation, storage.data(),
                                           static_cast<ULONG>(storage.size()), &required);
        if (status != kStatusInfoLengthMismatch && status != kStatusBufferTooSmall)
            break;

        qsizetype nextSize = storage.size() * 2;
        if (required > static_cast<ULONG>(storage.size()))
            nextSize = std::max<qsizetype>(nextSize, static_cast<qsizetype>(required) + 65536);
        if (nextSize > 128 * 1024 * 1024)
            break;
        storage.resize(nextSize);
        storage.fill(0);
    }

    if (status < 0) {
        if (details)
            details->append(
                QStringLiteral(
                    "NtQuerySystemInformation(SystemExtendedHandleInformation) failed: NTSTATUS 0x%1")
                    .arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0')));
        CloseHandle(beacnProcess);
        return INVALID_HANDLE_VALUE;
    }

    const auto *info = reinterpret_cast<const SystemHandleInformationEx *>(storage.constData());
    const ULONG_PTR count = info->numberOfHandles;
    int duplicated = 0;
    int winUsbCandidates = 0;

    for (ULONG_PTR i = 0; i < count; ++i) {
        const auto &entry = info->handles[i];
        if (entry.uniqueProcessId != static_cast<ULONG_PTR>(beacnPid))
            continue;

        HANDLE localHandle = nullptr;
        if (!DuplicateHandle(beacnProcess, reinterpret_cast<HANDLE>(entry.handleValue), GetCurrentProcess(),
                             &localHandle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            continue;
        }
        ++duplicated;

        // Do not reject a valid WinUSB file handle based only on the default
        // interface. Mix Create can expose the command pipes on an associated
        // interface, which is enumerated later by readMixCreateParameters().
        WINUSB_INTERFACE_HANDLE usb = nullptr;
        if (!pInitialize(localHandle, &usb) || !usb) {
            CloseHandle(localHandle);
            continue;
        }
        ++winUsbCandidates;
        pFree(usb);

        if (details) {
            details->append(
                QStringLiteral(
                    "Borrowed BEACN.exe pid=%1 handle=0x%2; WinUsb_Initialize succeeded. Keeping this candidate and scanning default + associated interfaces.")
                    .arg(beacnPid)
                    .arg(static_cast<qulonglong>(entry.handleValue), 0, 16));
        }
        CloseHandle(beacnProcess);
        return localHandle;
    }

    if (details) {
        details->append(
            QStringLiteral(
                "Scanned BEACN.exe pid=%1: duplicated %2 handles, %3 initialized as WinUSB, but no usable WinUSB file handle was found.")
                .arg(beacnPid)
                .arg(duplicated)
                .arg(winUsbCandidates));
    }
    CloseHandle(beacnProcess);
    return INVALID_HANDLE_VALUE;
}

constexpr quintptr kBeacnPreferredImageBase = static_cast<quintptr>(0x140000000ull);
constexpr quintptr kBeacnMixCreateVtableVa = static_cast<quintptr>(0x141544bd8ull);
constexpr quintptr kBeacnMixCreateVtableRva = kBeacnMixCreateVtableVa - kBeacnPreferredImageBase;
constexpr quintptr kBeacnParameterManagerOffset = static_cast<quintptr>(0x2b8u);
constexpr quintptr kBeacnParameterMapHeadOffset = static_cast<quintptr>(0x08u);

bool remoteProtectionReadable(DWORD protect) {
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
        return false;
    switch (protect & 0xffu) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

template <typename T> bool readRemoteValue(HANDLE process, quintptr address, T &value) {
    SIZE_T received = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), &value, sizeof(T), &received) !=
               FALSE &&
           received == sizeof(T);
}

bool readRemoteBlock(HANDLE process, quintptr address, void *buffer, SIZE_T size) {
    SIZE_T received = 0;
    return ReadProcessMemory(process, reinterpret_cast<LPCVOID>(address), buffer, size, &received) != FALSE &&
           received == size;
}

bool findBeacnModuleBase(DWORD pid, quintptr &base, DWORD &size, QString *errorText) {
    base = 0;
    size = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snapshot == INVALID_HANDLE_VALUE) {
        if (errorText)
            *errorText = QStringLiteral("CreateToolhelp32Snapshot(MODULE) failed: %1")
                             .arg(winErrorText(GetLastError()));
        return false;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (QString::fromWCharArray(entry.szModule)
                    .compare(QStringLiteral("BEACN.exe"), Qt::CaseInsensitive) == 0) {
                base = reinterpret_cast<quintptr>(entry.modBaseAddr);
                size = entry.modBaseSize;
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    if (!found && errorText)
        *errorText = QStringLiteral("Could not locate BEACN.exe module base in pid %1.").arg(pid);
    return found;
}

quint32 remoteU32(const unsigned char *data, int offset) {
    quint32 value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

quint64 remoteU64(const unsigned char *data, int offset) {
    quint64 value = 0;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

bool readBeacnCachedParameterTable(QHash<quint32, quint32> &values, QStringList *details,
                                   QString *errorText) {
    values.clear();
    const DWORD pid = findBeacnProcessId();
    if (pid == 0) {
        if (errorText)
            *errorText = QStringLiteral("BEACN.exe is not running.");
        return false;
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process) {
        if (errorText)
            *errorText = QStringLiteral("OpenProcess(PROCESS_VM_READ) for BEACN.exe pid=%1 failed: %2")
                             .arg(pid)
                             .arg(winErrorText(GetLastError()));
        return false;
    }

    quintptr moduleBase = 0;
    DWORD moduleSize = 0;
    QString moduleError;
    if (!findBeacnModuleBase(pid, moduleBase, moduleSize, &moduleError)) {
        CloseHandle(process);
        if (errorText)
            *errorText = moduleError;
        return false;
    }

    const quintptr runtimeVtable = moduleBase + kBeacnMixCreateVtableRva;
    if (details) {
        details->append(QStringLiteral("BEACN cache: pid=%1 moduleBase=0x%2 vtable=0x%3")
                            .arg(pid)
                            .arg(static_cast<qulonglong>(moduleBase), 0, 16)
                            .arg(static_cast<qulonglong>(runtimeVtable), 0, 16));
    }

    const std::array<quint32, 11> expectedByKey = {0x000u, 0x100u, 0x400u, 0x001u, 0x101u, 0x201u,
                                                   0x301u, 0x401u, 0x501u, 0x601u, 0x701u};

    struct CachedEntry {
        quint32 key = 0;
        quint32 id = 0;
        quint32 value = 0;
        bool dirty = false;
    };

    auto readValidatedTree = [&](quintptr head, QVector<CachedEntry> &entries) -> bool {
        entries.clear();
        std::array<unsigned char, 0x30> headBytes{};
        if (!readRemoteBlock(process, head, headBytes.data(), headBytes.size()))
            return false;
        if (headBytes[0x19] == 0)
            return false;

        const quintptr root = static_cast<quintptr>(remoteU64(headBytes.data(), 0x08));
        if (root == 0 || root == head)
            return false;

        QVector<quintptr> stack{root};
        QSet<qulonglong> visited;
        bool invalid = false;
        while (!stack.isEmpty() && entries.size() <= 32) {
            const quintptr node = stack.takeLast();
            if (node == 0 || node == head)
                continue;
            const qulonglong nodeKey = static_cast<qulonglong>(node);
            if (visited.contains(nodeKey)) {
                invalid = true;
                break;
            }
            visited.insert(nodeKey);

            std::array<unsigned char, 0x30> bytes{};
            if (!readRemoteBlock(process, node, bytes.data(), bytes.size())) {
                invalid = true;
                break;
            }
            if (bytes[0x19] != 0)
                continue;

            CachedEntry entry;
            entry.key = remoteU32(bytes.data(), 0x1c);
            entry.id = remoteU32(bytes.data(), 0x20);
            entry.value = remoteU32(bytes.data(), 0x24);
            entry.dirty = bytes[0x28] != 0;
            entries.append(entry);

            const quintptr left = static_cast<quintptr>(remoteU64(bytes.data(), 0x00));
            const quintptr right = static_cast<quintptr>(remoteU64(bytes.data(), 0x10));
            if (left != 0 && left != head)
                stack.append(left);
            if (right != 0 && right != head)
                stack.append(right);
        }

        if (invalid || entries.size() != static_cast<int>(expectedByKey.size()))
            return false;

        std::sort(entries.begin(), entries.end(),
                  [](const CachedEntry &a, const CachedEntry &b) { return a.key < b.key; });
        for (int i = 0; i < entries.size(); ++i) {
            if (entries.at(i).key != static_cast<quint32>(i) ||
                entries.at(i).id != expectedByKey.at(static_cast<size_t>(i)))
                return false;
        }
        return true;
    };

    static DWORD rememberedPid = 0;
    static FILETIME rememberedCreation{};
    static quintptr rememberedHead = 0;
    FILETIME creation{}, exitTime{}, kernelTime{}, userTime{};
    const bool haveIdentity = GetProcessTimes(process, &creation, &exitTime, &kernelTime, &userTime);
    if (!haveIdentity || rememberedPid != pid || CompareFileTime(&creation, &rememberedCreation) != 0)
        rememberedHead = 0;

    auto acceptTree = [&](quintptr head, quintptr objectAddress, const QString &method) -> bool {
        QVector<CachedEntry> entries;
        if (!readValidatedTree(head, entries))
            return false;

        if (haveIdentity) {
            rememberedPid = pid;
            rememberedCreation = creation;
            rememberedHead = head;
        }
        values.clear();
        for (const CachedEntry &entry : entries)
            values.insert(entry.id, entry.value);

        if (details) {
            if (objectAddress != 0) {
                details->append(
                    QStringLiteral(
                        "BEACN cache: MixCreate object=0x%1 parameterMapHead=0x%2 validated (%3 entries, %4)")
                        .arg(static_cast<qulonglong>(objectAddress), 0, 16)
                        .arg(static_cast<qulonglong>(head), 0, 16)
                        .arg(entries.size())
                        .arg(method));
            } else {
                details->append(
                    QStringLiteral("BEACN cache: parameterMapHead=0x%1 validated directly (%2 entries, %3)")
                        .arg(static_cast<qulonglong>(head), 0, 16)
                        .arg(entries.size())
                        .arg(method));
            }
            for (const CachedEntry &entry : entries) {
                details->append(QStringLiteral("BEACN cache param key=%1 id=0x%2 value=%3 (0x%4) dirty=%5")
                                    .arg(entry.key)
                                    .arg(entry.id, 3, 16, QLatin1Char('0'))
                                    .arg(entry.value)
                                    .arg(entry.value, 8, 16, QLatin1Char('0'))
                                    .arg(entry.dirty ? QStringLiteral("1") : QStringLiteral("0")));
            }
        }
        return true;
    };

    if (rememberedHead != 0) {
        if (acceptTree(rememberedHead, 0, QStringLiteral("revalidated cached address; no heap scan"))) {
            CloseHandle(process);
            if (errorText)
                errorText->clear();
            return true;
        }
        rememberedHead = 0;
    }

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    const quintptr minAddress = reinterpret_cast<quintptr>(systemInfo.lpMinimumApplicationAddress);
    const quintptr maxAddress = reinterpret_cast<quintptr>(systemInfo.lpMaximumApplicationAddress);
    constexpr SIZE_T kChunkSize = 1024 * 1024;

    // First try the original object-vtable route. It is fast when the primary
    // vptr is present in a normal heap allocation, but it is not required.
    QVector<quintptr> objectCandidates;
    quintptr cursor = minAddress;
    while (cursor < maxAddress && objectCandidates.size() < 64) {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T queried = VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi));
        if (queried != sizeof(mbi))
            break;

        const quintptr regionBase = reinterpret_cast<quintptr>(mbi.BaseAddress);
        const SIZE_T regionSize = mbi.RegionSize;
        const bool candidateRegion = mbi.State == MEM_COMMIT && remoteProtectionReadable(mbi.Protect);

        if (candidateRegion && regionSize > 0) {
            SIZE_T offset = 0;
            QByteArray tail;
            while (offset < regionSize && objectCandidates.size() < 64) {
                const SIZE_T requestSize = std::min<SIZE_T>(kChunkSize, regionSize - offset);
                QByteArray block;
                block.resize(static_cast<qsizetype>(requestSize));
                SIZE_T received = 0;
                if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(regionBase + offset), block.data(),
                                       requestSize, &received) ||
                    received < sizeof(quint64)) {
                    offset += requestSize;
                    tail.clear();
                    continue;
                }
                block.resize(static_cast<qsizetype>(received));

                QByteArray scan = tail + block;
                const quintptr scanBase = regionBase + offset - static_cast<quintptr>(tail.size());
                for (qsizetype i = 0; i + static_cast<qsizetype>(sizeof(quint64)) <= scan.size(); ++i) {
                    const quintptr absolute = scanBase + static_cast<quintptr>(i);
                    if ((absolute & (sizeof(quintptr) - 1u)) != 0)
                        continue;
                    quint64 candidate = 0;
                    std::memcpy(&candidate, scan.constData() + i, sizeof(candidate));
                    if (candidate == static_cast<quint64>(runtimeVtable))
                        objectCandidates.append(absolute);
                }

                const qsizetype keep = std::min<qsizetype>(7, scan.size());
                tail = scan.right(keep);
                offset += requestSize;
            }
        }

        const quintptr next = regionBase + static_cast<quintptr>(regionSize);
        if (next <= cursor)
            break;
        cursor = next;
    }

    if (details)
        details->append(QStringLiteral("BEACN cache: primary-vtable scan found %1 candidate(s).")
                            .arg(objectCandidates.size()));

    for (quintptr objectAddress : objectCandidates) {
        quint64 headValue = 0;
        const quintptr headPointerAddress =
            objectAddress + kBeacnParameterManagerOffset + kBeacnParameterMapHeadOffset;
        if (!readRemoteValue(process, headPointerAddress, headValue) || headValue == 0)
            continue;
        const quintptr head = static_cast<quintptr>(headValue);
        if (acceptTree(head, objectAddress, QStringLiteral("object/vtable locator"))) {
            CloseHandle(process);
            if (errorText)
                errorText->clear();
            return true;
        }
    }

    // Robust fallback: locate the std::map itself from its most distinctive
    // node. MSVC's tree node stores key at +0x1c and the BEACN parameter id at
    // +0x20. The final expected entry is key=10/id=0x701. From that node we can
    // walk parent pointers (+0x08) until we reach the _Isnil head node, then
    // validate the complete 11-entry map before accepting anything.
    if (details)
        details->append(
            QStringLiteral("BEACN cache: trying direct parameter-map signature (key=10/id=0x701)."));

    const std::array<unsigned char, 8> signature = {0x0a, 0x00, 0x00, 0x00, 0x01, 0x07, 0x00, 0x00};
    int signatureMatches = 0;
    cursor = minAddress;
    while (cursor < maxAddress && signatureMatches < 256) {
        MEMORY_BASIC_INFORMATION mbi{};
        const SIZE_T queried = VirtualQueryEx(process, reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi));
        if (queried != sizeof(mbi))
            break;

        const quintptr regionBase = reinterpret_cast<quintptr>(mbi.BaseAddress);
        const SIZE_T regionSize = mbi.RegionSize;
        const bool candidateRegion = mbi.State == MEM_COMMIT && remoteProtectionReadable(mbi.Protect);

        if (candidateRegion && regionSize > 0) {
            SIZE_T offset = 0;
            QByteArray tail;
            while (offset < regionSize && signatureMatches < 256) {
                const SIZE_T requestSize = std::min<SIZE_T>(kChunkSize, regionSize - offset);
                QByteArray block;
                block.resize(static_cast<qsizetype>(requestSize));
                SIZE_T received = 0;
                if (!ReadProcessMemory(process, reinterpret_cast<LPCVOID>(regionBase + offset), block.data(),
                                       requestSize, &received) ||
                    received < signature.size()) {
                    offset += requestSize;
                    tail.clear();
                    continue;
                }
                block.resize(static_cast<qsizetype>(received));
                QByteArray scan = tail + block;
                const quintptr scanBase = regionBase + offset - static_cast<quintptr>(tail.size());

                for (qsizetype i = 0; i + static_cast<qsizetype>(signature.size()) <= scan.size(); ++i) {
                    if (std::memcmp(scan.constData() + i, signature.data(), signature.size()) != 0)
                        continue;
                    const quintptr keyAddress = scanBase + static_cast<quintptr>(i);
                    if (keyAddress < 0x1cu)
                        continue;
                    const quintptr node = keyAddress - 0x1cu;
                    if ((node & (sizeof(quintptr) - 1u)) != 0)
                        continue;

                    ++signatureMatches;
                    std::array<unsigned char, 0x30> nodeBytes{};
                    if (!readRemoteBlock(process, node, nodeBytes.data(), nodeBytes.size()))
                        continue;
                    if (nodeBytes[0x19] != 0 || remoteU32(nodeBytes.data(), 0x1c) != 10u ||
                        remoteU32(nodeBytes.data(), 0x20) != 0x701u)
                        continue;

                    quintptr parent = static_cast<quintptr>(remoteU64(nodeBytes.data(), 0x08));
                    QSet<qulonglong> climbed;
                    for (int depth = 0; depth < 32 && parent != 0; ++depth) {
                        const qulonglong parentKey = static_cast<qulonglong>(parent);
                        if (climbed.contains(parentKey))
                            break;
                        climbed.insert(parentKey);

                        std::array<unsigned char, 0x30> parentBytes{};
                        if (!readRemoteBlock(process, parent, parentBytes.data(), parentBytes.size()))
                            break;
                        if (parentBytes[0x19] != 0) {
                            if (acceptTree(parent, 0, QStringLiteral("direct map signature"))) {
                                if (details)
                                    details->append(
                                        QStringLiteral(
                                            "BEACN cache: signature locator succeeded after %1 candidate match(es).")
                                            .arg(signatureMatches));
                                CloseHandle(process);
                                if (errorText)
                                    errorText->clear();
                                return true;
                            }
                            break;
                        }
                        parent = static_cast<quintptr>(remoteU64(parentBytes.data(), 0x08));
                    }
                }

                const qsizetype keep =
                    std::min<qsizetype>(static_cast<qsizetype>(signature.size() - 1), scan.size());
                tail = scan.right(keep);
                offset += requestSize;
            }
        }

        const quintptr next = regionBase + static_cast<quintptr>(regionSize);
        if (next <= cursor)
            break;
        cursor = next;
    }

    CloseHandle(process);
    if (errorText) {
        *errorText =
            QStringLiteral(
                "Could not validate BEACN's 11-entry Mix Create parameter cache (vtable candidates=%1, map-signature matches=%2).")
                .arg(objectCandidates.size())
                .arg(signatureMatches);
    }
    return false;
}

QString takeBstr(BSTR value) {
    if (!value)
        return {};
    const QString result = QString::fromWCharArray(value, static_cast<int>(SysStringLen(value)));
    SysFreeString(value);
    return result;
}

QString uiaName(IUIAutomationElement *element) {
    BSTR value = nullptr;
    if (!element || FAILED(element->get_CurrentName(&value)))
        return {};
    return takeBstr(value).trimmed();
}

QString uiaAutomationId(IUIAutomationElement *element) {
    BSTR value = nullptr;
    if (!element || FAILED(element->get_CurrentAutomationId(&value)))
        return {};
    return takeBstr(value).trimmed();
}

QString uiaHelpText(IUIAutomationElement *element) {
    BSTR value = nullptr;
    if (!element || FAILED(element->get_CurrentHelpText(&value)))
        return {};
    return takeBstr(value).trimmed();
}

QString normalizeUiText(QString value) {
    return value.simplified().toCaseFolded();
}

double rectCenterX(const RECT &r) {
    return (static_cast<double>(r.left) + static_cast<double>(r.right)) * 0.5;
}

double rectCenterY(const RECT &r) {
    return (static_cast<double>(r.top) + static_cast<double>(r.bottom)) * 0.5;
}

double rectDistanceSquared(const RECT &a, const RECT &b) {
    const double dx = rectCenterX(a) - rectCenterX(b);
    const double dy = rectCenterY(a) - rectCenterY(b);
    return dx * dx + dy * dy;
}

int busFromAccessibilityText(const QString &text) {
    const QString n = normalizeUiText(text);
    if (n.contains(QStringLiteral("audience")) || n.contains(QStringLiteral("broadcast")))
        return 0; // Driver bus 0 = Audience
    if (n.contains(QStringLiteral("personal")) || n.contains(QStringLiteral("headphone")) ||
        n.contains(QStringLiteral("headphones")))
        return 1; // Driver bus 1 = Personal
    return -1;
}

QString winErrorText(DWORD error) {
    wchar_t *message = nullptr;
    const DWORD chars = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                           FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr, error, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    QString text = chars && message ? QString::fromWCharArray(message, static_cast<int>(chars)).trimmed()
                                    : QStringLiteral("Unknown Windows error");
    if (message)
        LocalFree(message);
    return QStringLiteral("%1 (Win32 %2 / 0x%3)").arg(text).arg(error).arg(error, 8, 16, QLatin1Char('0'));
}

QStringList readRegistryStrings(HKEY key, const wchar_t *valueName) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0)
        return {};
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ)
        return {};

    QByteArray storage(static_cast<int>(bytes + sizeof(wchar_t) * 2), 0);
    auto *buffer = reinterpret_cast<BYTE *>(storage.data());
    DWORD readBytes = bytes;
    if (RegQueryValueExW(key, valueName, nullptr, &type, buffer, &readBytes) != ERROR_SUCCESS)
        return {};

    const auto *wide = reinterpret_cast<const wchar_t *>(storage.constData());
    QStringList result;
    if (type == REG_MULTI_SZ) {
        const wchar_t *cursor = wide;
        const wchar_t *end = reinterpret_cast<const wchar_t *>(storage.constData() + readBytes);
        while (cursor < end && *cursor) {
            const QString value = QString::fromWCharArray(cursor).trimmed();
            if (!value.isEmpty())
                result.append(value);
            cursor += std::wcslen(cursor) + 1;
        }
    } else {
        QString value = QString::fromWCharArray(wide).trimmed();
        if (type == REG_EXPAND_SZ) {
            wchar_t expanded[32768]{};
            const DWORD needed = ExpandEnvironmentStringsW(reinterpret_cast<LPCWSTR>(value.utf16()), expanded,
                                                           static_cast<DWORD>(std::size(expanded)));
            if (needed > 0 && needed < std::size(expanded))
                value = QString::fromWCharArray(expanded).trimmed();
        }
        if (!value.isEmpty())
            result.append(value);
    }
    return result;
}

QString readRegistryString(HKEY key, const wchar_t *valueName) {
    const QStringList values = readRegistryStrings(key, valueName);
    return values.isEmpty() ? QString() : values.first();
}

QStringList discoverBeacnMixInterfaceGuids(QStringList *registryLocations) {
    HKEY usbRoot = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ, &usbRoot) !=
        ERROR_SUCCESS)
        return {};

    QStringList deviceKeys;
    for (DWORD index = 0;; ++index) {
        wchar_t name[512]{};
        DWORD chars = static_cast<DWORD>(std::size(name));
        const LONG rc = RegEnumKeyExW(usbRoot, index, name, &chars, nullptr, nullptr, nullptr, nullptr);
        if (rc == ERROR_NO_MORE_ITEMS)
            break;
        if (rc != ERROR_SUCCESS)
            continue;
        const QString deviceKey = QString::fromWCharArray(name, static_cast<int>(chars));
        if (deviceKey.contains(QStringLiteral("VID_33AE&PID_0007"), Qt::CaseInsensitive))
            deviceKeys.append(deviceKey);
    }

    std::stable_sort(deviceKeys.begin(), deviceKeys.end(), [](const QString &a, const QString &b) {
        const bool aMi03 = a.contains(QStringLiteral("MI_03"), Qt::CaseInsensitive);
        const bool bMi03 = b.contains(QStringLiteral("MI_03"), Qt::CaseInsensitive);
        return aMi03 && !bMi03;
    });

    QSet<QString> seen;
    QStringList guids;
    for (const QString &deviceKey : deviceKeys) {
        HKEY device = nullptr;
        if (RegOpenKeyExW(usbRoot, reinterpret_cast<LPCWSTR>(deviceKey.utf16()), 0, KEY_READ, &device) !=
            ERROR_SUCCESS)
            continue;
        for (DWORD instanceIndex = 0;; ++instanceIndex) {
            wchar_t instanceName[1024]{};
            DWORD chars = static_cast<DWORD>(std::size(instanceName));
            const LONG rc = RegEnumKeyExW(device, instanceIndex, instanceName, &chars, nullptr, nullptr,
                                          nullptr, nullptr);
            if (rc == ERROR_NO_MORE_ITEMS)
                break;
            if (rc != ERROR_SUCCESS)
                continue;
            HKEY instance = nullptr;
            if (RegOpenKeyExW(device, instanceName, 0, KEY_READ, &instance) != ERROR_SUCCESS)
                continue;
            const QString service = readRegistryString(instance, L"Service");
            HKEY params = nullptr;
            const LONG paramsRc = RegOpenKeyExW(instance, L"Device Parameters", 0, KEY_READ, &params);
            if (service.compare(QStringLiteral("WINUSB"), Qt::CaseInsensitive) == 0 &&
                paramsRc == ERROR_SUCCESS) {
                const wchar_t *names[] = {L"DeviceInterfaceGUID", L"DeviceInterfaceGUIDs",
                                          L"DeviceInterfaceGuid"};
                for (const wchar_t *valueName : names) {
                    for (QString guidText : readRegistryStrings(params, valueName)) {
                        guidText = guidText.trimmed();
                        if (guidText.isEmpty())
                            continue;
                        const QString folded = guidText.toCaseFolded();
                        if (!seen.contains(folded)) {
                            seen.insert(folded);
                            guids.append(guidText);
                            if (registryLocations)
                                registryLocations->append(
                                    QStringLiteral("%1\\%2\\Device Parameters")
                                        .arg(deviceKey, QString::fromWCharArray(instanceName)));
                        }
                    }
                }
            }
            if (paramsRc == ERROR_SUCCESS)
                RegCloseKey(params);
            RegCloseKey(instance);
        }
        RegCloseKey(device);
    }
    RegCloseKey(usbRoot);
    return guids;
}

struct WinUsbCandidate {
    QString path;
    QString guidText;
};

QVector<WinUsbCandidate> enumerateBeacnWinUsbPaths(const QStringList &guidTexts) {
    QVector<WinUsbCandidate> result;
    QSet<QString> seenPaths;
    for (const QString &guidText : guidTexts) {
        GUID guid{};
        if (FAILED(CLSIDFromString(reinterpret_cast<LPCOLESTR>(guidText.utf16()), &guid)))
            continue;
        HDEVINFO info = SetupDiGetClassDevsW(&guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
        if (info == INVALID_HANDLE_VALUE)
            continue;
        for (DWORD index = 0;; ++index) {
            SP_DEVICE_INTERFACE_DATA iface{};
            iface.cbSize = sizeof(iface);
            if (!SetupDiEnumDeviceInterfaces(info, nullptr, &guid, index, &iface)) {
                if (GetLastError() == ERROR_NO_MORE_ITEMS)
                    break;
                continue;
            }
            DWORD required = 0;
            SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &required, nullptr);
            if (required < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
                continue;
            QByteArray detailStorage(static_cast<int>(required), 0);
            auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(detailStorage.data());
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, required, nullptr, nullptr))
                continue;
            const QString path = QString::fromWCharArray(detail->DevicePath);
            if (!path.contains(QStringLiteral("vid_33ae&pid_0007"), Qt::CaseInsensitive))
                continue;
            const QString folded = path.toCaseFolded();
            if (!seenPaths.contains(folded)) {
                seenPaths.insert(folded);
                result.push_back({path, guidText});
            }
        }
        SetupDiDestroyDeviceInfoList(info);
    }
    return result;
}
#endif

QVariantList makeWinUsbParameterRows(const QHash<quint32, quint32> &values, const QStringList &sourceNames) {
    const std::array<quint32, 11> ordered = {0x000u, 0x100u, 0x400u, 0x001u, 0x101u, 0x201u,
                                             0x301u, 0x401u, 0x501u, 0x601u, 0x701u};
    QVariantList rows;
    for (quint32 id : ordered) {
        if (!values.contains(id))
            continue;
        QVariantMap row;
        row.insert(QStringLiteral("id"), static_cast<qulonglong>(id));
        row.insert(QStringLiteral("idHex"),
                   QStringLiteral("0x%1").arg(id, 3, 16, QLatin1Char('0')).toUpper());
        row.insert(QStringLiteral("value"), static_cast<qulonglong>(values.value(id)));
        row.insert(QStringLiteral("valueHex"),
                   QStringLiteral("0x%1").arg(values.value(id), 8, 16, QLatin1Char('0')).toUpper());
        if ((id & 0xffu) == 0x01u && (id >> 8u) < 8u) {
            const int sourcePair = static_cast<int>(id >> 8u);
            row.insert(QStringLiteral("sourcePair"), sourcePair);
            row.insert(QStringLiteral("source"),
                       QStringLiteral("Slot %1 / mapping unconfirmed").arg(sourcePair));
        } else {
            row.insert(QStringLiteral("sourcePair"), -1);
            row.insert(QStringLiteral("source"), QStringLiteral("Global / unknown"));
        }
        rows.append(row);
    }
    return rows;
}

QString defaultDllPath() {
#ifdef Q_OS_WIN
    return QStringLiteral("C:/Program Files/BEACN/Mic Device Driver/x64/BEACNUsbAudioapi_x64.dll");
#else
    return QStringLiteral("BEACNUsbAudioapi_x64.dll");
#endif
}

QString pinName(int pin) {
    switch (pin) {
    case kPlaybackPin:
        return QStringLiteral("Playback");
    case kDeviceInputPin:
        return QStringLiteral("DeviceInput");
    case kVirtualPlaybackPin:
        return QStringLiteral("VirtualPlayback");
    case kRecordPin:
        return QStringLiteral("Record");
    case kDeviceOutputPin:
        return QStringLiteral("DeviceOutput");
    case kVirtualRecordPin:
        return QStringLiteral("VirtualRecord");
    default:
        return QStringLiteral("Pin%1").arg(pin);
    }
}
} // namespace

BeacnBackend::BeacnBackend(QObject *parent) : QObject(parent), m_dllPath(defaultDllPath()) {
    QString logDir = qEnvironmentVariable("WHAT_BEACN_LOG_DIR");
    if (logDir.isEmpty())
        logDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                 QStringLiteral("/logs/beacn");
    QDir().mkpath(logDir);
    // A new session log starts every time the module is enabled. Keep a week's
    // worth, and never more than the newest 30 files.
    {
        const auto old = QDir(logDir).entryInfoList({QStringLiteral("beacn-*")}, QDir::Files, QDir::Time);
        const auto cutoff = QDateTime::currentDateTime().addDays(-7);
        for (qsizetype i = 0; i < old.size(); ++i)
            if (i >= 30 || old[i].lastModified() < cutoff)
                QFile::remove(old[i].absoluteFilePath());
    }
    const QString session =
        QStringLiteral("beacn-%1-%2")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")))
            .arg(QCoreApplication::applicationPid());
    m_logPath = logDir + QLatin1Char('/') + session + QStringLiteral(".log");
    m_logFile = new QFile(m_logPath, this);
    const bool logOk = m_logFile->open(QIODevice::WriteOnly | QIODevice::Text);
    m_csvFile = new QFile(logDir + QLatin1Char('/') + session + QStringLiteral("-volumes.csv"), this);
    const bool csvOk = m_csvFile->open(QIODevice::WriteOnly | QIODevice::Text);
    if (csvOk) {
        m_csvFile->write(
            "timestamp,event,source_pair,bus_pair,left_raw,right_raw,left_percent,right_percent,valid\n");
        m_csvFile->flush();
    }
    m_recordingStatus = logOk && csvOk ? QStringLiteral("Auto-export ready; connect to record volumes")
                                       : QStringLiteral("EXPORT ERROR: log=%1; CSV=%2")
                                             .arg(m_logFile->errorString(), m_csvFile->errorString());
    m_recordTimer = new QTimer(this);
    m_recordTimer->setInterval(1000);
    connect(m_recordTimer, &QTimer::timeout, this, &BeacnBackend::recordTick);
    m_recordTimer->start();

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(3000);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this] {
        if (m_reconnectPending)
            connectDriver();
    });
    m_reconnectTimer->start();
    QSettings settings;
    settings.beginGroup("modules/beacn/backend");
    m_exePath =
        settings.value("exePath", QStringLiteral("C:/Program Files/BEACN/BEACN App/BEACN.exe")).toString();
    m_verboseLogging = settings.value(QStringLiteral("verboseLogging"), false).toBool();
    const QString savedDll = settings.value(QStringLiteral("dllPath")).toString();
    if (!savedDll.isEmpty())
        m_dllPath = savedDll;

    for (int i = 0; i < 32; ++i) {
        const QString alias = settings.value(QStringLiteral("sourceAlias/%1").arg(i)).toString();
        if (!alias.isEmpty())
            m_sourceAliases.insert(i, alias);
    }

    appendLog(QStringLiteral(
        "WHAT BEACN module (backend 0.8.4) started; native volume/mute bridge; commands require current-session confirmation"));
    appendLog(QStringLiteral("Continuous exports: %1").arg(logDir));
    appendLog(QStringLiteral("DLL path: %1").arg(m_dllPath));

#ifdef Q_OS_WIN
    m_beacnAppPollTimer = new QTimer(this);
    m_beacnAppPollTimer->setInterval(100);
    m_beacnAppPollTimer->setTimerType(Qt::PreciseTimer);
    connect(m_beacnAppPollTimer, &QTimer::timeout, this, &BeacnBackend::pollBeacnAppFaders);
#endif

    refreshProfile();
}

BeacnBackend::~BeacnBackend() {
    m_nativeScanStop->store(true);
    disconnectDriver();
    releaseBeacnAppBridge();
}

void BeacnBackend::setDllPath(const QString &path) {
    const QString cleaned = QDir::fromNativeSeparators(path.trimmed());
    if (cleaned == m_dllPath)
        return;
    if (m_connected || m_reconnectPending)
        disconnectDriver();
    m_dllPath = cleaned;
    ++m_installationGeneration;
    m_installation.clear();
    emit installationChanged();
    QSettings().setValue(QStringLiteral("modules/beacn/backend/dllPath"), m_dllPath);
    emit dllPathChanged();
}

void BeacnBackend::setExePath(const QString &path) {
    const auto cleaned = QDir::fromNativeSeparators(path.trimmed());
    if (cleaned == m_exePath)
        return;
    disconnectDriver();
    m_exePath = cleaned;
    ++m_installationGeneration;
    m_installation.clear();
    QSettings().setValue("modules/beacn/backend/exePath", m_exePath);
    emit installationChanged();
}

void BeacnBackend::selectInstalledFile(const QUrl &url, bool executable) {
    if (!url.isLocalFile() || m_discoveryBusy)
        return;
    if (executable)
        setExePath(url.toLocalFile());
    else
        setDllPath(url.toLocalFile());
    scanInstalledFiles(false);
}

void BeacnBackend::scanInstalledFiles(bool search) {
    if (m_discoveryBusy || m_connected || m_reconnectPending)
        return;
    m_discoveryBusy = true;
    const auto generation = ++m_installationGeneration;
    emit installationChanged();
    auto watcher = new QFutureWatcher<QVariantMap>(this);
    connect(watcher, &QFutureWatcher<QVariantMap>::finished, this, [this, watcher, generation] {
        const auto result = watcher->result();
        watcher->deleteLater();
        m_discoveryBusy = false;
        if (generation == m_installationGeneration) {
            m_exePath = result.value("appPath").toString();
            m_dllPath = result.value("driverPath").toString();
            m_installation = result;
            m_installation.insert("helperPresent", QFileInfo::exists(QCoreApplication::applicationDirPath() +
                                                                     "/beacn-volume-hook.dll"));
            QSettings settings;
            settings.beginGroup("modules/beacn/backend");
            settings.setValue("exePath", m_exePath);
            settings.setValue("dllPath", m_dllPath);
            appendLog(
                "INSTALLATION " +
                QString::fromUtf8(QJsonDocument::fromVariant(m_installation).toJson(QJsonDocument::Compact)));
            emit dllPathChanged();
        }
        emit installationChanged();
    });
    watcher->setFuture(QtConcurrent::run(discoverInstalledFiles, m_exePath, m_dllPath, search));
}

void BeacnBackend::setVerboseLogging(bool enabled) {
    if (enabled == m_verboseLogging)
        return;
    m_verboseLogging = enabled;
    QSettings().setValue(QStringLiteral("modules/beacn/backend/verboseLogging"), enabled);
    appendLog(QStringLiteral("Verbose driver I/O logging %1")
                  .arg(enabled ? QStringLiteral("enabled") : QStringLiteral("disabled")));
    emit verboseLoggingChanged();
}

void BeacnBackend::setWritesArmed(bool armed) {
    if (armed == m_writesArmed)
        return;
    m_writesArmed = armed;
    appendLog(QStringLiteral("Writes %1 by user")
                  .arg(armed ? QStringLiteral("ARMED") : QStringLiteral("disarmed")));
    emit writesArmedChanged();
}

bool BeacnBackend::connectDriver() {
    if (m_connected)
        return true;

    if (m_discoveryBusy) {
        updateStatus("Wait for file verification to finish.");
        return false;
    }
    updateStatus(QStringLiteral("Loading driver API…"));
    appendLog(QStringLiteral("Connect requested"));

    if (!loadApi())
        return false;

    m_reconnectPending = true;
    emit connectedChanged();
    const Status enumStatus = m_api.enumerateDevices();
    appendLog(QStringLiteral("TUSBAUDIO_EnumerateDevices -> %1").arg(statusHex(enumStatus)));
    if (enumStatus != 0) {
        updateStatus(QStringLiteral("Device enumeration failed (%1)").arg(statusHex(enumStatus)));
        unloadApi();
        return false;
    }

    const auto count = m_api.getDeviceCount();
    appendLog(QStringLiteral("TUSBAUDIO_GetDeviceCount -> %1").arg(count));
    if (count < 1) {
        updateStatus(QStringLiteral("No TUSBAudio device found"));
        unloadApi();
        return false;
    }

    if (!openDevice()) {
        unloadApi();
        return false;
    }

    m_connected = true;
    ++m_connectionGeneration;
    updateStatus(QStringLiteral("Connected to TUSBAudio device 0"));

    // Connecting must not implicitly arm writes.
    setWritesArmed(false);

    if (!refreshDeviceInfo())
        appendLog(QStringLiteral("Connected, but one or more device-info queries failed"));
    if (!refreshMatrix()) {
        disconnectDriver();
        m_reconnectPending = true;
        emit connectedChanged();
        updateStatus("Device unavailable; retrying connection every 3 seconds.");
        return false;
    }
    refreshProfile();
    refreshNativeStates();
    m_reconnectPending = false;
    setRecording(true);
    emit connectedChanged();

    // Do not auto-scan UI Automation here. BEACN 1.4.0 exposes no writable
    // RangeValue sliders. Hardware/application sync is being mapped through
    // the Mix Create WinUSB parameter protocol instead.
    m_beacnAppBridgeStatus =
        QStringLiteral("UI Automation disabled: BEACN 1.4.0 exposes no writable mixer sliders.");
    emit beacnAppBridgeChanged();
    return true;
}

void BeacnBackend::disconnectDriver() {
    ++m_connectionGeneration;
    ++m_globalSequence;
    m_reconnectPending = false;
    stopNotificationMonitor();
    releaseBeacnAppBridge();
    setWritesArmed(false);
    closeDevice();
    unloadApi();

    if (m_connected) {
        m_connected = false;
        emit connectedChanged();
    }

    m_apiVersion = QStringLiteral("-");
    m_mixerVersion = QStringLiteral("-");
    m_mixerMajor = 0;
    m_mixerMinor = 0;
    m_sampleRate = 0;
    m_channelSummary = QStringLiteral("-");
    m_deviceOutCount = 0;
    m_deviceInCount = 0;
    m_playbackCount = 0;
    m_recordCount = 0;
    m_virtualPlaybackCount = 0;
    m_virtualRecordCount = 0;
    m_sourceRouteMasks.clear();
    m_recordedMatrix.clear();
    m_matrix.clear();
    m_globalState.clear();
    m_deviceChoices.clear();
    m_globalMixer = m_globalProfiles = m_globalAssignments = m_globalOsd = m_globalSettings = 0;
    m_globalUncertain = false;
    m_globalStatus = QStringLiteral("Disconnected");
    emit globalStateChanged();
    m_nativeStates.clear();
    emit nativeStatesChanged();
    exportRecording(false);
    emit matrixChanged();
    emit deviceInfoChanged();
    updateStatus(QStringLiteral("Disconnected"));
    emit connectedChanged();
}

bool BeacnBackend::loadApi() {
    if (m_library.isLoaded())
        return true;

    if (!QFileInfo::exists(m_dllPath)) {
        updateStatus(QStringLiteral("DLL not found: %1").arg(m_dllPath));
        appendLog(m_status);
        return false;
    }

#ifndef BEACN_TEST_FIXTURE
    if (!verifiedDriverFile(m_dllPath)) {
        updateStatus("Unsupported driver DLL. Use Setup to locate and verify the installed BEACN files.");
        appendLog(m_status);
        return false;
    }
#endif
    m_library.setFileName(m_dllPath);
    if (!m_library.load()) {
        updateStatus(QStringLiteral("DLL load failed: %1").arg(m_library.errorString()));
        appendLog(m_status);
        return false;
    }

    appendLog(QStringLiteral("Loaded %1").arg(m_dllPath));
    if (!resolveFunctions()) {
        unloadApi();
        return false;
    }
    return true;
}

void BeacnBackend::unloadApi() {
    m_api = {};
    if (m_library.isLoaded()) {
        m_library.unload();
        appendLog(QStringLiteral("Driver API DLL unloaded"));
    }
}

bool BeacnBackend::resolveFunctions() {
    auto resolve = [this](const char *name) -> QFunctionPointer {
        const auto pointer = m_library.resolve(name);
        if (!pointer)
            appendLog(QStringLiteral("Missing export: %1").arg(QString::fromLatin1(name)));
        return pointer;
    };

    m_api.getApiVersion = reinterpret_cast<GetApiVersionFn>(resolve("TUSBAUDIO_GetApiVersion"));
    m_api.enumerateDevices = reinterpret_cast<EnumerateDevicesFn>(resolve("TUSBAUDIO_EnumerateDevices"));
    m_api.getDeviceCount = reinterpret_cast<GetDeviceCountFn>(resolve("TUSBAUDIO_GetDeviceCount"));
    m_api.openDeviceByIndex = reinterpret_cast<OpenDeviceByIndexFn>(resolve("TUSBAUDIO_OpenDeviceByIndex"));
    m_api.closeDevice = reinterpret_cast<CloseDeviceFn>(resolve("TUSBAUDIO_CloseDevice"));
    m_api.getCurrentSampleRate =
        reinterpret_cast<GetCurrentSampleRateFn>(resolve("TUSBAUDIO_GetCurrentSampleRate"));
    m_api.getDspProperty = reinterpret_cast<GetDspPropertyFn>(resolve("TUSBAUDIO_GetDspProperty"));
    m_api.setDspProperty = reinterpret_cast<SetDspPropertyFn>(resolve("TUSBAUDIO_SetDspProperty"));
    m_api.registerDeviceNotification =
        reinterpret_cast<RegisterDeviceNotificationFn>(resolve("TUSBAUDIO_RegisterDeviceNotification"));
    m_api.readDeviceNotification =
        reinterpret_cast<ReadDeviceNotificationFn>(resolve("TUSBAUDIO_ReadDeviceNotification"));

    const bool ok = m_api.getApiVersion && m_api.enumerateDevices && m_api.getDeviceCount &&
                    m_api.openDeviceByIndex && m_api.closeDevice && m_api.getCurrentSampleRate &&
                    m_api.getDspProperty && m_api.setDspProperty;
    if (!ok)
        updateStatus(QStringLiteral("Required TUSBAudio exports are missing"));
    return ok;
}

QString BeacnBackend::notificationEventName(std::uint32_t eventType) const {
    switch (eventType) {
    case 0:
        return QStringLiteral("SampleRateChanged");
    case 1:
        return QStringLiteral("StreamFormatChanged");
    case 2:
        return QStringLiteral("AcNodeInterrupt");
    case 3:
        return QStringLiteral("VolumeChanged");
    case 4:
        return QStringLiteral("MuteChanged");
    default:
        return QStringLiteral("Unknown(%1)").arg(eventType);
    }
}

bool BeacnBackend::startNotificationMonitor() {
    stopNotificationMonitor();

    if (!m_connected || !m_device) {
        appendLog(QStringLiteral("Notification monitor not started: device is not connected"));
        return false;
    }
    if (!m_api.registerDeviceNotification || !m_api.readDeviceNotification) {
        appendLog(
            QStringLiteral("Notification monitor unavailable: TUSBAUDIO notification exports are missing"));
        m_lastNotification = QStringLiteral("Notification API unavailable in this driver DLL.");
        emit notificationChanged();
        return false;
    }

#ifdef Q_OS_WIN
    // Thesycon explicitly requires an auto-reset event for the shared notification event.
    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) {
        const DWORD error = GetLastError();
        appendLog(QStringLiteral("CreateEventW for notification monitor failed: Win32 error %1").arg(error));
        m_lastNotification = QStringLiteral("Could not create Win32 notification event (%1).").arg(error);
        emit notificationChanged();
        return false;
    }

    constexpr std::uint32_t kNotifyAll = 0xffffffffu;
    const Status status = m_api.registerDeviceNotification(m_device, kNotifyAll, eventHandle, 0);
    appendLog(QStringLiteral("TUSBAUDIO_RegisterDeviceNotification(ALL) -> %1 event=0x%2")
                  .arg(statusHex(status))
                  .arg(reinterpret_cast<quintptr>(eventHandle), 0, 16));
    if (status != 0) {
        CloseHandle(eventHandle);
        m_lastNotification =
            QStringLiteral("Driver notification registration failed (%1).").arg(statusHex(status));
        emit notificationChanged();
        return false;
    }

    m_notificationEvent = eventHandle;
    m_notificationPollTimer = new QTimer(this);
    m_notificationPollTimer->setInterval(20);
    m_notificationPollTimer->setTimerType(Qt::PreciseTimer);
    connect(m_notificationPollTimer, &QTimer::timeout, this, [this] {
        if (!m_notificationEvent)
            return;
        const DWORD wait = WaitForSingleObject(reinterpret_cast<HANDLE>(m_notificationEvent), 0);
        if (wait == WAIT_OBJECT_0)
            drainDeviceNotifications();
        else if (wait == WAIT_FAILED)
            appendLog(QStringLiteral("WaitForSingleObject(notification) failed: Win32 error %1")
                          .arg(GetLastError()));
    });
    m_notificationPollTimer->start();
    m_notificationActive = true;
    m_notificationCount = 0;
    m_lastNotification = QStringLiteral("Monitoring driver notifications; move a BEACN knob or fader.");
    appendLog(QStringLiteral("TUSBAudio notification monitor ACTIVE"));
    emit notificationChanged();
    return true;
#else
    appendLog(QStringLiteral("Notification monitor is Windows-only"));
    m_lastNotification = QStringLiteral("Notification monitor is Windows-only.");
    emit notificationChanged();
    return false;
#endif
}

void BeacnBackend::stopNotificationMonitor() {
    if (m_notificationPollTimer) {
        m_notificationPollTimer->stop();
        delete m_notificationPollTimer;
        m_notificationPollTimer = nullptr;
    }

#ifdef Q_OS_WIN
    if (m_device && m_api.registerDeviceNotification && m_notificationActive) {
        constexpr std::uint32_t kNotifyNone = 0u;
        const Status status = m_api.registerDeviceNotification(m_device, kNotifyNone, nullptr, 0);
        appendLog(QStringLiteral("TUSBAUDIO_RegisterDeviceNotification(NONE) -> %1").arg(statusHex(status)));
    }

    if (m_notificationEvent) {
        CloseHandle(reinterpret_cast<HANDLE>(m_notificationEvent));
        m_notificationEvent = nullptr;
    }
#endif

    if (m_notificationActive) {
        m_notificationActive = false;
        emit notificationChanged();
    }
}

bool BeacnBackend::restartNotificationMonitor() {
    return startNotificationMonitor();
}

void BeacnBackend::drainDeviceNotifications() {
    if (!m_connected || !m_device || !m_api.readDeviceNotification)
        return;

    // Large enough for documented notification payloads and useful for private/extended events.
    std::array<unsigned char, 1024> data{};
    constexpr int kDrainLimit = 128;

    for (int i = 0; i < kDrainLimit; ++i) {
        std::uint32_t eventType = 0xffffffffu;
        std::uint32_t bytesReturned = 0;
        const Status status = m_api.readDeviceNotification(
            m_device, &eventType, data.data(), static_cast<std::uint32_t>(data.size()), &bytesReturned);

        if (status != 0) {
            // The documented empty-queue result is nonzero. Avoid log spam for normal drains.
            if (i == 0 && m_verboseLogging)
                appendLog(
                    QStringLiteral("ReadDeviceNotification queue empty/status=%1").arg(statusHex(status)));
            break;
        }

        ++m_notificationCount;
        const std::uint32_t safeBytes =
            std::min<std::uint32_t>(bytesReturned, static_cast<std::uint32_t>(data.size()));
        QByteArray raw(reinterpret_cast<const char *>(data.data()), static_cast<qsizetype>(safeBytes));
        const QString hex = QString::fromLatin1(raw.toHex(' '));
        QString details;

        // Documented VolumeChanged/MuteChanged payload: u8 featureUnitId, u8 channel, i16 newValue.
        if ((eventType == 3 || eventType == 4) && safeBytes >= 4) {
            const auto unit = static_cast<unsigned int>(data[0]);
            const auto channel = static_cast<unsigned int>(data[1]);
            std::int16_t newValue = 0;
            std::memcpy(&newValue, data.data() + 2, sizeof(newValue));
            if (eventType == 3) {
                details = QStringLiteral(" featureUnit=%1 channel=%2 value=%3 (%4 dB)")
                              .arg(unit)
                              .arg(channel)
                              .arg(newValue)
                              .arg(static_cast<double>(newValue) / 256.0, 0, 'f', 2);
            } else {
                details = QStringLiteral(" featureUnit=%1 channel=%2 muted=%3")
                              .arg(unit)
                              .arg(channel)
                              .arg(newValue != 0 ? QStringLiteral("true") : QStringLiteral("false"));
            }
        }

        m_lastNotification = QStringLiteral("%1  bytes=%2%3")
                                 .arg(notificationEventName(eventType))
                                 .arg(safeBytes)
                                 .arg(details);
        appendLog(QStringLiteral("NOTIFY #%1 type=%2(%3) bytes=%4%5 raw=[%6]")
                      .arg(m_notificationCount)
                      .arg(eventType)
                      .arg(notificationEventName(eventType))
                      .arg(safeBytes)
                      .arg(details)
                      .arg(hex));
        emit notificationChanged();

        // Any driver/device-originated event may correspond to a BEACN control change.
        // Refresh live mixer state so WHAT follows the physical device and the official app immediately.
        refreshMatrix();
    }
}

void BeacnBackend::releaseBeacnAppBridge() {
#ifdef Q_OS_WIN
    if (m_beacnAppPollTimer)
        m_beacnAppPollTimer->stop();
    m_beacnWindow = nullptr;
    for (auto &binding : m_uiaFaderBindings) {
        if (binding.element) {
            reinterpret_cast<IUIAutomationElement *>(binding.element)->Release();
            binding.element = nullptr;
        }
    }
    m_uiaFaderBindings.clear();

    if (m_uiaAutomation) {
        reinterpret_cast<IUIAutomation *>(m_uiaAutomation)->Release();
        m_uiaAutomation = nullptr;
    }
    if (m_uiaComOwned) {
        CoUninitialize();
        m_uiaComOwned = false;
    }
#endif

    const bool changed = m_beacnAppBridgeReady || m_beacnAppSliderCount != 0 ||
                         m_beacnAppMappedFaderCount != 0 || !m_beacnAppFaders.isEmpty();
    m_beacnAppBridgeReady = false;
    m_beacnAppSliderCount = 0;
    m_beacnAppMappedFaderCount = 0;
    m_beacnAppFaders.clear();
    const bool hadLevels = !m_beacnAppLevels.isEmpty();
    m_beacnAppLevels.clear();
    if (hadLevels)
        emit beacnAppLevelsChanged();
    if (!m_connected)
        m_beacnAppBridgeStatus = QStringLiteral("BEACN app bridge disconnected.");
    if (changed)
        emit beacnAppBridgeChanged();
}

bool BeacnBackend::scanBeacnAppBridge() {
#ifdef Q_OS_WIN
    releaseBeacnAppBridge();
    m_beacnAppBridgeStatus = QStringLiteral("Scanning the official BEACN app…");
    emit beacnAppBridgeChanged();

    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (comHr == S_OK || comHr == S_FALSE) {
        m_uiaComOwned = true;
    } else if (comHr != RPC_E_CHANGED_MODE) {
        m_beacnAppBridgeStatus = QStringLiteral("COM initialization failed (0x%1)")
                                     .arg(static_cast<quint32>(comHr), 8, 16, QLatin1Char('0'));
        appendLog(QStringLiteral("BEACN UIA: %1").arg(m_beacnAppBridgeStatus));
        emit beacnAppBridgeChanged();
        return false;
    }

    IUIAutomation *automation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                  reinterpret_cast<void **>(&automation));
    if (FAILED(hr) || !automation) {
        m_beacnAppBridgeStatus = QStringLiteral("Could not create Windows UI Automation client (0x%1)")
                                     .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        appendLog(QStringLiteral("BEACN UIA: %1").arg(m_beacnAppBridgeStatus));
        emit beacnAppBridgeChanged();
        return false;
    }
    m_uiaAutomation = automation;

    BeacnWindowSearch search;
    EnumWindows(findBeacnWindowProc, reinterpret_cast<LPARAM>(&search));
    if (!search.best) {
        m_beacnAppBridgeStatus = QStringLiteral(
            "BEACN.exe is running without a usable top-level window. Open the BEACN mixer window and rescan.");
        appendLog(QStringLiteral("BEACN UIA: no BEACN top-level window found"));
        emit beacnAppBridgeChanged();
        return false;
    }

    m_beacnWindow = reinterpret_cast<void *>(search.best);

    wchar_t titleBuffer[1024]{};
    GetWindowTextW(search.best, titleBuffer, static_cast<int>(std::size(titleBuffer)));
    appendLog(QStringLiteral("BEACN UIA: using hwnd=0x%1 pid=%2 title=\"%3\"")
                  .arg(reinterpret_cast<quintptr>(search.best), 0, 16)
                  .arg(search.pid)
                  .arg(QString::fromWCharArray(titleBuffer)));

    IUIAutomationElement *root = nullptr;
    hr = automation->ElementFromHandle(search.best, &root);
    if (FAILED(hr) || !root) {
        m_beacnAppBridgeStatus = QStringLiteral("UI Automation could not attach to the BEACN window (0x%1)")
                                     .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        appendLog(QStringLiteral("BEACN UIA: %1").arg(m_beacnAppBridgeStatus));
        emit beacnAppBridgeChanged();
        return false;
    }

    IUIAutomationCondition *trueCondition = nullptr;
    IUIAutomationElementArray *elements = nullptr;
    hr = automation->CreateTrueCondition(&trueCondition);
    if (SUCCEEDED(hr) && trueCondition)
        hr = root->FindAll(static_cast<TreeScope>(TreeScope_Element | TreeScope_Descendants), trueCondition,
                           &elements);
    if (trueCondition)
        trueCondition->Release();
    root->Release();

    if (FAILED(hr) || !elements) {
        m_beacnAppBridgeStatus = QStringLiteral("Could not enumerate BEACN accessibility controls (0x%1)")
                                     .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        appendLog(QStringLiteral("BEACN UIA: %1").arg(m_beacnAppBridgeStatus));
        emit beacnAppBridgeChanged();
        return false;
    }

    struct SliderCandidate {
        IUIAutomationElement *element = nullptr;
        QString name;
        QString automationId;
        QString helpText;
        RECT rect{};
        double minimum = 0.0;
        double maximum = 100.0;
        double value = 0.0;
    };

    struct NamedElement {
        QString name;
        RECT rect{};
        CONTROLTYPEID controlType = 0;
    };

    QVector<SliderCandidate> sliders;
    QVector<NamedElement> named;
    int elementCount = 0;
    elements->get_Length(&elementCount);

    for (int i = 0; i < elementCount; ++i) {
        IUIAutomationElement *element = nullptr;
        if (FAILED(elements->GetElement(i, &element)) || !element)
            continue;

        CONTROLTYPEID controlType = 0;
        RECT rect{};
        BOOL offscreen = FALSE;
        element->get_CurrentControlType(&controlType);
        element->get_CurrentBoundingRectangle(&rect);
        element->get_CurrentIsOffscreen(&offscreen);

        const QString name = uiaName(element);
        const QString automationId = uiaAutomationId(element);
        const QString helpText = uiaHelpText(element);
        if (!name.isEmpty() && !offscreen)
            named.push_back({name, rect, controlType});

        bool retained = false;
        if (controlType == UIA_SliderControlTypeId && !offscreen) {
            IUnknown *unknownPattern = nullptr;
            if (SUCCEEDED(element->GetCurrentPattern(UIA_RangeValuePatternId, &unknownPattern)) &&
                unknownPattern) {
                IUIAutomationRangeValuePattern *range = nullptr;
                const HRESULT qi = unknownPattern->QueryInterface(IID_IUIAutomationRangeValuePattern,
                                                                  reinterpret_cast<void **>(&range));
                unknownPattern->Release();
                if (SUCCEEDED(qi) && range) {
                    BOOL readOnly = TRUE;
                    double minimum = 0.0;
                    double maximum = 0.0;
                    double value = 0.0;
                    range->get_CurrentIsReadOnly(&readOnly);
                    range->get_CurrentMinimum(&minimum);
                    range->get_CurrentMaximum(&maximum);
                    range->get_CurrentValue(&value);
                    range->Release();
                    if (!readOnly && std::isfinite(minimum) && std::isfinite(maximum) && maximum > minimum) {
                        sliders.push_back(
                            {element, name, automationId, helpText, rect, minimum, maximum, value});
                        retained = true;
                        appendLog(
                            QStringLiteral(
                                "BEACN UIA slider #%1 name=\"%2\" id=\"%3\" help=\"%4\" rect=[%5,%6 %7x%8] range=[%9,%10] value=%11")
                                .arg(sliders.size() - 1)
                                .arg(name)
                                .arg(automationId)
                                .arg(helpText)
                                .arg(rect.left)
                                .arg(rect.top)
                                .arg(rect.right - rect.left)
                                .arg(rect.bottom - rect.top)
                                .arg(minimum, 0, 'g', 10)
                                .arg(maximum, 0, 'g', 10)
                                .arg(value, 0, 'g', 10));
                    }
                }
            }
        }

        if (!retained)
            element->Release();
    }
    elements->Release();

    m_beacnAppSliderCount = sliders.size();
    QSet<int> used;
    QVariantList mappedRows;

    auto combinedSliderText = [&](const SliderCandidate &slider) {
        return normalizeUiText(slider.name + QLatin1Char(' ') + slider.automationId + QLatin1Char(' ') +
                               slider.helpText);
    };

    auto addBinding = [&](int sliderIndex, int sourcePair, int busPair, const QString &sourceName,
                          const QString &reason) {
        if (sliderIndex < 0 || sliderIndex >= sliders.size() || used.contains(sliderIndex))
            return false;
        auto &slider = sliders[sliderIndex];
        UiaFaderBinding binding;
        binding.sourcePair = sourcePair;
        binding.busPair = busPair;
        binding.element = slider.element;
        binding.minimum = slider.minimum;
        binding.maximum = slider.maximum;
        binding.sourceName = sourceName;
        binding.elementName = slider.name;
        binding.automationId = slider.automationId;
        binding.left = slider.rect.left;
        binding.top = slider.rect.top;
        binding.right = slider.rect.right;
        binding.bottom = slider.rect.bottom;
        m_uiaFaderBindings.push_back(binding);
        used.insert(sliderIndex);
        slider.element = nullptr; // ownership moved to m_uiaFaderBindings

        QVariantMap row;
        row.insert(QStringLiteral("sourcePair"), sourcePair);
        row.insert(QStringLiteral("busPair"), busPair);
        row.insert(QStringLiteral("source"), sourceName);
        row.insert(QStringLiteral("mix"),
                   busPair == 0 ? QStringLiteral("Audience") : QStringLiteral("Personal"));
        row.insert(QStringLiteral("name"), binding.elementName);
        row.insert(QStringLiteral("automationId"), binding.automationId);
        row.insert(QStringLiteral("minimum"), binding.minimum);
        row.insert(QStringLiteral("maximum"), binding.maximum);
        row.insert(QStringLiteral("reason"), reason);
        mappedRows.append(row);
        appendLog(
            QStringLiteral("BEACN UIA MAP sourcePair=%1 %2 -> %3 mix slider#%4 name=\"%5\" id=\"%6\" (%7)")
                .arg(sourcePair)
                .arg(sourceName)
                .arg(busPair == 0 ? QStringLiteral("Audience") : QStringLiteral("Personal"))
                .arg(sliderIndex)
                .arg(binding.elementName)
                .arg(binding.automationId)
                .arg(reason));
        return true;
    };

    // First choice: match each source label/name exposed by JUCE accessibility,
    // then bind the two closest writable RangeValue sliders. This is resilient
    // to channel reordering in the BEACN profile.
    for (int sourcePair = 0; sourcePair < m_sourceNames.size(); ++sourcePair) {
        const QString sourceName = m_sourceNames.at(sourcePair).trimmed();
        if (sourceName.isEmpty())
            continue;
        const QString sourceNorm = normalizeUiText(sourceName);

        QVector<int> direct;
        for (int si = 0; si < sliders.size(); ++si) {
            if (used.contains(si))
                continue;
            if (combinedSliderText(sliders.at(si)).contains(sourceNorm))
                direct.push_back(si);
        }

        RECT anchor{};
        bool haveAnchor = false;
        double bestAnchorScore = std::numeric_limits<double>::max();
        for (const auto &entry : named) {
            const QString n = normalizeUiText(entry.name);
            if (n != sourceNorm && !n.contains(sourceNorm))
                continue;
            double nearestSlider = std::numeric_limits<double>::max();
            for (int si = 0; si < sliders.size(); ++si) {
                if (!used.contains(si))
                    nearestSlider =
                        std::min(nearestSlider, rectDistanceSquared(entry.rect, sliders.at(si).rect));
            }
            if (nearestSlider < bestAnchorScore) {
                anchor = entry.rect;
                bestAnchorScore = nearestSlider;
                haveAnchor = true;
            }
        }

        QVector<int> chosen = direct;
        if (chosen.size() < 2 && haveAnchor) {
            QVector<QPair<double, int>> scored;
            for (int si = 0; si < sliders.size(); ++si) {
                if (used.contains(si) || chosen.contains(si))
                    continue;
                scored.append({rectDistanceSquared(anchor, sliders.at(si).rect), si});
            }
            std::sort(scored.begin(), scored.end(),
                      [](const auto &a, const auto &b) { return a.first < b.first; });
            for (const auto &item : scored) {
                chosen.push_back(item.second);
                if (chosen.size() >= 2)
                    break;
            }
        }

        if (chosen.size() < 2)
            continue;
        if (chosen.size() > 2)
            chosen.resize(2);

        int firstBus = busFromAccessibilityText(sliders.at(chosen[0]).name + QLatin1Char(' ') +
                                                sliders.at(chosen[0]).automationId + QLatin1Char(' ') +
                                                sliders.at(chosen[0]).helpText);
        int secondBus = busFromAccessibilityText(sliders.at(chosen[1]).name + QLatin1Char(' ') +
                                                 sliders.at(chosen[1]).automationId + QLatin1Char(' ') +
                                                 sliders.at(chosen[1]).helpText);

        if (firstBus >= 0 && secondBus < 0)
            secondBus = 1 - firstBus;
        else if (secondBus >= 0 && firstBus < 0)
            firstBus = 1 - secondBus;
        else if (firstBus < 0 || secondBus < 0 || firstBus == secondBus) {
            // BEACN's own UI describes the left fader as Personal and the right
            // fader as Audience. Use horizontal position when accessibility names
            // do not explicitly identify the mix.
            const double x0 = rectCenterX(sliders.at(chosen[0]).rect);
            const double x1 = rectCenterX(sliders.at(chosen[1]).rect);
            if (x0 <= x1) {
                firstBus = 1;  // Personal
                secondBus = 0; // Audience
            } else {
                firstBus = 0;
                secondBus = 1;
            }
        }

        addBinding(chosen[0], sourcePair, firstBus, sourceName,
                   haveAnchor ? QStringLiteral("source label proximity")
                              : QStringLiteral("slider accessibility name"));
        addBinding(chosen[1], sourcePair, secondBus, sourceName,
                   haveAnchor ? QStringLiteral("source label proximity")
                              : QStringLiteral("slider accessibility name"));
    }

    // Fallback for JUCE builds where some source labels are not exposed as
    // accessibility elements: pair the remaining sliders using the profile's
    // mixerPlaceInOrder. Already mapped sources/sliders are left untouched.
    if (sliders.size() >= 2 && !m_profileMixers.isEmpty()) {
        QVector<int> remaining;
        for (int i = 0; i < sliders.size(); ++i) {
            if (!used.contains(i))
                remaining.push_back(i);
        }

        QSet<int> mappedSourcePairs;
        for (const auto &binding : m_uiaFaderBindings)
            mappedSourcePairs.insert(binding.sourcePair);

        struct OrderedSource {
            int place = 0;
            int sourcePair = -1;
            QString name;
        };

        QVector<OrderedSource> orderedSources;
        for (const QVariant &entry : m_profileMixers) {
            const QVariantMap row = entry.toMap();
            const QString profileName = row.value(QStringLiteral("name")).toString();
            int pair = -1;
            for (int sp = 0; sp < m_sourceNames.size(); ++sp) {
                if (m_sourceNames.at(sp).compare(profileName, Qt::CaseInsensitive) == 0) {
                    pair = sp;
                    break;
                }
            }
            if (pair >= 0 && !mappedSourcePairs.contains(pair))
                orderedSources.push_back({row.value(QStringLiteral("place")).toInt(), pair, profileName});
        }
        std::sort(orderedSources.begin(), orderedSources.end(),
                  [](const auto &a, const auto &b) { return a.place < b.place; });

        if (!orderedSources.isEmpty() && remaining.size() == orderedSources.size() * 2) {
            std::sort(remaining.begin(), remaining.end(), [&](int a, int b) {
                const auto &ra = sliders.at(a).rect;
                const auto &rb = sliders.at(b).rect;
                const double ax = rectCenterX(ra), bx = rectCenterX(rb);
                if (std::abs(ax - bx) > 10.0)
                    return ax < bx;
                return rectCenterY(ra) < rectCenterY(rb);
            });
            for (int oi = 0; oi < orderedSources.size(); ++oi) {
                const int sourcePair = orderedSources.at(oi).sourcePair;
                const QString sourceName = orderedSources.at(oi).name;
                const int a = remaining.at(oi * 2);
                const int b = remaining.at(oi * 2 + 1);

                int aBus = busFromAccessibilityText(sliders.at(a).name + QLatin1Char(' ') +
                                                    sliders.at(a).automationId + QLatin1Char(' ') +
                                                    sliders.at(a).helpText);
                int bBus = busFromAccessibilityText(sliders.at(b).name + QLatin1Char(' ') +
                                                    sliders.at(b).automationId + QLatin1Char(' ') +
                                                    sliders.at(b).helpText);
                if (aBus < 0 || bBus < 0 || aBus == bBus) {
                    const bool aLeft = rectCenterX(sliders.at(a).rect) <= rectCenterX(sliders.at(b).rect);
                    aBus = aLeft ? 1 : 0;
                    bBus = aLeft ? 0 : 1;
                }
                addBinding(a, sourcePair, aBus, sourceName, QStringLiteral("profile-order fallback"));
                addBinding(b, sourcePair, bBus, sourceName, QStringLiteral("profile-order fallback"));
            }
        }
    }

    for (auto &slider : sliders) {
        if (slider.element) {
            slider.element->Release();
            slider.element = nullptr;
        }
    }

    m_beacnAppFaders = mappedRows;
    m_beacnAppMappedFaderCount = m_uiaFaderBindings.size();
    m_beacnAppBridgeReady = m_beacnAppMappedFaderCount > 0;
    if (m_beacnAppBridgeReady) {
        m_beacnAppBridgeStatus = QStringLiteral("BEACN app bridge ready · %1/%2 writable faders mapped")
                                     .arg(m_beacnAppMappedFaderCount)
                                     .arg(m_beacnAppSliderCount);
    } else if (m_beacnAppSliderCount == 0) {
        m_beacnAppBridgeStatus = QStringLiteral(
            "BEACN window found, but it exposed no writable UI Automation sliders. Make sure the mixer page is open.");
    } else {
        m_beacnAppBridgeStatus =
            QStringLiteral(
                "Found %1 BEACN sliders but could not map them to channel names. See the log for UIA names/rectangles.")
                .arg(m_beacnAppSliderCount);
    }
    appendLog(QStringLiteral("BEACN UIA scan complete: %1").arg(m_beacnAppBridgeStatus));
    emit beacnAppBridgeChanged();

    m_beacnAppLevels.clear();
    m_beacnAppLevels.reserve(m_sourceNames.size() * 2);
    for (int i = 0; i < m_sourceNames.size() * 2; ++i)
        m_beacnAppLevels.append(-1.0);
    pollBeacnAppFaders();
    if (m_beacnAppBridgeReady && m_beacnAppPollTimer)
        m_beacnAppPollTimer->start();
    return m_beacnAppBridgeReady;
#else
    m_beacnAppBridgeReady = false;
    m_beacnAppBridgeStatus = QStringLiteral("BEACN app bridge is Windows-only.");
    m_beacnAppSliderCount = 0;
    m_beacnAppMappedFaderCount = 0;
    m_beacnAppFaders.clear();
    emit beacnAppBridgeChanged();
    return false;
#endif
}

#ifdef Q_OS_WIN
void BeacnBackend::pollBeacnAppFaders() {
    if (!m_beacnAppBridgeReady || m_uiaFaderBindings.isEmpty())
        return;

    QVariantList next;
    next.reserve(m_sourceNames.size() * 2);
    for (int i = 0; i < m_sourceNames.size() * 2; ++i)
        next.append(-1.0);
    int good = 0;

    for (auto &binding : m_uiaFaderBindings) {
        if (!binding.element || binding.sourcePair < 0 || binding.busPair < 0 || binding.busPair > 1)
            continue;
        const int index = binding.sourcePair * 2 + binding.busPair;
        if (index < 0 || index >= next.size())
            continue;

        auto *element = reinterpret_cast<IUIAutomationElement *>(binding.element);
        IUnknown *unknownPattern = nullptr;
        HRESULT hr = element->GetCurrentPattern(UIA_RangeValuePatternId, &unknownPattern);
        if (FAILED(hr) || !unknownPattern)
            continue;

        IUIAutomationRangeValuePattern *range = nullptr;
        hr = unknownPattern->QueryInterface(IID_IUIAutomationRangeValuePattern,
                                            reinterpret_cast<void **>(&range));
        unknownPattern->Release();
        if (FAILED(hr) || !range)
            continue;

        double minimum = binding.minimum;
        double maximum = binding.maximum;
        double value = minimum;
        range->get_CurrentMinimum(&minimum);
        range->get_CurrentMaximum(&maximum);
        hr = range->get_CurrentValue(&value);
        range->Release();
        if (FAILED(hr) || !std::isfinite(minimum) || !std::isfinite(maximum) || !std::isfinite(value) ||
            maximum <= minimum) {
            continue;
        }

        binding.minimum = minimum;
        binding.maximum = maximum;
        const double percent = std::clamp((value - minimum) * 100.0 / (maximum - minimum), 0.0, 100.0);
        next[index] = percent;
        ++good;
    }

    bool changed = next.size() != m_beacnAppLevels.size();
    if (!changed) {
        for (qsizetype i = 0; i < next.size(); ++i) {
            const double a = next.at(i).toDouble();
            const double b = m_beacnAppLevels.at(i).toDouble();
            if (std::abs(a - b) > 0.05) {
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        if (m_verboseLogging && !m_beacnAppLevels.isEmpty()) {
            for (const auto &binding : m_uiaFaderBindings) {
                const int index = binding.sourcePair * 2 + binding.busPair;
                if (index < 0 || index >= next.size() || index >= m_beacnAppLevels.size())
                    continue;
                const double oldValue = m_beacnAppLevels.at(index).toDouble();
                const double newValue = next.at(index).toDouble();
                if (newValue >= 0.0 && std::abs(newValue - oldValue) > 0.5) {
                    appendLog(QStringLiteral("BEACN UIA FEEDBACK sourcePair=%1 name=%2 mix=%3 %4% -> %5%")
                                  .arg(binding.sourcePair)
                                  .arg(binding.sourceName)
                                  .arg(binding.busPair == 0 ? QStringLiteral("Audience")
                                                            : QStringLiteral("Personal"))
                                  .arg(oldValue, 0, 'f', 1)
                                  .arg(newValue, 0, 'f', 1));
                }
            }
        }
        m_beacnAppLevels = next;
        emit beacnAppLevelsChanged();
    }

    if (good == 0 && m_beacnAppPollTimer)
        m_beacnAppPollTimer->stop();
}

bool BeacnBackend::clickBeacnAppFader(UiaFaderBinding &binding, double percent) {
    auto hwnd = reinterpret_cast<HWND>(m_beacnWindow);
    if (!hwnd || !IsWindow(hwnd)) {
        appendLog(QStringLiteral("BEACN native fader click FAILED: BEACN window is gone"));
        return false;
    }

    const int width = binding.right - binding.left;
    const int height = binding.bottom - binding.top;
    if (width < 4 || height < 4) {
        appendLog(QStringLiteral("BEACN native fader click FAILED: invalid slider rectangle [%1,%2,%3,%4]")
                      .arg(binding.left)
                      .arg(binding.top)
                      .arg(binding.right)
                      .arg(binding.bottom));
        return false;
    }

    percent = std::clamp(percent, 0.0, 100.0);
    const double f = percent / 100.0;
    const int inset = std::clamp(std::min(width, height) / 6, 2, 10);
    POINT pt{};

    if (height > width * 1.35) {
        pt.x = binding.left + width / 2;
        const int usable = std::max(1, height - inset * 2);
        pt.y = binding.bottom - inset - static_cast<int>(std::lround(f * usable));
    } else {
        const int usable = std::max(1, width - inset * 2);
        pt.x = binding.left + inset + static_cast<int>(std::lround(f * usable));
        pt.y = binding.top + height / 2;
    }

    POINT client = pt;
    if (!ScreenToClient(hwnd, &client)) {
        appendLog(
            QStringLiteral("BEACN native fader click FAILED: ScreenToClient error=%1").arg(GetLastError()));
        return false;
    }

    const LPARAM pos = MAKELPARAM(static_cast<WORD>(client.x), static_cast<WORD>(client.y));
    DWORD_PTR ignored = 0;
    SendMessageTimeoutW(hwnd, WM_MOUSEMOVE, 0, pos, SMTO_ABORTIFHUNG, 250, &ignored);
    SendMessageTimeoutW(hwnd, WM_LBUTTONDOWN, MK_LBUTTON, pos, SMTO_ABORTIFHUNG, 250, &ignored);
    SendMessageTimeoutW(hwnd, WM_LBUTTONUP, 0, pos, SMTO_ABORTIFHUNG, 250, &ignored);

    appendLog(
        QStringLiteral(
            "BEACN native fader click sourcePair=%1 name=%2 mix=%3 target=%4% screen=(%5,%6) client=(%7,%8) rect=[%9,%10 %11x%12]")
            .arg(binding.sourcePair)
            .arg(binding.sourceName)
            .arg(binding.busPair == 0 ? QStringLiteral("Audience") : QStringLiteral("Personal"))
            .arg(percent, 0, 'f', 1)
            .arg(pt.x)
            .arg(pt.y)
            .arg(client.x)
            .arg(client.y)
            .arg(binding.left)
            .arg(binding.top)
            .arg(width)
            .arg(height));
    return true;
}
#endif

bool BeacnBackend::setBeacnAppFaderPercent(int sourcePair, int busPair, double percent) {
    if (!m_connected || !m_writesArmed) {
        appendLog(QStringLiteral("BEACN app write refused: %1")
                      .arg(!m_connected ? QStringLiteral("driver not connected")
                                        : QStringLiteral("live control disabled")));
        return false;
    }
    if (busPair != 0 && busPair != 1) {
        appendLog(
            QStringLiteral(
                "BEACN app write refused: bus %1 is not a Personal/Audience fader. Chat/VOD routing remains diagnostic-only until its BEACN app control is mapped.")
                .arg(busPair));
        return false;
    }

#ifdef Q_OS_WIN
    auto findBinding = [&]() -> UiaFaderBinding * {
        for (auto &binding : m_uiaFaderBindings) {
            if (binding.sourcePair == sourcePair && binding.busPair == busPair)
                return &binding;
        }
        return nullptr;
    };

    UiaFaderBinding *binding = findBinding();
    if (!binding) {
        appendLog(QStringLiteral("BEACN UIA: no cached fader for sourcePair=%1 bus=%2; rescanning")
                      .arg(sourcePair)
                      .arg(busPair));
        scanBeacnAppBridge();
        binding = findBinding();
    }
    if (!binding || !binding->element) {
        appendLog(QStringLiteral(
                      "BEACN app write FAILED: no mapped official-app fader for sourcePair=%1 (%2) bus=%3")
                      .arg(sourcePair)
                      .arg(sourcePair >= 0 && sourcePair < m_sourceNames.size() ? m_sourceNames.at(sourcePair)
                                                                                : QStringLiteral("unknown"))
                      .arg(busPair));
        return false;
    }

    auto *element = reinterpret_cast<IUIAutomationElement *>(binding->element);
    IUnknown *unknownPattern = nullptr;
    HRESULT hr = element->GetCurrentPattern(UIA_RangeValuePatternId, &unknownPattern);
    if (FAILED(hr) || !unknownPattern) {
        appendLog(QStringLiteral("BEACN UIA SetValue FAILED: RangeValue pattern disappeared (0x%1)")
                      .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
        return false;
    }

    IUIAutomationRangeValuePattern *range = nullptr;
    hr =
        unknownPattern->QueryInterface(IID_IUIAutomationRangeValuePattern, reinterpret_cast<void **>(&range));
    unknownPattern->Release();
    if (FAILED(hr) || !range) {
        appendLog(QStringLiteral("BEACN UIA SetValue FAILED: RangeValue QI 0x%1")
                      .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0')));
        return false;
    }

    BOOL readOnly = TRUE;
    double minimum = binding->minimum;
    double maximum = binding->maximum;
    double before = 0.0;
    range->get_CurrentIsReadOnly(&readOnly);
    range->get_CurrentMinimum(&minimum);
    range->get_CurrentMaximum(&maximum);
    range->get_CurrentValue(&before);
    if (readOnly || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum) {
        range->Release();
        appendLog(
            QStringLiteral("BEACN UIA SetValue refused: official slider is read-only or has invalid range"));
        return false;
    }

    percent = std::clamp(percent, 0.0, 100.0);
    const double target = minimum + (maximum - minimum) * (percent / 100.0);
    appendLog(QStringLiteral(
                  "BEACN UIA SET sourcePair=%1 name=%2 mix=%3 percent=%4 appValue=%5 range=[%6,%7] before=%8")
                  .arg(sourcePair)
                  .arg(binding->sourceName)
                  .arg(busPair == 0 ? QStringLiteral("Audience") : QStringLiteral("Personal"))
                  .arg(percent, 0, 'f', 1)
                  .arg(target, 0, 'g', 10)
                  .arg(minimum, 0, 'g', 10)
                  .arg(maximum, 0, 'g', 10)
                  .arg(before, 0, 'g', 10));

    // Put keyboard/accessibility focus on the official control before setting
    // it. JUCE routes accessibility value changes through the Slider handler,
    // i.e. the same component that services the BEACN UI.
    element->SetFocus();
    hr = range->SetValue(target);

    // JUCE can expose RangeValue without routing SetValue through the same
    // handler as a real fader gesture. Always send a click to the mapped
    // official fader too, so BEACN itself performs the control change and
    // pushes it to the Mix Create.
    const bool nativeClickSent = clickBeacnAppFader(*binding, percent);

    double after = before;
    if (SUCCEEDED(hr) || nativeClickSent)
        range->get_CurrentValue(&after);
    range->Release();

    const double tolerance = std::max(1e-6, std::abs(maximum - minimum) / 10000.0);
    const bool verified = SUCCEEDED(hr) && std::abs(after - target) <= tolerance;
    appendLog(QStringLiteral("BEACN UIA SetValue -> 0x%1 readback=%2 %3; nativeClick=%4")
                  .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'))
                  .arg(after, 0, 'g', 10)
                  .arg(verified ? QStringLiteral("PASS") : QStringLiteral("READBACK MISMATCH"))
                  .arg(nativeClickSent ? QStringLiteral("sent") : QStringLiteral("failed")));

    if (SUCCEEDED(hr) || nativeClickSent) {
        // BEACN owns the real write. Give its event loop a moment to update the
        // DSP and Mix Create, then follow that state back through the driver.
        QTimer::singleShot(40, this, [this] { pollBeacnAppFaders(); });
        QTimer::singleShot(100, this, [this] { refreshMatrix(); });
        QTimer::singleShot(500, this, [this] { refreshProfile(); });
    }
    return SUCCEEDED(hr) || nativeClickSent;
#else
    Q_UNUSED(sourcePair)
    Q_UNUSED(busPair)
    Q_UNUSED(percent)
    appendLog(QStringLiteral("BEACN app write is Windows-only"));
    return false;
#endif
}

bool BeacnBackend::beacnAppFaderMapped(int sourcePair, int busPair) const {
#ifdef Q_OS_WIN
    for (const auto &binding : m_uiaFaderBindings) {
        if (binding.sourcePair == sourcePair && binding.busPair == busPair && binding.element)
            return true;
    }
#else
    Q_UNUSED(sourcePair)
    Q_UNUSED(busPair)
#endif
    return false;
}

bool BeacnBackend::openDevice() {
    if (m_device)
        return true;
    Status status = m_api.openDeviceByIndex(0, &m_device);
    appendLog(QStringLiteral("TUSBAUDIO_OpenDeviceByIndex(0) -> %1 handle=0x%2")
                  .arg(statusHex(status))
                  .arg(reinterpret_cast<quintptr>(m_device), 0, 16));
    if (status != 0 || !m_device) {
        m_device = nullptr;
        updateStatus(QStringLiteral("OpenDeviceByIndex failed (%1)").arg(statusHex(status)));
        return false;
    }
    return true;
}

void BeacnBackend::closeDevice() {
    if (!m_device || !m_api.closeDevice)
        return;
    const Status status = m_api.closeDevice(m_device);
    appendLog(QStringLiteral("TUSBAUDIO_CloseDevice -> %1").arg(statusHex(status)));
    m_device = nullptr;
}

bool BeacnBackend::refreshDeviceInfo() {
    if (!m_connected || !m_device)
        return false;

    bool ok = true;

    const auto version = m_api.getApiVersion();
    const auto apiMajor = (version >> 16) & 0xffffu;
    const auto apiMinor = version & 0xffffu;
    m_apiVersion = QStringLiteral("%1.%2").arg(apiMajor).arg(apiMinor);
    appendLog(QStringLiteral("TUSBAUDIO API version %1 (0x%2)")
                  .arg(m_apiVersion)
                  .arg(version, 8, 16, QLatin1Char('0')));

    std::uint32_t rate = 0;
    const Status rateStatus = m_api.getCurrentSampleRate(m_device, &rate);
    if (rateStatus == 0)
        m_sampleRate = rate;
    else
        ok = false;
    appendLog(QStringLiteral("GetCurrentSampleRate -> %1, %2 Hz").arg(statusHex(rateStatus)).arg(rate));

    std::uint32_t mixerMajor = 0;
    std::uint32_t mixerMinor = 0;
    if (getInterfaceVersion(mixerMajor, mixerMinor)) {
        m_mixerMajor = mixerMajor;
        m_mixerMinor = mixerMinor;
        m_mixerVersion = QStringLiteral("%1.%2").arg(mixerMajor).arg(mixerMinor);
    } else {
        m_mixerMajor = 0;
        m_mixerMinor = 0;
        m_mixerVersion = QStringLiteral("query failed");
        ok = false;
    }

    if (m_mixerMajor != 2) {
        appendLog(
            QStringLiteral(
                "Private mixer interface major %1 is not supported by this version of WHAT; private mixer reads/writes disabled")
                .arg(m_mixerMajor));
        m_channelSummary = QStringLiteral("Unsupported private mixer interface %1").arg(m_mixerVersion);
        emit deviceInfoChanged();
        return false;
    }

    const std::array<std::pair<std::uint32_t, int *>, 6> countQueries{{
        {kDeviceOutCount, &m_deviceOutCount},
        {kDeviceInCount, &m_deviceInCount},
        {kPlaybackCount, &m_playbackCount},
        {kRecordCount, &m_recordCount},
        {kVirtualPlaybackCount, &m_virtualPlaybackCount},
        {kVirtualRecordCount, &m_virtualRecordCount},
    }};
    for (const auto &[property, target] : countQueries) {
        if (!getChannelCount(property, *target))
            ok = false;
    }

    m_channelSummary =
        QStringLiteral(
            "Dev out %1 · Dev in %2 · Playback %3 · Record %4 · Virtual playback %5 · Virtual record %6")
            .arg(m_deviceOutCount)
            .arg(m_deviceInCount)
            .arg(m_playbackCount)
            .arg(m_recordCount)
            .arg(m_virtualPlaybackCount)
            .arg(m_virtualRecordCount);

    rebuildSourceNames();
    rebuildBusNames();
    emit deviceInfoChanged();
    return ok;
}

bool BeacnBackend::getInterfaceVersion(std::uint32_t &major, std::uint32_t &minor) {
    InterfaceVersionRequest request;
    const Status status = m_api.getDspProperty(m_device, &request, sizeof(request));
    appendLog(QStringLiteral("Mixer InterfaceVersion -> %1, %2.%3")
                  .arg(statusHex(status))
                  .arg(request.major)
                  .arg(request.minor));
    if (status != 0)
        return false;
    major = request.major;
    minor = request.minor;
    return true;
}

bool BeacnBackend::getChannelCount(std::uint32_t propertyId, int &count) {
    CountRequest request;
    request.property = propertyId;
    const Status status = m_api.getDspProperty(m_device, &request, sizeof(request));
    appendLog(QStringLiteral("Mixer count property 0x%1 -> %2, count=%3")
                  .arg(propertyId, 0, 16)
                  .arg(statusHex(status))
                  .arg(request.count));
    if (status != 0)
        return false;
    count = static_cast<int>(request.count);
    return true;
}

BeacnBackend::RouteValue BeacnBackend::readWeight(int inputPin, int inputIndex, int outputPin,
                                                  int outputIndex, bool logIo) {
    RouteValue value;
    if (!m_device || !m_api.getDspProperty) {
        value.status = 0xffffffffu;
        return value;
    }

    if (inputIndex < 0 || inputIndex > 255 || outputIndex < 0 || outputIndex > 255) {
        value.status = 0xfffffffeu;
        return value;
    }

    WeightRequest request;
    request.inputPin = static_cast<std::uint8_t>(storedPin(inputPin));
    request.inputIndex = static_cast<std::uint8_t>(inputIndex);
    request.outputPin = static_cast<std::uint8_t>(storedPin(outputPin));
    request.outputIndex = static_cast<std::uint8_t>(outputIndex);

    value.status = m_api.getDspProperty(m_device, &request, sizeof(request));
    value.raw = request.rawGain;

    if (logIo || (m_verboseLogging && !m_recordTick) || value.status != 0) {
        appendLog(QStringLiteral("GET %1 -> %2 raw=%3 fader=%4% dB=%5")
                      .arg(routeText(inputPin, inputIndex, outputPin, outputIndex))
                      .arg(statusHex(value.status))
                      .arg(value.raw)
                      .arg(rawToPercent(value.raw), 0, 'f', 1)
                      .arg(rawToDb(value.raw), 0, 'f', 3));
    }
    return value;
}

bool BeacnBackend::writeWeight(int inputPin, int inputIndex, int outputPin, int outputIndex, std::int32_t raw,
                               bool logIo) {
    if (!m_writesArmed) {
        appendLog(QStringLiteral("WRITE REFUSED (writes disarmed): %1")
                      .arg(routeText(inputPin, inputIndex, outputPin, outputIndex)));
        return false;
    }
    if (!m_device || !m_api.setDspProperty)
        return false;
    if (inputIndex < 0 || inputIndex > 255 || outputIndex < 0 || outputIndex > 255)
        return false;

    WeightRequest request;
    request.inputPin = static_cast<std::uint8_t>(storedPin(inputPin));
    request.inputIndex = static_cast<std::uint8_t>(inputIndex);
    request.outputPin = static_cast<std::uint8_t>(storedPin(outputPin));
    request.outputIndex = static_cast<std::uint8_t>(outputIndex);
    request.rawGain = raw;

    const Status status = m_api.setDspProperty(m_device, &request, sizeof(request));
    if (logIo || m_verboseLogging || status != 0) {
        appendLog(QStringLiteral("SET %1 raw=%2 fader=%3% dB=%4 -> %5")
                      .arg(routeText(inputPin, inputIndex, outputPin, outputIndex))
                      .arg(raw)
                      .arg(rawToPercent(raw), 0, 'f', 1)
                      .arg(rawToDb(raw), 0, 'f', 3)
                      .arg(statusHex(status)));
    }
    return status == 0;
}

QVariantList BeacnBackend::sourceLevels() const {
    const int sourcePairs = m_virtualPlaybackCount > 0 ? m_virtualPlaybackCount / 2 : m_sourceNames.size();
    const int busPairs = m_virtualRecordCount > 0 ? m_virtualRecordCount / 2 : m_busNames.size();
    QVariantList levels;
    levels.reserve(sourcePairs);

    for (int source = 0; source < sourcePairs; ++source) {
        double sum = 0.0;
        int count = 0;
        const quint32 mask = source < m_sourceRouteMasks.size() ? m_sourceRouteMasks.at(source) : 0u;

        for (int bus = 0; bus < busPairs; ++bus) {
            if (mask != 0u && (mask & (quint32(1) << bus)) == 0u)
                continue;
            const int idx = source * busPairs + bus;
            if (idx < 0 || idx >= m_matrix.size())
                continue;
            const QVariantMap cell = m_matrix.at(idx).toMap();
            if (!cell.value(QStringLiteral("valid")).toBool())
                continue;
            const auto leftRaw = cell.value(QStringLiteral("leftRaw")).toInt();
            const auto rightRaw = cell.value(QStringLiteral("rightRaw")).toInt();
            if (leftRaw <= 0 && rightRaw <= 0)
                continue;
            sum += (cell.value(QStringLiteral("leftPercent")).toDouble() +
                    cell.value(QStringLiteral("rightPercent")).toDouble()) /
                   2.0;
            ++count;
        }

        levels.append(count > 0 ? sum / count : 0.0);
    }
    return levels;
}

bool BeacnBackend::refreshMatrix() {
    if (!m_connected || !m_device || m_mixerMajor != 2)
        return false;

    const int sourcePairs = m_virtualPlaybackCount / 2;
    const int busPairs = m_virtualRecordCount / 2;
    QVariantList next;
    next.reserve(sourcePairs * busPairs);
    int failures = 0;
    QVector<quint32> observedMasks(sourcePairs, 0u);
    if (m_sourceRouteMasks.size() != sourcePairs)
        m_sourceRouteMasks.resize(sourcePairs);

    for (int source = 0; source < sourcePairs; ++source) {
        const int inLeft = source * 2;
        const int inRight = inLeft + 1;
        for (int bus = 0; bus < busPairs; ++bus) {
            const int outLeft = bus * 2;
            const int outRight = outLeft + 1;
            const auto left = readWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft);
            const auto right = readWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight);
            if (left.status != 0 || right.status != 0)
                ++failures;
            else if (left.raw > 0 || right.raw > 0)
                observedMasks[source] |= (quint32(1) << bus);

            QVariantMap cell;
            cell.insert(QStringLiteral("sourcePair"), source);
            cell.insert(QStringLiteral("busPair"), bus);
            cell.insert(QStringLiteral("leftRaw"), left.raw);
            cell.insert(QStringLiteral("rightRaw"), right.raw);
            cell.insert(QStringLiteral("leftDb"), rawToDb(left.raw));
            cell.insert(QStringLiteral("rightDb"), rawToDb(right.raw));
            cell.insert(QStringLiteral("leftPercent"), rawToPercent(left.raw));
            cell.insert(QStringLiteral("rightPercent"), rawToPercent(right.raw));
            cell.insert(QStringLiteral("valid"), left.status == 0 && right.status == 0);
            next.append(cell);
        }
    }

    for (int source = 0; source < sourcePairs; ++source) {
        // If at least one route is live, this is the best current routing mask.
        // If every route is zero (for example a source fader at 0%), preserve
        // the previous mask so the source can be raised again later.
        if (observedMasks.at(source) != 0u || m_sourceRouteMasks.at(source) == 0u)
            m_sourceRouteMasks[source] = observedMasks.at(source);
    }

    m_matrix = next;
    emit matrixChanged();
    autoResolveSourceNames();
    if (!m_recordTick || failures != 0)
        appendLog(QStringLiteral("Matrix refresh: %1 source pairs × %2 buses, %3 failed cells")
                      .arg(sourcePairs)
                      .arg(busPairs)
                      .arg(failures));
    return failures == 0;
}

QVariantMap BeacnBackend::readStereoRoute(int sourcePair, int busPair) {
    QVariantMap result;
    if (!m_connected || sourcePair < 0 || busPair < 0)
        return result;

    const int inLeft = sourcePair * 2;
    const int inRight = inLeft + 1;
    const int outLeft = busPair * 2;
    const int outRight = outLeft + 1;

    const auto left = readWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, true);
    const auto right = readWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight, true);

    result.insert(QStringLiteral("valid"), left.status == 0 && right.status == 0);
    result.insert(QStringLiteral("leftRaw"), left.raw);
    result.insert(QStringLiteral("rightRaw"), right.raw);
    result.insert(QStringLiteral("leftDb"), rawToDb(left.raw));
    result.insert(QStringLiteral("rightDb"), rawToDb(right.raw));
    result.insert(QStringLiteral("averageDb"), (rawToDb(left.raw) + rawToDb(right.raw)) / 2.0);
    result.insert(QStringLiteral("leftPercent"), rawToPercent(left.raw));
    result.insert(QStringLiteral("rightPercent"), rawToPercent(right.raw));
    result.insert(QStringLiteral("averagePercent"), (rawToPercent(left.raw) + rawToPercent(right.raw)) / 2.0);
    return result;
}

bool BeacnBackend::setStereoRouteDb(int sourcePair, int busPair, double db) {
    if (!m_connected || !m_writesArmed) {
        appendLog(
            QStringLiteral("Stereo write refused: %1")
                .arg(!m_connected ? QStringLiteral("not connected") : QStringLiteral("writes disarmed")));
        return false;
    }
    if (m_mixerMajor != 2) {
        appendLog(QStringLiteral("Stereo write refused: unsupported private mixer interface %1")
                      .arg(m_mixerVersion));
        return false;
    }

    if (sourcePair < 0 || sourcePair * 2 + 1 >= m_virtualPlaybackCount || busPair < 0 ||
        busPair * 2 + 1 >= m_virtualRecordCount) {
        appendLog(QStringLiteral("Stereo write refused: selection out of range"));
        return false;
    }

    db = std::clamp(db, -120.0, 6.0);
    const std::int32_t target = dbToRaw(db);
    const int inLeft = sourcePair * 2;
    const int inRight = inLeft + 1;
    const int outLeft = busPair * 2;
    const int outRight = outLeft + 1;

    const auto originalLeft = readWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, true);
    const auto originalRight = readWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight, true);
    if (originalLeft.status != 0 || originalRight.status != 0) {
        appendLog(QStringLiteral("Stereo write aborted: could not read original values"));
        return false;
    }

    appendLog(QStringLiteral("Applying stereo route sourcePair=%1 bus=%2 target=%3 dB raw=%4")
                  .arg(sourcePair)
                  .arg(busPair)
                  .arg(db, 0, 'f', 2)
                  .arg(target));

    if (!writeWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, target))
        return false;

    if (!writeWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight, target)) {
        appendLog(QStringLiteral("Right-channel write failed; restoring left channel"));
        writeWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, originalLeft.raw);
        return false;
    }

    const auto verifyLeft = readWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, true);
    const auto verifyRight = readWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight, true);
    const bool verified = verifyLeft.status == 0 && verifyRight.status == 0 && verifyLeft.raw == target &&
                          verifyRight.raw == target;

    appendLog(QStringLiteral("Stereo write verification: %1")
                  .arg(verified ? QStringLiteral("PASS") : QStringLiteral("FAILED")));
    refreshMatrix();
    refreshProfile();
    return verified;
}

bool BeacnBackend::setStereoRoutePercent(int sourcePair, int busPair, double percent) {
    if (!m_connected || !m_writesArmed) {
        appendLog(QStringLiteral("Stereo percent write refused: %1")
                      .arg(!m_connected ? QStringLiteral("not connected")
                                        : QStringLiteral("live control disabled")));
        return false;
    }
    if (m_mixerMajor != 2) {
        appendLog(QStringLiteral("Stereo percent write refused: unsupported private mixer interface %1")
                      .arg(m_mixerVersion));
        return false;
    }

    if (sourcePair < 0 || sourcePair * 2 + 1 >= m_virtualPlaybackCount || busPair < 0 ||
        busPair * 2 + 1 >= m_virtualRecordCount) {
        appendLog(QStringLiteral("Stereo percent write refused: selection out of range"));
        return false;
    }

    percent = std::clamp(percent, 0.0, 100.0);
    const std::int32_t target = percentToRaw(percent);
    const int inLeft = sourcePair * 2;
    const int inRight = inLeft + 1;
    const int outLeft = busPair * 2;
    const int outRight = outLeft + 1;

    const auto originalLeft = readWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, true);
    const auto originalRight = readWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight, true);
    if (originalLeft.status != 0 || originalRight.status != 0) {
        appendLog(QStringLiteral("Stereo percent write aborted: could not read original values"));
        return false;
    }

    appendLog(QStringLiteral("Applying BEACN fader sourcePair=%1 bus=%2 target=%3% raw=%4")
                  .arg(sourcePair)
                  .arg(busPair)
                  .arg(percent, 0, 'f', 1)
                  .arg(target));

    if (!writeWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, target))
        return false;

    if (!writeWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight, target)) {
        appendLog(QStringLiteral("Right-channel percent write failed; restoring left channel"));
        writeWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, originalLeft.raw);
        return false;
    }

    const auto verifyLeft = readWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, true);
    const auto verifyRight = readWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight, true);
    const bool verified = verifyLeft.status == 0 && verifyRight.status == 0 && verifyLeft.raw == target &&
                          verifyRight.raw == target;

    appendLog(QStringLiteral("BEACN fader write verification: %1 (%2%)")
                  .arg(verified ? QStringLiteral("PASS") : QStringLiteral("FAILED"))
                  .arg(percent, 0, 'f', 1));
    refreshMatrix();
    refreshProfile();
    return verified;
}

bool BeacnBackend::setSourcePercent(int sourcePair, double percent) {
    if (!m_connected || !m_writesArmed) {
        appendLog(QStringLiteral("Source fader write refused: %1")
                      .arg(!m_connected ? QStringLiteral("not connected")
                                        : QStringLiteral("live control disabled")));
        return false;
    }
    if (m_mixerMajor != 2) {
        appendLog(QStringLiteral("Source fader write refused: unsupported private mixer interface %1")
                      .arg(m_mixerVersion));
        return false;
    }

    const int sourcePairs = m_virtualPlaybackCount / 2;
    const int busPairs = m_virtualRecordCount / 2;
    if (sourcePair < 0 || sourcePair >= sourcePairs) {
        appendLog(QStringLiteral("Source fader write refused: source %1 out of range").arg(sourcePair));
        return false;
    }

    if (m_sourceRouteMasks.size() != sourcePairs)
        refreshMatrix();
    quint32 mask = sourcePair < m_sourceRouteMasks.size() ? m_sourceRouteMasks.at(sourcePair) : 0u;

    // If this is the first time we see the source and it is currently at zero,
    // use profile mute state as a conservative routing fallback for known sources.
    if (mask == 0u) {
        const QString sourceName =
            sourcePair < m_sourceNames.size() ? m_sourceNames.at(sourcePair) : QString();
        for (const QVariant &entry : m_profileMixers) {
            const QVariantMap row = entry.toMap();
            if (row.value(QStringLiteral("name")).toString().compare(sourceName, Qt::CaseInsensitive) != 0)
                continue;
            if (!row.value(QStringLiteral("audienceMuted")).toBool() && busPairs > 0)
                mask |= 1u << 0;
            if (!row.value(QStringLiteral("personalMuted")).toBool() && busPairs > 1)
                mask |= 1u << 1;
            if (!row.value(QStringLiteral("chatMuted")).toBool() && busPairs > 2)
                mask |= 1u << 2;
            if (!row.value(QStringLiteral("vod1Muted")).toBool() && busPairs > 3)
                mask |= 1u << 3;
            if (!row.value(QStringLiteral("vod2Muted")).toBool() && busPairs > 4)
                mask |= 1u << 4;
            if (!row.value(QStringLiteral("vod3Muted")).toBool() && busPairs > 5)
                mask |= 1u << 5;
            break;
        }
    }

    if (mask == 0u) {
        appendLog(QStringLiteral("Source fader write refused: no active route mask known for source %1 (%2)")
                      .arg(sourcePair)
                      .arg(sourcePair < m_sourceNames.size() ? m_sourceNames.at(sourcePair)
                                                             : QStringLiteral("unknown")));
        return false;
    }

    percent = std::clamp(percent, 0.0, 100.0);
    const std::int32_t target = percentToRaw(percent);
    const int inLeft = sourcePair * 2;
    const int inRight = inLeft + 1;

    struct OriginalRoute {
        int bus = 0;
        RouteValue left;
        RouteValue right;
    };

    QVector<OriginalRoute> originals;

    for (int bus = 0; bus < busPairs; ++bus) {
        if ((mask & (quint32(1) << bus)) == 0u)
            continue;
        const int outLeft = bus * 2;
        const int outRight = outLeft + 1;
        const auto left = readWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, true);
        const auto right = readWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight, true);
        if (left.status != 0 || right.status != 0) {
            appendLog(
                QStringLiteral("Source fader write aborted: failed to read source %1 bus %2 before write")
                    .arg(sourcePair)
                    .arg(bus));
            return false;
        }
        originals.append({bus, left, right});
    }

    appendLog(
        QStringLiteral("Applying BEACN source fader sourcePair=%1 name=%2 target=%3% raw=%4 routesMask=0x%5")
            .arg(sourcePair)
            .arg(sourcePair < m_sourceNames.size() ? m_sourceNames.at(sourcePair) : QStringLiteral("unknown"))
            .arg(percent, 0, 'f', 1)
            .arg(target)
            .arg(mask, 0, 16));

    int writtenRoutes = 0;
    for (const OriginalRoute &original : originals) {
        const int outLeft = original.bus * 2;
        const int outRight = outLeft + 1;
        if (!writeWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, target) ||
            !writeWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight, target)) {
            appendLog(QStringLiteral("Source fader write failed; rolling back %1 already-written route(s)")
                          .arg(writtenRoutes));
            for (int i = 0; i <= writtenRoutes && i < originals.size(); ++i) {
                const OriginalRoute &restore = originals.at(i);
                const int restoreLeft = restore.bus * 2;
                const int restoreRight = restoreLeft + 1;
                writeWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, restoreLeft, restore.left.raw);
                writeWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, restoreRight, restore.right.raw);
            }
            refreshMatrix();
            return false;
        }
        ++writtenRoutes;
    }

    bool verified = true;
    for (const OriginalRoute &original : originals) {
        const int outLeft = original.bus * 2;
        const int outRight = outLeft + 1;
        const auto left = readWeight(kVirtualPlaybackPin, inLeft, kVirtualRecordPin, outLeft, true);
        const auto right = readWeight(kVirtualPlaybackPin, inRight, kVirtualRecordPin, outRight, true);
        if (left.status != 0 || right.status != 0 || left.raw != target || right.raw != target) {
            verified = false;
            break;
        }
    }

    appendLog(QStringLiteral("BEACN source fader verification: %1 (%2%, %3 stereo bus route(s))")
                  .arg(verified ? QStringLiteral("PASS") : QStringLiteral("FAILED"))
                  .arg(percent, 0, 'f', 1)
                  .arg(originals.size()));
    refreshMatrix();
    return verified;
}

QHash<BeacnBackend::RouteKey, BeacnBackend::RouteValue> BeacnBackend::snapshotAllRoutes(bool *ok) {
    QHash<RouteKey, RouteValue> snapshot;
    bool allOk = true;

    const std::array<std::pair<int, int>, 3> inputs{{{kPlaybackPin, m_playbackCount},
                                                     {kDeviceInputPin, m_deviceInCount},
                                                     {kVirtualPlaybackPin, m_virtualPlaybackCount}}};
    const std::array<std::pair<int, int>, 3> outputs{{{kRecordPin, m_recordCount},
                                                      {kDeviceOutputPin, m_deviceOutCount},
                                                      {kVirtualRecordPin, m_virtualRecordCount}}};

    qsizetype expected = 0;
    for (const auto &[inPin, inCount] : inputs) {
        Q_UNUSED(inPin);
        for (const auto &[outPin, outCount] : outputs) {
            Q_UNUSED(outPin);
            expected += inCount * outCount;
        }
    }
    if (expected > 8192) {
        appendLog(
            QStringLiteral("Full snapshot refused: %1 potential routes exceeds safety cap").arg(expected));
        if (ok)
            *ok = false;
        return snapshot;
    }

    for (const auto &[inPin, inCount] : inputs) {
        for (int inIndex = 0; inIndex < inCount; ++inIndex) {
            for (const auto &[outPin, outCount] : outputs) {
                for (int outIndex = 0; outIndex < outCount; ++outIndex) {
                    RouteKey key{inPin, inIndex, outPin, outIndex};
                    const auto value = readWeight(inPin, inIndex, outPin, outIndex);
                    if (value.status != 0)
                        allOk = false;
                    snapshot.insert(key, value);
                }
            }
        }
    }

    if (ok)
        *ok = allOk;
    return snapshot;
}

bool BeacnBackend::captureBaseline() {
    if (!m_connected)
        return false;
    bool ok = false;
    m_baseline = snapshotAllRoutes(&ok);
    m_detectedSourcePair = -1;
    m_detectionSummary = QStringLiteral(
        "Snapshot captured. Now move one physical BEACN source control, then click Detect change.");
    m_diffSummary =
        QStringLiteral("Snapshot captured: %1 routes (%2).")
            .arg(m_baseline.size())
            .arg(ok ? QStringLiteral("all reads successful") : QStringLiteral("some reads failed"));
    appendLog(m_diffSummary);
    emit diffSummaryChanged();
    emit detectionChanged();
    return !m_baseline.isEmpty();
}

bool BeacnBackend::compareBaseline() {
    if (!m_connected || m_baseline.isEmpty())
        return false;

    bool ok = false;
    const auto current = snapshotAllRoutes(&ok);
    QStringList changes;
    QSet<int> changedSourcePairs;

    for (auto it = current.constBegin(); it != current.constEnd(); ++it) {
        const auto beforeIt = m_baseline.constFind(it.key());
        if (beforeIt == m_baseline.constEnd())
            continue;
        const RouteValue &before = beforeIt.value();
        const RouteValue &after = it.value();
        if (before.status != after.status || before.raw != after.raw) {
            changes.append(QStringLiteral("%1: %2 (%3 dB) → %4 (%5 dB)")
                               .arg(routeText(it.key().inputPin, it.key().inputIndex, it.key().outputPin,
                                              it.key().outputIndex))
                               .arg(before.raw)
                               .arg(rawToDb(before.raw), 0, 'f', 2)
                               .arg(after.raw)
                               .arg(rawToDb(after.raw), 0, 'f', 2));
            if (it.key().inputPin == kVirtualPlaybackPin)
                changedSourcePairs.insert(it.key().inputIndex / 2);
        }
    }

    if (changes.isEmpty()) {
        m_diffSummary = QStringLiteral("No route changes detected (%1 routes scanned).").arg(current.size());
        m_detectedSourcePair = -1;
        m_detectionSummary = QStringLiteral(
            "No source change detected. Capture a new snapshot and move one physical source control more noticeably.");
    } else {
        m_diffSummary = QStringLiteral("%1 changed route(s):\n%2")
                            .arg(changes.size())
                            .arg(changes.join(QLatin1Char('\n')));
        if (changedSourcePairs.size() == 1) {
            m_detectedSourcePair = *changedSourcePairs.constBegin();
            const int left = m_detectedSourcePair * 2;
            m_detectionSummary =
                QStringLiteral(
                    "Detected source pair VP %1/%2. Give it a name below (for example Game, Music or Browser).")
                    .arg(left)
                    .arg(left + 1);
        } else if (changedSourcePairs.size() > 1) {
            m_detectedSourcePair = -1;
            QStringList pairs;
            QList<int> sorted = changedSourcePairs.values();
            std::sort(sorted.begin(), sorted.end());
            for (int pair : sorted)
                pairs.append(QStringLiteral("VP %1/%2").arg(pair * 2).arg(pair * 2 + 1));
            m_detectionSummary =
                QStringLiteral(
                    "More than one source changed (%1). Capture again and move only one physical source control.")
                    .arg(pairs.join(QStringLiteral(", ")));
        } else {
            m_detectedSourcePair = -1;
            m_detectionSummary = QStringLiteral(
                "A driver route changed, but it was not a virtual playback source. The raw diff is in Diagnostics.");
        }
    }

    appendLog(QStringLiteral("Snapshot compare: %1 changes, reads %2")
                  .arg(changes.size())
                  .arg(ok ? QStringLiteral("OK") : QStringLiteral("with failures")));
    for (const QString &line : changes)
        appendLog(QStringLiteral("DIFF %1").arg(line));
    emit diffSummaryChanged();
    emit detectionChanged();
    refreshMatrix();
    refreshProfile();
    return true;
}

void BeacnBackend::refreshProfile() {
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QDir dir(docs + QStringLiteral("/BEACN/profiles/MixerProfiles"));
    if (!dir.exists()) {
        m_profilePath.clear();
        m_profileMixers.clear();
        emit profileChanged();
        return;
    }

    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.beacnMixer")}, QDir::Files, QDir::Time);
    if (files.isEmpty()) {
        m_profilePath.clear();
        m_profileMixers.clear();
        emit profileChanged();
        return;
    }

    m_profilePath = files.constFirst().absoluteFilePath();
    const QString liveProfile = m_globalState.value("profile").toString();
    if (!liveProfile.isEmpty()) {
        m_profilePath.clear();
        for (const auto &entry : files)
            if (entry.completeBaseName() == liveProfile) {
                m_profilePath = entry.absoluteFilePath();
                break;
            }
        // Never use a different profile's mappings when the live profile has no saved file.
    }
    QFile file(m_profilePath);
    QVariantList mixers;

    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QXmlStreamReader xml(&file);
        bool inPrimaryMixerTree = false;
        int mixerTreeDepth = 0;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement()) {
                const QStringView name = xml.name();
                if (!inPrimaryMixerTree && name == u"mixerTree") {
                    inPrimaryMixerTree = true;
                    mixerTreeDepth = 1;
                    continue;
                }
                if (inPrimaryMixerTree) {
                    ++mixerTreeDepth;
                    const QString n = name.toString();
                    if (n.startsWith(QStringLiteral("mixer")) && n != QStringLiteral("mixerTree")) {
                        const auto a = xml.attributes();
                        QVariantMap row;
                        row.insert(QStringLiteral("name"), a.value(QStringLiteral("mixerName")).toString());
                        row.insert(QStringLiteral("knobType"),
                                   a.value(QStringLiteral("bartonKnobType")).toInt());
                        row.insert(QStringLiteral("place"),
                                   a.value(QStringLiteral("mixerPlaceInOrder")).toInt());
                        row.insert(QStringLiteral("linked"),
                                   a.value(QStringLiteral("mixerFadersLinked")) == u"1");
                        row.insert(QStringLiteral("personal"),
                                   a.value(QStringLiteral("mixerHPLevel")).toDouble());
                        row.insert(QStringLiteral("audience"),
                                   a.value(QStringLiteral("mixerBroadcastLevel")).toDouble());
                        row.insert(QStringLiteral("personalMuted"),
                                   a.value(QStringLiteral("mixerHPMute")) == u"1");
                        row.insert(QStringLiteral("audienceMuted"),
                                   a.value(QStringLiteral("mixerBroadcastMute")) == u"1");
                        row.insert(QStringLiteral("chatMuted"),
                                   a.value(QStringLiteral("mixerChatMute")) == u"1");
                        row.insert(QStringLiteral("vod1Muted"),
                                   a.value(QStringLiteral("mixerVodTrackMute")) == u"1");
                        row.insert(QStringLiteral("vod2Muted"),
                                   a.value(QStringLiteral("mixerVodTrack2Mute")) == u"1");
                        row.insert(QStringLiteral("vod3Muted"),
                                   a.value(QStringLiteral("mixerVodTrack3Mute")) == u"1");
                        if (!row.value(QStringLiteral("name")).toString().isEmpty())
                            mixers.append(row);
                    }
                }
            } else if (xml.isEndElement() && inPrimaryMixerTree) {
                --mixerTreeDepth;
                if (mixerTreeDepth <= 0)
                    break;
            }
        }
    }

    m_profileMixers = mixers;
    emit profileChanged();
    autoResolveSourceNames();
    appendLog(QStringLiteral("Profile refresh: %1 (%2 mixer rows)").arg(m_profilePath).arg(mixers.size()));
}

void BeacnBackend::setSourceAlias(int sourcePair, const QString &name) {
    if (sourcePair < 0)
        return;
    const QString cleaned = name.trimmed();
    if (cleaned.isEmpty())
        m_sourceAliases.remove(sourcePair);
    else
        m_sourceAliases.insert(sourcePair, cleaned);

    QSettings settings;
    settings.beginGroup("modules/beacn/backend");
    const QString key = QStringLiteral("sourceAlias/%1").arg(sourcePair);
    if (cleaned.isEmpty())
        settings.remove(key);
    else
        settings.setValue(key, cleaned);
    rebuildSourceNames();
}

#ifdef Q_OS_WIN
bool BeacnBackend::readMixCreateParameters(QHash<quint32, quint32> &values, QString *errorText,
                                           bool verifyNoopWrite) {
    values.clear();

    // Writing to the Mix Create over USB froze its screen, so the verification
    // write is disabled and this path never opens a USB handle.
    if (verifyNoopWrite) {
        const QString error = QStringLiteral(
            "USB write testing is disabled while investigating the device-screen freeze. No USB handle was opened.");
        appendLog(error);
        if (errorText)
            *errorText = error;
        return false;
    }
    QStringList details;
    const bool ok = readBeacnCachedParameterTable(values, &details, errorText);
    for (const QString &detail : details)
        appendLog(detail);
    if (!ok)
        return false;
    m_winUsbPath = QStringLiteral("BEACN.exe parameter cache [process-memory reads only]");
    m_winUsbGuid.clear();
    appendLog(QStringLiteral("BEACN cache read PASS (%1 values); no USB handle opened.").arg(values.size()));
    return true;
}
#endif

bool BeacnBackend::probeMixCreateParameters() {
#ifdef Q_OS_WIN
    QHash<quint32, quint32> values;
    QString error;
    m_winUsbStatus = QStringLiteral("Probing the Mix Create WinUSB parameter interface…");
    emit winUsbProbeChanged();
    if (!readMixCreateParameters(values, &error)) {
        m_winUsbStatus = QStringLiteral("Probe failed: %1").arg(error);
        emit winUsbProbeChanged();
        return false;
    }

    m_winUsbParameters = makeWinUsbParameterRows(values, m_sourceNames);
    m_winUsbStatus =
        QStringLiteral("Read %1 Mix Create parameters from BEACN cache without consuming endpoint 0x83.")
            .arg(values.size());
    appendLog(
        QStringLiteral(
            "Mix Create parameter probe PASS (%1 values; BEACN cache preferred, no shared IN reads when available)")
            .arg(values.size()));
    emit winUsbProbeChanged();
    return true;
#else
    m_winUsbStatus = QStringLiteral("Mix Create WinUSB probing is Windows-only.");
    emit winUsbProbeChanged();
    return false;
#endif
}

bool BeacnBackend::verifyMixCreateParameterWritePath() {
#ifdef Q_OS_WIN
    QHash<quint32, quint32> values;
    QString error;
    m_winUsbStatus = QStringLiteral("Verifying Mix Create SetParameter path with a no-op write…");
    emit winUsbProbeChanged();
    if (!readMixCreateParameters(values, &error, true)) {
        m_winUsbStatus = QStringLiteral("No-op hardware write test failed: %1").arg(error);
        emit winUsbProbeChanged();
        return false;
    }

    m_winUsbParameters = makeWinUsbParameterRows(values, m_sourceNames);
    m_winUsbStatus =
        QStringLiteral("Write-only no-op SetParameter sent. BEACN remains the sole endpoint 0x83 reader.");
    appendLog(QStringLiteral("WINUSB write-only no-op hardware verification PASS."));
    emit winUsbProbeChanged();
    return true;
#else
    m_winUsbStatus = QStringLiteral("Mix Create WinUSB probing is Windows-only.");
    emit winUsbProbeChanged();
    return false;
#endif
}

bool BeacnBackend::captureMixCreateParameterBaseline() {
#ifdef Q_OS_WIN
    QHash<quint32, quint32> values;
    QString error;
    m_winUsbStatus = QStringLiteral("Capturing Mix Create hardware baseline…");
    emit winUsbProbeChanged();
    if (!readMixCreateParameters(values, &error)) {
        m_winUsbStatus = QStringLiteral("Baseline failed: %1").arg(error);
        emit winUsbProbeChanged();
        return false;
    }
    m_winUsbBaseline = values;
    m_winUsbDiffSummary = QStringLiteral(
        "Baseline captured from BEACN cache. Move exactly ONE physical BEACN fader, then click Compare.");
    m_winUsbParameters = makeWinUsbParameterRows(values, m_sourceNames);
    m_winUsbStatus =
        QStringLiteral("Baseline captured from %1 BEACN-cached Mix Create parameters.").arg(values.size());
    appendLog(QStringLiteral("BEACN cached parameter baseline captured (%1 values)").arg(values.size()));
    emit winUsbProbeChanged();
    return true;
#else
    return false;
#endif
}

bool BeacnBackend::compareMixCreateParameters() {
#ifdef Q_OS_WIN
    if (m_winUsbBaseline.isEmpty()) {
        m_winUsbDiffSummary = QStringLiteral("Capture a hardware parameter baseline first.");
        emit winUsbProbeChanged();
        return false;
    }

    QHash<quint32, quint32> values;
    QString error;
    m_winUsbStatus = QStringLiteral("Comparing Mix Create hardware parameters…");
    emit winUsbProbeChanged();
    if (!readMixCreateParameters(values, &error)) {
        m_winUsbStatus = QStringLiteral("Compare failed: %1").arg(error);
        emit winUsbProbeChanged();
        return false;
    }

    QStringList changes;
    int perChannelChanges = 0;
    quint32 onlyChannelId = 0;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const quint32 id = it.key();
        const quint32 before = m_winUsbBaseline.value(id, it.value());
        const quint32 after = it.value();
        if (before == after)
            continue;

        QString label = QStringLiteral("0x%1").arg(id, 3, 16, QLatin1Char('0')).toUpper();
        if ((id & 0xffu) == 0x01u && (id >> 8u) < 8u) {
            const int sourcePair = static_cast<int>(id >> 8u);
            const QString source = sourcePair < m_sourceNames.size()
                                       ? m_sourceNames.at(sourcePair)
                                       : QStringLiteral("Source %1").arg(sourcePair + 1);
            label += QStringLiteral(" (slot %1; mapping unconfirmed)").arg(sourcePair);
            ++perChannelChanges;
            onlyChannelId = id;
        }
        changes.append(QStringLiteral("%1: %2 (0x%3) -> %4 (0x%5)")
                           .arg(label)
                           .arg(before)
                           .arg(before, 8, 16, QLatin1Char('0'))
                           .arg(after)
                           .arg(after, 8, 16, QLatin1Char('0')));
    }

    if (changes.isEmpty()) {
        m_winUsbDiffSummary = QStringLiteral(
            "No probed hardware parameter changed. Try a larger physical fader move, then Compare again without recapturing baseline.");
    } else if (changes.size() == 1 && perChannelChanges == 1) {
        const int sourcePair = static_cast<int>(onlyChannelId >> 8u);
        const QString source = sourcePair < m_sourceNames.size()
                                   ? m_sourceNames.at(sourcePair)
                                   : QStringLiteral("Source %1").arg(sourcePair + 1);
        m_winUsbDiffSummary =
            QStringLiteral(
                "ONE parameter changed: 0x%1 = %2. Source mapping and value meaning remain unconfirmed.\n%3")
                .arg(onlyChannelId, 3, 16, QLatin1Char('0'))
                .arg(values.value(onlyChannelId))
                .arg(changes.first());
    } else {
        m_winUsbDiffSummary = QStringLiteral("%1 parameter(s) changed:\n%2")
                                  .arg(changes.size())
                                  .arg(changes.join(QLatin1Char('\n')));
    }

    for (const QString &change : changes)
        appendLog(QStringLiteral("WINUSB DIFF %1").arg(change));
    appendLog(QStringLiteral("WINUSB compare result: %1").arg(m_winUsbDiffSummary));

    // Keep the comparison against the original baseline until the user explicitly captures again.
    m_winUsbParameters = makeWinUsbParameterRows(values, m_sourceNames);
    m_winUsbStatus = QStringLiteral("Compare read %1 hardware parameters successfully.").arg(values.size());
    emit winUsbProbeChanged();
    return true;
#else
    return false;
#endif
}

void BeacnBackend::clearLogView() {
    emit logMessage(QStringLiteral("--- log view cleared; file logging continues ---"));
}

void BeacnBackend::autoResolveSourceNames() {
    QHash<int, QString> resolved;
    const int sourcePairCount = m_virtualPlaybackCount > 0 ? m_virtualPlaybackCount / 2 : 8;

    // BEACN's profile stores a stable source type (bartonKnobType). The common
    // Windows virtual sources map to stable TUSBAudio virtual-playback pairs.
    // We confirmed Game (type 5) == VP 4/5 on this device by moving the physical
    // BEACN control and diffing the live driver matrix.
    for (const QVariant &entry : m_profileMixers) {
        const QVariantMap row = entry.toMap();
        const QString name = row.value(QStringLiteral("name")).toString().trimmed();
        const int knobType = row.value(QStringLiteral("knobType")).toInt();
        if (name.isEmpty())
            continue;

        int pair = -1;
        switch (knobType) {
        case 1:
            pair = 7;
            break; // Mic / routed microphone input
        case 3:
            pair = 0;
            break; // Chat
        case 4:
            pair = 1;
            break; // Music
        case 5:
            pair = 2;
            break; // Game (physically confirmed)
        case 6:
            pair = 3;
            break; // Browser
        case 7:
            pair = 4;
            break; // System
        default:
            break; // Monitor is not a normal VP source pair
        }
        if (pair >= 0 && pair < sourcePairCount)
            resolved.insert(pair, name);
    }

    // Names for virtual sources that may exist even when they are not assigned
    // to one of the current physical BEACN faders/profile mixer rows.
    const std::array<std::pair<int, const char *>, 8> canonical{{{0, "Chat"},
                                                                 {1, "Music"},
                                                                 {2, "Game"},
                                                                 {3, "Browser"},
                                                                 {4, "System"},
                                                                 {5, "Aux 1"},
                                                                 {6, "Aux 2"},
                                                                 {7, "Mic"}}};
    for (const auto &[pair, label] : canonical) {
        if (pair < sourcePairCount && !resolved.contains(pair))
            resolved.insert(pair, QString::fromLatin1(label));
    }

    if (resolved != m_autoSourceAliases) {
        m_autoSourceAliases = resolved;
        appendLog(QStringLiteral("Resolved BEACN source names: %1").arg([&] {
            QStringList parts;
            for (int i = 0; i < sourcePairCount; ++i) {
                if (m_autoSourceAliases.contains(i))
                    parts << QStringLiteral("VP %1/%2=%3")
                                 .arg(i * 2)
                                 .arg(i * 2 + 1)
                                 .arg(m_autoSourceAliases.value(i));
            }
            return parts.join(QStringLiteral(", "));
        }()));
        rebuildSourceNames();
    }
}

void BeacnBackend::rebuildSourceNames() {
    const int count = m_virtualPlaybackCount > 0 ? m_virtualPlaybackCount / 2 : 8;
    QStringList names;
    for (int i = 0; i < count; ++i) {
        QString label = m_sourceAliases.value(i).trimmed();
        if (label.isEmpty())
            label = m_autoSourceAliases.value(i).trimmed();
        if (label.isEmpty())
            label = QStringLiteral("Unmapped source");
        names.append(label);
    }
    if (names != m_sourceNames) {
        m_sourceNames = names;
        emit sourceNamesChanged();
    }
}

void BeacnBackend::rebuildBusNames() {
    const int count = m_virtualRecordCount > 0 ? m_virtualRecordCount / 2 : 6;
    // Live matrix/profile comparison confirmed the first two driver buses are
    // Audience then Personal. Chat and the three VOD buses follow.
    static const QStringList inferred{QStringLiteral("Audience Mix"), QStringLiteral("Personal Mix"),
                                      QStringLiteral("Chat Mix"),     QStringLiteral("VOD 1"),
                                      QStringLiteral("VOD 2"),        QStringLiteral("VOD 3")};
    QStringList names;
    for (int i = 0; i < count; ++i) {
        if (i < inferred.size())
            names.append(inferred.at(i));
        else
            names.append(QStringLiteral("Bus %1").arg(i + 1));
    }
    m_busNames = names;
}

void BeacnBackend::updateStatus(const QString &text) {
    if (text == m_status)
        return;
    m_status = text;
    emit statusChanged();
}

int BeacnBackend::nativeSourceType(int sourcePair) {
    static constexpr int types[] = {3, 4, 5, 6, 7, -1, -1, 1};
    return sourcePair >= 0 && sourcePair < 8 ? types[sourcePair] : -1;
}

QString BeacnBackend::nativeSourceName(int sourcePair) const {
    const int type = nativeSourceType(sourcePair);
    if (type < 0)
        return {};
    QString name;
    for (const auto &entry : m_profileMixers) {
        const auto row = entry.toMap();
        if (row.value(QStringLiteral("knobType")).toInt() != type)
            continue;
        if (!name.isEmpty())
            return {};
        name = row.value(QStringLiteral("name")).toString();
    }
    return name;
}

void BeacnBackend::refreshNativeStates(bool allowDiscovery) {
    m_nativeFaders.clear();
    m_nativeBase = 0;
    m_nativeBirth = 0;
    m_globalMixer = m_globalProfiles = m_globalAssignments = m_globalOsd = m_globalSettings = 0;
    QString unavailable =
        m_connected ? "BEACN.exe is not running. Start the BEACN app, then retry." : m_status;
    QVariantList states;
    for (int i = 0; i < m_sourceNames.size(); ++i)
        states.append(QVariantMap{{QStringLiteral("valid"), false}});
#ifdef Q_OS_WIN
    // Cache addresses only; revalidate the object and its model on every sample.
    static thread_local DWORD cachedPid = 0;
    static thread_local quint64 cachedBirth = 0;
    static thread_local quintptr base = 0;
    static thread_local bool verified = false;
    static thread_local QString verificationError;
    static thread_local QString cachedSelectedPath;
    static thread_local QVector<quintptr> objects, globalMixers, globalProfiles, globalAssignments,
        globalOsds, globalSettings;
    static thread_local QElapsedTimer scanAge;
    const DWORD pid = m_connected ? findBeacnProcessId() : 0;
    HANDLE process = pid ? OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid) : nullptr;
    if (pid && !process)
        unavailable = "Cannot read the BEACN process: " + winErrorText(GetLastError()) +
                      ". Run WHAT and BEACN at the same privilege level.";
    if (process) {
        FILETIME birth{}, exit{}, kernel{}, user{};
        const bool identity = GetProcessTimes(process, &birth, &exit, &kernel, &user);
        const quint64 born = (quint64(birth.dwHighDateTime) << 32) | birth.dwLowDateTime;
        if (!identity || pid != cachedPid || born != cachedBirth || cachedSelectedPath != m_exePath) {
            cachedSelectedPath = m_exePath;
            cachedPid = pid;
            cachedBirth = born;
            objects.clear();
            globalMixers.clear();
            globalProfiles.clear();
            globalAssignments.clear();
            globalOsds.clear();
            globalSettings.clear();
            scanAge.invalidate();
            verified = false;
            verificationError =
                "Cannot identify the running BEACN executable. Reconnect after restarting BEACN.";
            base = 0;
            wchar_t path[32768];
            DWORD count = 32768, size = 0;
            QString error;
            if (identity && QueryFullProcessImageNameW(process, 0, path, &count)) {
                const auto runningPath = QString::fromWCharArray(path, count);
                if (QFileInfo(runningPath)
                        .canonicalFilePath()
                        .compare(QFileInfo(m_exePath).canonicalFilePath(), Qt::CaseInsensitive) != 0)
                    verificationError =
                        "The running BEACN app differs from the selected executable. Use Auto-detect in Modules > BEACN.";
                else if (!compatibleBeacnAppFile(runningPath))
                    verificationError =
                        "The running BEACN app " + beacnFileVersion(runningPath) +
                        " is not compatible with this native bridge. App controls remain unavailable.";
                else if (!findBeacnModuleBase(pid, base, size, &error))
                    verificationError = error;
                else {
                    verified = true;
                    verificationError.clear();
                }
            }
        }
        unavailable =
            verified
                ? "The live mixer layout could not be validated. Check BEACN has an active mixing device, then use Reconnect."
                : verificationError;
        if (verified) {
            // Rediscovery can read a large heap. Run it separately so neither
            // sliders nor the serialized vendor command worker wait for a scan.
            if (allowDiscovery && !m_nativeScanPending &&
                (!scanAge.isValid() || scanAge.elapsed() >= 5000)) {
                struct Objects {
                    QVector<quintptr> faders, mixers, profiles, assignments, osds, settings;
                };
                m_nativeScanPending = true;
                auto watcher = new QFutureWatcher<Objects>(this);
                const auto generation = m_connectionGeneration;
                const auto scanBase = base;
                const auto selectedPath = m_exePath;
                connect(watcher, &QFutureWatcher<Objects>::finished, this,
                        [this, watcher, generation, pid, born, scanBase, selectedPath] {
                    const auto found = watcher->result();
                    watcher->deleteLater();
                    m_nativeScanPending = false;
                    // A late result from an old process/profile configuration is unusable.
                    if (generation != m_connectionGeneration || cachedPid != pid ||
                        cachedBirth != born || base != scanBase || m_exePath != selectedPath)
                        return;
                    objects = found.faders;
                    globalMixers = found.mixers;
                    globalProfiles = found.profiles;
                    globalAssignments = found.assignments;
                    globalOsds = found.osds;
                    globalSettings = found.settings;
                    scanAge.restart();
                    refreshNativeStates(false);
                });
                watcher->setFuture(QtConcurrent::run([pid, born, scanBase, stop = m_nativeScanStop] {
                    Objects found;
                    HANDLE scanProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                    if (!scanProcess)
                        return found;
                    struct ProcessGuard {
                        HANDLE handle;
                        ~ProcessGuard() { CloseHandle(handle); }
                    } guard{scanProcess};
                    FILETIME birth{}, exit{}, kernel{}, user{};
                    if (!GetProcessTimes(scanProcess, &birth, &exit, &kernel, &user) ||
                        ((quint64(birth.dwHighDateTime) << 32) | birth.dwLowDateTime) != born)
                        return found;
                    quintptr cursor = 0;
                    while (!stop->load() && cursor < 0x00007fffffffffffULL) {
                        MEMORY_BASIC_INFORMATION mbi{};
                        if (!VirtualQueryEx(scanProcess, reinterpret_cast<void *>(cursor), &mbi, sizeof(mbi)))
                            break;
                        const quintptr start = reinterpret_cast<quintptr>(mbi.BaseAddress),
                                       end = start + mbi.RegionSize;
                        if (end <= cursor)
                            break;
                        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
                            remoteProtectionReadable(mbi.Protect)) {
                            for (quintptr pos = start; pos < end && !stop->load(); pos += 1024 * 1024) {
                                const SIZE_T count = std::min<quintptr>(1024 * 1024, end - pos);
                                QByteArray block(static_cast<qsizetype>(count), '\0');
                                SIZE_T got = 0;
                                ReadProcessMemory(scanProcess, reinterpret_cast<void *>(pos),
                                                  block.data(), count, &got);
                                for (SIZE_T i = 0; i + 8 <= got; i += 8) {
                                    quint64 value;
                                    std::memcpy(&value, block.constData() + i, 8);
                                    if (value == scanBase + BeacnLayout::FaderTable)
                                        found.faders.append(pos + i);
                                    if (value == scanBase + BeacnLayout::MixerTable)
                                        found.mixers.append(pos + i);
                                    if (value == scanBase + BeacnLayout::ProfilesTable)
                                        found.profiles.append(pos + i);
                                    if (value == scanBase + BeacnLayout::AssignmentsTable)
                                        found.assignments.append(pos + i);
                                    if (value == scanBase + BeacnLayout::OsdTable)
                                        found.osds.append(pos + i);
                                    if (value == scanBase + BeacnLayout::SystemSettingsTable)
                                        found.settings.append(pos + i);
                                }
                            }
                        }
                        cursor = end;
                    }
                    return found;
                }));
            }
            m_globalPid = pid;
            readGlobalState(process, base, globalMixers, globalProfiles, globalAssignments, globalOsds,
                            globalSettings);
            m_nativeBase = base;
            m_nativeBirth = born;
            m_nativeFaders.fill(0, states.size());
            QVector<int> matches(states.size(), 0);
            for (quintptr obj : objects) {
                quint64 table = 0, data = 0, name = 0;
                unsigned char fields[0x5a]{};
                if (!readRemoteValue(process, obj, table) || table != base + BeacnLayout::FaderTable ||
                    !readRemoteValue(process, obj + 0x150, data) ||
                    !readRemoteBlock(process, data, fields, sizeof(fields)))
                    continue;
                std::memcpy(&name, fields, 8);
                float levels[2];
                std::memcpy(levels, fields + 0x10, sizeof(levels));
                if (!std::isfinite(levels[0]) || !std::isfinite(levels[1]) || levels[0] < 0 ||
                    levels[0] > 1 || levels[1] < 0 || levels[1] > 1 || fields[9] > 1 || fields[0x18] > 1 ||
                    fields[0x19] > 1 || fields[0x58] > 1 || fields[0x59] > 1)
                    continue;
                int modes[2];
                std::memcpy(modes, fields + 0x50, sizeof(modes));
                for (int i = 0; i < states.size(); ++i) {
                    const QByteArray label = nativeSourceName(i).toUtf8();
                    char actual[128]{};
                    if (label.isEmpty() || label.size() >= 128 ||
                        !readRemoteBlock(process, name, actual, label.size() + 1) ||
                        std::memcmp(actual, label.constData(), label.size() + 1))
                        continue;
                    ++matches[i];
                    m_nativeFaders[i] = obj;
                    states[i] =
                        QVariantMap{{QStringLiteral("valid"), true},
                                    {QStringLiteral("linked"), fields[9] == 1 && levels[0] == levels[1]},
                                    {QStringLiteral("personalPercent"), levels[0] * 100.0},
                                    {QStringLiteral("audiencePercent"), levels[1] * 100.0},
                                    {QStringLiteral("personalMuted"), fields[0x18] != 0},
                                    {QStringLiteral("audienceMuted"), fields[0x19] != 0},
                                    {QStringLiteral("knobMuted"), fields[0x58] != 0},
                                    {QStringLiteral("buttonMuted"), fields[0x59] != 0},
                                    {QStringLiteral("knobMode"), modes[0]},
                                    {QStringLiteral("buttonMode"), modes[1]}};
                }
            }
            for (int i = 0; i < states.size(); ++i)
                if (matches[i] != 1) {
                    states[i] = QVariantMap{{QStringLiteral("valid"), false}};
                    m_nativeFaders[i] = 0; // Never select an ambiguous or stale row.
                }
        }
        CloseHandle(process);
    }
#endif
    if (!m_globalMixer && (!m_globalState.isEmpty() || m_globalStatus != unavailable)) {
        m_globalState.clear();
        m_deviceChoices.clear();
        m_globalStatus = unavailable;
        appendLog("BEACN app readback: " + unavailable);
        emit globalStateChanged();
    }
    if (states != m_nativeStates) {
        for (int i = 0; i < states.size(); ++i) {
            const auto now = states[i].toMap();
            const auto old = i < m_nativeStates.size() ? m_nativeStates[i].toMap() : QVariantMap{};
            if (now.value("valid") != old.value("valid") ||
                now.value("knobMuted") != old.value("knobMuted") ||
                now.value("buttonMuted") != old.value("buttonMuted") ||
                now.value("personalMuted") != old.value("personalMuted") ||
                now.value("audienceMuted") != old.value("audienceMuted"))
                appendLog(QStringLiteral("NATIVE STATE %1: %2")
                              .arg(m_sourceNames[i], QString::fromUtf8(QJsonDocument::fromVariant(now).toJson(
                                                         QJsonDocument::Compact))));
        }
        m_nativeStates = states;
        emit nativeStatesChanged();
    }
}

bool BeacnBackend::nativeSourceAvailable(int sourcePair) const {
    if (nativeSourceName(sourcePair).isEmpty())
        return false;
    if (sourcePair < m_nativeStates.size()) {
        const auto state = m_nativeStates[sourcePair].toMap();
        return state.value(QStringLiteral("valid")).toBool() &&
               state.value(QStringLiteral("linked")).toBool();
    }
    return false;
}

bool BeacnBackend::setNativeSourcePercent(int sourcePair, double percent) {
    const bool ok = writeNativeVolume(sourcePair, percent, false);
    // The command acknowledgement includes a snapshot. Refresh the app model
    // first so the UI does not briefly snap back to the previous volume.
    if (ok)
        refreshNativeStates(false);
    return ok;
}

bool BeacnBackend::stepGameVolume(double percentStep) {
    return writeNativeVolume(2, percentStep, true);
}

bool BeacnBackend::setNativeControl(int sourcePair, bool active, bool channelButton) {
    const bool ok = writeNativeVolume(sourcePair, active ? 100 : 0, false, channelButton ? 6 : 5);
    refreshNativeStates();
    exportRecording(false);
    return ok;
}

bool BeacnBackend::validateNativeFader(void *process, quintptr base, quintptr object,
                                       const QByteArray &name, bool linked, float &current) {
#ifdef Q_OS_WIN
    quint64 table = 0, data = 0, nameAddress = 0;
    float levels[2]{};
    unsigned char linkedFlag = 0;
    char label[128]{};
    if (!base || !object || name.isEmpty() || name.size() >= qsizetype(sizeof(label)) ||
        !readRemoteValue(process, object, table) || table != base + BeacnLayout::FaderTable ||
        !readRemoteValue(process, object + 0x150, data) || !readRemoteValue(process, data, nameAddress) ||
        !readRemoteBlock(process, nameAddress, label, name.size() + 1) ||
        std::memcmp(label, name.constData(), name.size() + 1) ||
        !readRemoteBlock(process, data + 0x10, levels, sizeof(levels)) ||
        !readRemoteValue(process, data + 0x09, linkedFlag) || linkedFlag > 1 ||
        !std::isfinite(levels[0]) || !std::isfinite(levels[1]) || levels[0] < 0 || levels[0] > 1 ||
        levels[1] < 0 || levels[1] > 1 || (linked && (linkedFlag != 1 || levels[0] != levels[1])))
        return false;
    current = levels[0];
    return true;
#else
    Q_UNUSED(process); Q_UNUSED(base); Q_UNUSED(object); Q_UNUSED(name); Q_UNUSED(linked); Q_UNUSED(current);
    return false;
#endif
}

bool BeacnBackend::writeNativeVolume(int sourcePair, double percentStep, bool relative, int operation) {
#ifdef Q_OS_WIN
    auto fail = [this](const QString &message) {
        appendLog(QStringLiteral("APP WRITE REFUSED: %1").arg(message));
        m_winUsbStatus = message;
        emit winUsbProbeChanged();
        return false;
    };
    if (m_globalUncertain)
        return fail(QStringLiteral("Previous command outcome is unknown. Check BEACN before reconnecting."));
    if (operation < 1 || operation > 6 || !m_connected || !std::isfinite(percentStep) ||
        (relative ? std::abs(percentStep) > 1.0 : (percentStep < 0 || percentStep > 100)))
        return fail(QStringLiteral("Connect first and choose a volume between 0 and 100%."));
    const QString sourceName = nativeSourceName(sourcePair);
    const QByteArray encodedName = sourceName.toUtf8();
    if (encodedName.isEmpty() || encodedName.size() >= 128)
        return fail(QStringLiteral("No uniquely mapped BEACN profile row for this source."));
    const DWORD pid = findBeacnProcessId();
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process)
        return fail(QStringLiteral("Cannot read BEACN process."));

    struct ProcessGuard {
        HANDLE h;

        ~ProcessGuard() {
            CloseHandle(h);
        }
    } guard{process};

    wchar_t exeName[32768];
    DWORD nameSize = 32768;
    if (!QueryFullProcessImageNameW(process, 0, exeName, &nameSize))
        return fail(QStringLiteral("Cannot identify the running BEACN executable."));
    QFile executable(QString::fromWCharArray(exeName, nameSize));
    if (QFileInfo(executable)
            .canonicalFilePath()
            .compare(QFileInfo(m_exePath).canonicalFilePath(), Qt::CaseInsensitive) != 0)
        return fail("The running BEACN app differs from the selected executable. Check Setup.");
    FILETIME birth{}, exit{}, kernel{}, user{};
    const quint64 born = GetProcessTimes(process, &birth, &exit, &kernel, &user)
                             ? (quint64(birth.dwHighDateTime) << 32) | birth.dwLowDateTime : 0;
    // The worker refreshed and verified these addresses before dispatch. Validate
    // the process lifetime and the exact row again; the injected handler repeats
    // these checks on BEACN's own UI thread before touching its model.
    if (!born || pid != m_globalPid || born != m_nativeBirth || !m_nativeBase)
        return fail("BEACN restarted since the last readback. Wait for reconnection.");
    const quintptr game = m_nativeFaders.value(sourcePair);
    float original = 0;
    if (!validateNativeFader(process, m_nativeBase, game, encodedName, operation == 1, original))
        return fail("The BEACN source changed since the last readback. No command was sent.");

    struct WindowSearch {
        DWORD pid;
        HWND hwnd;
        DWORD thread;
    } search{pid, nullptr, 0};

    EnumWindows(
        [](HWND hwnd, LPARAM arg) -> BOOL {
            auto s = reinterpret_cast<WindowSearch *>(arg);
            DWORD owner = 0;
            DWORD thread = GetWindowThreadProcessId(hwnd, &owner);
            if (owner != s->pid)
                return TRUE;
            wchar_t title[256]{};
            GetWindowTextW(hwnd, title, 256);
            if (!wcscmp(title, L"BEACN")) {
                s->hwnd = hwnd;
                s->thread = thread;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.hwnd)
        return fail(QStringLiteral("BEACN main window/thread not found."));

    struct Command {
        DWORD magic, pid;
        volatile LONG state;
        DWORD mode;
        quint64 object;
        float expected, target, observed;
        DWORD error;
        char sourceName[128];
    };

    static_assert(sizeof(Command) == 168);
    const QString mappingName = QStringLiteral("Local\\BEACN.WHAT.Volume142.%1").arg(pid);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Command),
                                        reinterpret_cast<LPCWSTR>(mappingName.utf16()));
    const DWORD mappingError = GetLastError();
    if (!mapping)
        return fail(QStringLiteral("Cannot create bridge command mapping."));
    if (mappingError == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        return fail(QStringLiteral("Another native volume command is active."));
    }
    auto cmd = static_cast<Command *>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Command)));
    if (!cmd) {
        CloseHandle(mapping);
        return fail(QStringLiteral("Cannot map bridge command."));
    }
    const float target =
        static_cast<float>(std::clamp((relative ? original : 0.0) + percentStep / 100.0, 0.0, 1.0));
    *cmd = Command{0x42564f4c, pid, 0, static_cast<DWORD>(operation), game, original, target, 0, 0, {}};
    std::memcpy(cmd->sourceName, encodedName.constData(), encodedName.size() + 1);
    QString bridgePreparationError;
    const QString dll = prepareBeacnHook(
        QCoreApplication::applicationDirPath() + "/beacn-volume-hook.dll",
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/bridges/beacn",
        &bridgePreparationError);
    SetLastError(0);
    HMODULE module = dll.isEmpty() ? nullptr : LoadLibraryW(reinterpret_cast<LPCWSTR>(dll.utf16()));
    DWORD sendError = module ? 0 : GetLastError();
    auto entry = module ? GetProcAddress(module, "VolumeHook") : nullptr;
    if (module && !entry)
        sendError = GetLastError();
    HHOOK hook =
        entry ? SetWindowsHookExW(WH_CALLWNDPROC, reinterpret_cast<HOOKPROC>(entry), module, search.thread)
              : nullptr;
    if (entry && !hook)
        sendError = GetLastError();
    bool sent = false;
    const bool allowed = !m_commandGuard || m_commandGuard();
    if (hook && allowed) {
        appendLog(QStringLiteral(
                      "APP WRITE %4: %1% -> %2%; BEACN UI thread=%3; operation=%5; no direct USB commands")
                      .arg(original * 100, 0, 'f', 4)
                      .arg(target * 100, 0, 'f', 4)
                      .arg(search.thread)
                      .arg(sourceName)
                      .arg(operation));
        DWORD_PTR result = 0;
        sent = SendMessageTimeoutW(search.hwnd, RegisterWindowMessageW(L"BEACN.WHAT.Volume.142"), 0, 0,
                                   SMTO_ABORTIFHUNG, 3000, &result) != 0;
        sendError = GetLastError();
    }
    const LONG state = cmd->state;
    const float observed = cmd->observed;
    const DWORD bridgeError = cmd->error;
    if (hook)
        UnhookWindowsHookEx(hook);
    UnmapViewOfFile(cmd);
    CloseHandle(mapping);
    if (module)
        FreeLibrary(module);
    if (hook && allowed && (!sent || state == 1))
        m_globalUncertain = true;
    if (!bridgePreparationError.isEmpty())
        return fail(bridgePreparationError);
    if (!sent || state != 2)
        return fail(
            QStringLiteral(
                "Bridge result state=%1 error=%2 Win32=%3. Do not retry until BEACN state is checked.")
                .arg(state)
                .arg(bridgeError)
                .arg(sendError));
    if (operation != 1) {
        m_winUsbStatus = QStringLiteral("Native control update applied: %1; active=%2; operation=%3")
                             .arg(sourceName)
                             .arg(target != 0)
                             .arg(operation);
        appendLog(m_winUsbStatus);
        emit winUsbProbeChanged();
        QTimer::singleShot(1200, this, [this, generation = m_connectionGeneration] {
            if (generation == m_connectionGeneration)
                recordTick();
        });
        return true;
    }
    appendLog(
        QStringLiteral(
            "APP WRITE handler returned; BEACN model=%1%. Physical screen update requires visual confirmation.")
            .arg(observed * 100, 0, 'f', 4));
    m_winUsbStatus = QStringLiteral("Source set through BEACN to %1%. Check app and device display.")
                         .arg(observed * 100, 0, 'f', 2);
    emit winUsbProbeChanged();
    QTimer::singleShot(1200, this, [this, target, sourcePair, generation = m_connectionGeneration] {
        if (!m_connected || generation != m_connectionGeneration)
            return;
        const auto route = readStereoRoute(sourcePair, 1);
        const int expectedRaw = static_cast<int>(std::pow(static_cast<double>(target), 4) * 16777216.0);
        appendLog(QStringLiteral("APP WRITE settled audio check: expectedRaw=%1 L=%2 R=%3 valid=%4")
                      .arg(expectedRaw)
                      .arg(route.value(QStringLiteral("leftRaw")).toInt())
                      .arg(route.value(QStringLiteral("rightRaw")).toInt())
                      .arg(route.value(QStringLiteral("valid")).toBool()));
        recordTick();
    });
    return true;
#else
    Q_UNUSED(percentStep);
    Q_UNUSED(sourcePair);
    Q_UNUSED(relative);
    Q_UNUSED(operation);
    return false;
#endif
}

void BeacnBackend::setRecording(bool enabled) {
    m_recording = enabled;
    m_recordedMatrix.clear();
    m_recordingStatus = enabled ? QStringLiteral("Recording enabled")
                                : QStringLiteral("Volume polling paused; session log stays open");
    appendLog(m_recordingStatus);
    emit recordingChanged();
}

void BeacnBackend::markObservation(const QString &text) {
    QString clean = text.left(500);
    clean.replace(QLatin1Char('\n'), QLatin1Char(' '));
    clean.replace(QLatin1Char('\r'), QLatin1Char(' '));
    appendLog(QStringLiteral("USER MARK: %1").arg(clean));
    if (m_connected && m_recording)
        recordTick();
}

void BeacnBackend::recordTick() {
    if (!m_connected || !m_recording || m_recordTick)
        return;
    QElapsedTimer elapsed;
    elapsed.start();
    m_recordTick = true;
    const bool ok = refreshMatrix();
    m_recordTick = false;
    const qint64 duration = elapsed.elapsed();
    refreshNativeStates();
    if (!ok) {
        const bool uncertain = m_globalUncertain;
        disconnectDriver();
        m_recording = false;
        m_reconnectPending = !uncertain;
        m_globalUncertain = uncertain;
        m_recordingStatus = "Device read failed; reconnecting every 3 seconds. No writes will be replayed.";
        if (uncertain)
            m_recordingStatus =
                "Device read failed after an uncertain command. Check BEACN and reconnect manually.";
        appendLog(m_recordingStatus);
        exportRecording(false);
        emit connectedChanged();
        emit recordingChanged();
        return;
    }
    if (duration > 500) {
        // Reads are serialized off the GUI thread in WHAT. Keep observing after
        // a slow sample so UI/HA state cannot silently become stale indefinitely.
        m_recordingStatus = QStringLiteral("Slow read %1, duration %2 ms; monitoring continues.")
                                .arg(ok ? QStringLiteral("slow") : QStringLiteral("failed"))
                                .arg(duration);
        appendLog(m_recordingStatus);
    }
    exportRecording((++m_recordTicks % 10) == 0);
    if (m_recording && !m_recordingStatus.startsWith(QStringLiteral("EXPORT ERROR"))) {
        m_recordingStatus =
            QStringLiteral("Reading volumes every second · last read %1 ms")
                .arg(duration);
    }
    emit recordingChanged();
}

void BeacnBackend::exportRecording(bool heartbeat) {
    // The volume CSV and state snapshot are troubleshooting aids. Writing them
    // every second by default only wears the disk, so they follow verbose logging.
    if (!m_verboseLogging) {
        m_recordedMatrix = m_matrix;
        return;
    }
    const QString stamp = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
    for (int i = 0; i < m_matrix.size(); ++i) {
        const QVariantMap current = m_matrix.at(i).toMap();
        const QVariantMap before =
            i < m_recordedMatrix.size() ? m_recordedMatrix.at(i).toMap() : QVariantMap();
        const bool changed = current != before;
        if (!changed && !heartbeat)
            continue;
        const int source = current.value(QStringLiteral("sourcePair")).toInt();
        const int bus = current.value(QStringLiteral("busPair")).toInt();
        if (changed) {
            appendLog(QStringLiteral("VOLUME %1 / %2: L=%3% R=%4% raw=%5/%6 valid=%7")
                          .arg(m_sourceNames.value(source), m_busNames.value(bus))
                          .arg(current.value(QStringLiteral("leftPercent")).toDouble(), 0, 'f', 2)
                          .arg(current.value(QStringLiteral("rightPercent")).toDouble(), 0, 'f', 2)
                          .arg(current.value(QStringLiteral("leftRaw")).toInt())
                          .arg(current.value(QStringLiteral("rightRaw")).toInt())
                          .arg(current.value(QStringLiteral("valid")).toBool()));
        }
        if (m_csvFile->isOpen()) {
            const QByteArray line =
                QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8,%9\n")
                    .arg(stamp, changed ? QStringLiteral("change") : QStringLiteral("heartbeat"))
                    .arg(source)
                    .arg(bus)
                    .arg(current.value(QStringLiteral("leftRaw")).toInt())
                    .arg(current.value(QStringLiteral("rightRaw")).toInt())
                    .arg(current.value(QStringLiteral("leftPercent")).toDouble(), 0, 'f', 4)
                    .arg(current.value(QStringLiteral("rightPercent")).toDouble(), 0, 'f', 4)
                    .arg(current.value(QStringLiteral("valid")).toBool() ? 1 : 0)
                    .toUtf8();
            if (m_csvFile->write(line) != line.size() || !m_csvFile->flush())
                m_recordingStatus = QStringLiteral("EXPORT ERROR: volume CSV write failed");
        }
    }
    m_recordedMatrix = m_matrix;
    QJsonObject state;
    state.insert(QStringLiteral("timestamp"), stamp);
    state.insert(QStringLiteral("sessionLog"), m_logPath);
    state.insert(QStringLiteral("connected"), m_connected);
    state.insert(QStringLiteral("recording"), m_recording);
    state.insert(QStringLiteral("synchronizedWritesVerified"), false);
    state.insert(QStringLiteral("matrix"), QJsonValue::fromVariant(m_matrix));
    state.insert(QStringLiteral("globalState"), QJsonValue::fromVariant(m_globalState));
    state.insert(QStringLiteral("deviceChoices"), QJsonValue::fromVariant(m_deviceChoices));
    state.insert(QStringLiteral("globalStatus"), m_globalStatus);
    state.insert(QStringLiteral("nativeStates"), QJsonValue::fromVariant(m_nativeStates));
    state.insert(QStringLiteral("sourceNames"), QJsonValue::fromVariant(m_sourceNames));
    state.insert(QStringLiteral("busNames"), QJsonValue::fromVariant(m_busNames));
    QSaveFile latest(m_logPath + QStringLiteral(".state.json"));
    const QByteArray json = QJsonDocument(state).toJson();
    if (!latest.open(QIODevice::WriteOnly) || latest.write(json) != json.size() || !latest.commit())
        m_recordingStatus = QStringLiteral("EXPORT ERROR: state snapshot write failed");
    if (heartbeat)
        appendLog(QStringLiteral("RECORDER heartbeat: %1 routes; exports updated").arg(m_matrix.size()));
}

void BeacnBackend::appendLog(const QString &message) {
    const QString line = QStringLiteral("%1  %2").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")), message);

    if (m_logFile && m_logFile->isOpen()) {
        QTextStream stream(m_logFile);
        stream << line << '\n';
        stream.flush();
        if (stream.status() != QTextStream::Ok || !m_logFile->flush()) {
            m_recordingStatus = QStringLiteral("EXPORT ERROR: session log write failed");
            emit recordingChanged();
        }
    }
    emit logMessage(line);
}

QString BeacnBackend::statusHex(Status status) const {
    return QStringLiteral("0x%1").arg(status, 8, 16, QLatin1Char('0')).toUpper();
}

QString BeacnBackend::routeText(int inputPin, int inputIndex, int outputPin, int outputIndex) const {
    return QStringLiteral("%1[%2] → %3[%4]")
        .arg(pinName(inputPin))
        .arg(inputIndex)
        .arg(pinName(outputPin))
        .arg(outputIndex);
}

double BeacnBackend::rawToDb(std::int32_t raw) {
    if (raw <= 0)
        return -120.0;
    const double linear = static_cast<double>(raw) / 16777216.0;
    return 20.0 * std::log10(linear);
}

std::int32_t BeacnBackend::dbToRaw(double db) {
    if (db <= -120.0)
        return 0;
    const double linear = std::pow(10.0, db / 20.0);
    const double raw = std::round(linear * 16777216.0);
    return static_cast<std::int32_t>(
        std::clamp(raw, 0.0, static_cast<double>(std::numeric_limits<std::int32_t>::max())));
}

double BeacnBackend::rawToPercent(std::int32_t raw) {
    if (raw <= 0)
        return 0.0;

    // Recovered from BEACN 1.4.0: driverRaw = pow(fader01, 4.0) * 2^24.
    const double linear = std::clamp(static_cast<double>(raw) / 16777216.0, 0.0, 1.0);
    return std::pow(linear, 0.25) * 100.0;
}

std::int32_t BeacnBackend::percentToRaw(double percent) {
    const double fader = std::clamp(percent, 0.0, 100.0) / 100.0;
    const double raw = std::pow(fader, 4.0) * 16777216.0;
    return static_cast<std::int32_t>(std::clamp(raw, 0.0, 16777216.0));
}

void BeacnBackend::readGlobalState(void *handle, quintptr base, const QVector<quintptr> &mixers,
                                   const QVector<quintptr> &profiles, const QVector<quintptr> &assignments,
                                   const QVector<quintptr> &osds, const QVector<quintptr> &settings) {
#ifdef Q_OS_WIN
    HANDLE process = static_cast<HANDLE>(handle);
    auto ptr = [&](quintptr address) {
        quintptr value = 0;
        readRemoteValue(process, address, value);
        return value;
    };
    auto integer = [&](quintptr address) {
        int value = -1;
        readRemoteValue(process, address, value);
        return value;
    };
    auto byte = [&](quintptr address) {
        unsigned char value = 255;
        readRemoteValue(process, address, value);
        return int(value);
    };
    auto text = [&](quintptr address) {
        QByteArray bytes;
        for (int i = 0; i < 512; ++i) {
            char ch = 0;
            if (!readRemoteValue(process, address + i, ch))
                return QString();
            if (!ch)
                return QString::fromUtf8(bytes);
            bytes.append(ch);
        }
        return QString();
    };
    QVector<quintptr> valid;
    for (auto p : mixers)
        if (ptr(p) == base + BeacnLayout::MixerTable)
            valid.append(p);
    if (valid.size() != 1)
        return;
    const auto root = valid[0], data = ptr(root + 0x2b0), listen = ptr(root + 0x258),
               audience = ptr(root + 0x268);
    if (ptr(listen) != base + BeacnLayout::ListeningTable || ptr(audience) != base + BeacnLayout::AudienceTable)
        return;
    int page = integer(root + 0x674), pages = integer(root + 0x678), mix = integer(data + 0xd8);
    const auto ld = ptr(listen + 0x488), ad = ptr(audience + 0x180);
    int active = byte(ld + 0x10);
    if (page < 1 || pages < page || pages > 64 || mix < 0 || mix > 1 || active > 1)
        return;
    m_globalMixer = root;
    QVariantMap state{{"valid", true},
                      {"submix", mix == 1},
                      {"page", page},
                      {"pages", pages},
                      {"activeA", active == 1},
                      {"deviceA", text(ptr(ld))},
                      {"deviceB", text(ptr(ld + 8))},
                      {"audienceDevice", text(ptr(ad + 8))}};
    // Display order is independent of the driver's stable stereo source indices.
    // Read the active UI's ordered shared_ptr vector so unsaved drag/reorder changes
    // appear immediately; never reinterpret a visual row number as an audio route.
    QVariantList sourceOrder;
    const auto orderBegin = ptr(root + 0x218), orderEnd = ptr(root + 0x220);
    bool orderValid = orderBegin && orderEnd >= orderBegin && (orderEnd - orderBegin) % 16 == 0 &&
                      (orderEnd - orderBegin) / 16 <= 128;
    if (orderValid)
        for (auto it = orderBegin; it < orderEnd; it += 16) {
            auto knob = ptr(it), knobData = ptr(knob + 0x150);
            if (ptr(knob) != base + BeacnLayout::FaderTable || !knobData) {
                orderValid = false;
                break;
            }
            const int type = integer(knobData + 0x28);
            const int source = type == 3   ? 0
                               : type == 4 ? 1
                               : type == 5 ? 2
                               : type == 6 ? 3
                               : type == 7 ? 4
                               : type == 1 ? 7
                                           : -1;
            if (source < 0)
                continue; // Listening Device is an output, not a source row.
            if (source >= m_sourceNames.size() || sourceOrder.contains(source)) {
                orderValid = false;
                break;
            }
            sourceOrder.append(source);
        }
    if (!orderValid)
        sourceOrder.clear();
    for (int source = 0; source < m_sourceNames.size(); ++source)
        if (!sourceOrder.contains(source))
            sourceOrder.append(source);
    state.insert("sourceOrder", sourceOrder);
    state.insert("sourceOrderValid", orderValid);
    valid.clear();
    for (auto p : profiles)
        if (ptr(p) == base + BeacnLayout::ProfilesTable)
            valid.append(p);
    QStringList names;
    QString current;
    if (valid.size() == 1) {
        const auto owner = valid[0], begin = ptr(owner + 0x190), end = ptr(owner + 0x198);
        bool ok = end >= begin && (end - begin) % 16 == 0 && (end - begin) / 16 <= 128;
        int activeCount = 0;
        if (ok)
            for (auto it = begin; it < end; it += 16) {
                const auto row = ptr(it);
                if (ptr(row) != base + BeacnLayout::ProfileRowTable || ptr(row + 0x250) != owner + 0x140 ||
                    byte(row + 0x171) > 1) {
                    ok = false;
                    break;
                }
                auto name = text(ptr(row + 0x178));
                if (name.isEmpty() || names.contains(name)) {
                    ok = false;
                    break;
                }
                names.append(name);
                if (byte(row + 0x171)) {
                    current = name;
                    ++activeCount;
                }
            }
        if (ok && activeCount == 1)
            m_globalProfiles = owner;
        else {
            names.clear();
            current.clear();
        }
    }
    state.insert("profiles", names);
    state.insert("profile", current);
    state.insert("profilesValid", m_globalProfiles != 0);
    // JUCE SimpleValueSource stores its var at +0x40; validate the concrete
    // source and var types before reading strings or numeric slider values.
    auto comboText = [&](quintptr combo) {
        auto source = ptr(ptr(combo + 0x220) + 0x1f0);
        if (ptr(combo) != base + BeacnLayout::ComboTable || ptr(source) != base + BeacnLayout::ValueSourceTable ||
            ptr(source + 0x40) != base + BeacnLayout::StringValueType)
            return QString();
        return text(ptr(source + 0x48));
    };
    auto sliderValue = [&](quintptr slider) {
        auto source = ptr(ptr(slider + 0x258) + 0x68);
        double value = -1;
        if (ptr(source) == base + BeacnLayout::ValueSourceTable && ptr(source + 0x40) == base + BeacnLayout::DoubleValueType)
            readRemoteValue(process, source + 0x48, value);
        return value;
    };
    valid.clear();
    for (auto p : assignments)
        if (ptr(p) == base + BeacnLayout::AssignmentsTable && ptr(p + 0x1c0) == root + 0x1d0)
            valid.append(p);
    QStringList defaults;
    if (valid.size() == 1) {
        m_globalAssignments = valid[0];
        for (auto off : {0x158, 0x168, 0x180, 0x190})
            defaults.append(comboText(ptr(valid[0] + off)));
    }
    state.insert("assignmentsValid", m_globalAssignments != 0);
    state.insert("assignments", defaults);
    valid.clear();
    for (auto p : settings)
        if (ptr(p) == base + BeacnLayout::SystemSettingsTable && ptr(ptr(p + 0x1b0)) == base + BeacnLayout::DefaultResetManagerTable)
            valid.append(p);
    state.insert("autoResetValid", false);
    if (valid.size() == 1) {
        // 1.4.2 keeps both default-device options in this group. Select the
        // auto-reset row by its setting ID, not its position or the row count.
        const auto group = ptr(valid[0] + 0x168), begin = ptr(group + 0x168), end = ptr(group + 0x170);
        quintptr row = 0;
        int matches = 0;
        if (ptr(group) == base + BeacnLayout::SettingsGroupTable && end >= begin &&
            (end - begin) % 8 == 0 && end - begin <= 256) {
            for (auto it = begin; it < end; it += 8) {
                const auto candidate = ptr(it);
                if (ptr(candidate) == base + BeacnLayout::SettingsToggleTable && integer(candidate + 0x170) == 5) {
                    row = candidate;
                    ++matches;
                }
            }
        }
        if (matches == 1) {
            double value = sliderValue(ptr(row + 0x150));
            if (value == 0 || value == 1) {
                m_globalSettings = valid[0];
                state.insert("autoResetValid", true);
                state.insert("autoResetDefaults", value == 1);
            }
        }
    }
    QStringList windowsDefaults;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator *enumerator = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&enumerator)))) {
        const PROPERTYKEY friendlyName = {
            {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
        for (int i = 0; i < 4; ++i) {
            QString name;
            IMMDevice *device = nullptr;
            IPropertyStore *properties = nullptr;
            if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(i < 2 ? eCapture : eRender,
                                                              i % 2 ? eCommunications : eConsole, &device))) {
                if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
                    PROPVARIANT value{};
                    if (SUCCEEDED(properties->GetValue(friendlyName, &value)) && value.vt == VT_LPWSTR)
                        name = QString::fromWCharArray(value.pwszVal);
                    PropVariantClear(&value);
                    properties->Release();
                }
                device->Release();
            }
            windowsDefaults.append(name);
        }
        enumerator->Release();
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    state.insert("windowsDefaults", windowsDefaults);

    valid.clear();
    for (auto p : osds)
        if (ptr(p) == base + BeacnLayout::OsdTable && ptr(p + 0x3d0) + 8 == ptr(root + 0x340))
            valid.append(p);
    state.insert("lightingValid", false);
    if (valid.size() == 1) {
        auto osd = valid[0], picker = ptr(osd + 0x3b8), light = ptr(osd + 0x3c0);
        auto dimmer = ptr(light + 0x130), screen = ptr(light + 0x120), buttons = ptr(light + 0x128);
        double seconds = -1;
        readRemoteValue(process, dimmer + 0x48, seconds);
        const auto screenValue = sliderValue(screen + 0x3c8), buttonValue = sliderValue(buttons + 0x3c8);
        int theme = integer(picker + 0x140);
        if (ptr(picker) == base + BeacnLayout::ThemeTable && ptr(light) == base + BeacnLayout::LightingTable &&
            ptr(dimmer) == base + BeacnLayout::DimmerTable && ptr(screen) == base + BeacnLayout::ScreenBrightnessTable &&
            ptr(buttons) == base + BeacnLayout::ButtonBrightnessTable && theme >= 1 && theme <= 5 && std::isfinite(seconds) &&
            seconds >= 1 && seconds <= 86400 && screenValue >= 2 && screenValue <= 100 && buttonValue >= 4 &&
            buttonValue <= 40) {
            m_globalOsd = osd;
            state.insert("lightingValid", true);
            state.insert("themeId", theme);
            state.insert("themeNames", QStringList{"Original", "Neumorphic", "Neumorphic Dark", "Analogue",
                                                   "Analogue Dark"});
            state.insert("screenBrightness", screenValue);
            state.insert("buttonBrightness", buttonValue);
            state.insert("dimmerSeconds", seconds);
            state.insert("dimmed", byte(dimmer + 0x50) == 1);
        }
    }
    const bool profileChanged = current != m_globalState.value("profile").toString();
    if (state != m_globalState) {
        m_globalState = state;
        appendLog(
            QStringLiteral("GLOBAL STATE %1")
                .arg(QString::fromUtf8(QJsonDocument::fromVariant(state).toJson(QJsonDocument::Compact))));
        emit globalStateChanged();
        if (profileChanged)
            refreshProfile();
    }
#else
    Q_UNUSED(handle);
    Q_UNUSED(base);
    Q_UNUSED(mixers);
    Q_UNUSED(profiles);
    Q_UNUSED(assignments);
    Q_UNUSED(osds);
    Q_UNUSED(settings);
#endif
}

bool BeacnBackend::globalControl(int operation, int value, const QString &target) {
#ifdef Q_OS_WIN
    auto fail = [&](const QString &message) {
        m_globalStatus = message;
        appendLog("GLOBAL BRIDGE: " + message);
        emit globalStateChanged();
        return false;
    };
    if (m_globalBusy || m_globalUncertain)
        return fail(QStringLiteral(
            "Previous command has not been resolved. Check BEACN, then disable and re-enable the BEACN module."));
    const bool known = (operation >= 0 && operation <= 7) || (operation >= 10 && operation <= 14) ||
                       operation == 20 || (operation >= 30 && operation <= 32);
    if (!known || target.toUtf8().size() >= 512 || target.contains(QChar('\0')) ||
        (operation == 14 && (value < 0 || value > 1)) || (operation == 20 && (value < 1 || value > 5)) ||
        (operation == 30 && (value < 2 || value > 100)) || (operation == 31 && (value < 4 || value > 40)) ||
        (operation == 32 && (value < 1 || value > 86400)))
        return fail("Invalid control request.");
    const auto sequence = operation ? ++m_globalSequence : m_globalSequence;
    m_globalBusy = true;

    struct BusyGuard {
        bool &busy;

        ~BusyGuard() {
            busy = false;
        }
    } busyGuard{m_globalBusy};

    refreshNativeStates(); // Checks process identity/hash and revalidates objects before dispatch.
    if (!m_connected || !m_globalMixer || (operation == 7 && !m_globalProfiles))
        return fail("Live BEACN controls are unavailable for this app/profile.");

    struct WindowSearch {
        DWORD pid;
        HWND hwnd;
        DWORD thread;
    } search{m_globalPid, nullptr, 0};

    EnumWindows(
        [](HWND hwnd, LPARAM arg) -> BOOL {
            auto s = reinterpret_cast<WindowSearch *>(arg);
            DWORD owner = 0;
            DWORD thread = GetWindowThreadProcessId(hwnd, &owner);
            if (owner != s->pid)
                return TRUE;
            wchar_t title[256]{};
            GetWindowTextW(hwnd, title, 256);
            if (!wcscmp(title, L"BEACN")) {
                s->hwnd = hwnd;
                s->thread = thread;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.hwnd)
        return fail("BEACN main window is unavailable.");
    const QString name = QStringLiteral("Local\\BEACN.WHAT.Global142.%1").arg(search.pid);
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(GlobalCommand), reinterpret_cast<LPCWSTR>(name.utf16()));
    const DWORD mappingError = GetLastError();
    if (!mapping)
        return fail("Cannot create global command mapping.");
    if (mappingError == ERROR_ALREADY_EXISTS) {
        CloseHandle(mapping);
        return fail("Another global command is active.");
    }
    auto cmd = static_cast<GlobalCommand *>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(GlobalCommand)));
    if (!cmd) {
        CloseHandle(mapping);
        return fail("Cannot map global command.");
    }
    std::memset(cmd, 0, sizeof(*cmd));
    cmd->magic = 0x42474c42;
    cmd->pid = search.pid;
    cmd->operation = operation;
    cmd->mixer = m_globalMixer;
    cmd->profiles = m_globalProfiles;
    cmd->assignments = m_globalAssignments;
    cmd->osd = m_globalOsd;
    cmd->settings = m_globalSettings;
    cmd->value = value;
    const auto encoded = target.toUtf8();
    std::memcpy(cmd->target, encoded.constData(), encoded.size());
    QString bridgePreparationError;
    const QString dll = prepareBeacnHook(
        QCoreApplication::applicationDirPath() + "/beacn-volume-hook.dll",
        QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/bridges/beacn",
        &bridgePreparationError);
    SetLastError(0);
    HMODULE module = dll.isEmpty() ? nullptr : LoadLibraryW(reinterpret_cast<LPCWSTR>(dll.utf16()));
    DWORD winError = module ? 0 : GetLastError();
    auto entry = module ? GetProcAddress(module, "GlobalHook") : nullptr;
    if (module && !entry)
        winError = GetLastError();
    HHOOK hook =
        entry ? SetWindowsHookExW(WH_CALLWNDPROC, reinterpret_cast<HOOKPROC>(entry), module, search.thread)
              : nullptr;
    if (entry && !hook)
        winError = GetLastError();
    bool sent = false;
    appendLog(QStringLiteral("GLOBAL COMMAND operation=%1 value=%2 target=%3")
                  .arg(operation)
                  .arg(value)
                  .arg(target));
    const bool allowed = operation == 0 || !m_commandGuard || m_commandGuard();
    if (hook && allowed) {
        DWORD_PTR result = 0;
        sent = SendMessageTimeoutW(search.hwnd, RegisterWindowMessageW(GlobalMessage), 0, 0, SMTO_ABORTIFHUNG,
                                   3000, &result) != 0;
        winError = GetLastError();
    }
    const LONG state = cmd->state;
    const DWORD error = cmd->error;
    if (sent && state == 2 && (operation == 0 || (operation >= 4 && operation <= 6))) {
        QVariantList lists;
        for (int i = 0; i < 7; ++i) {
            QStringList items;
            for (unsigned j = 0; j < std::min(cmd->counts[i], 128u); ++j) {
                if (std::memchr(cmd->choices[i][j], 0, 512))
                    items.append(QString::fromUtf8(cmd->choices[i][j]));
            }
            lists.append(QVariant(items));
        }
        m_deviceChoices = lists;
    }
    if (hook)
        UnhookWindowsHookEx(hook);
    UnmapViewOfFile(cmd);
    CloseHandle(mapping);
    if (module)
        FreeLibrary(module);
    if (!bridgePreparationError.isEmpty())
        return fail(bridgePreparationError);
    if (!sent || state != 2) {
        // A timeout may leave a handler running. Never automatically retry a mutation.
        if (hook && allowed && (!sent || state == 1))
            m_globalUncertain = true;
        return fail(
            QStringLiteral(
                "Command incomplete/refused (state %1, bridge %2, Windows %3). Check BEACN before retrying.")
                .arg(state)
                .arg(error)
                .arg(winError));
    }
    m_globalStatus =
        operation == 0 ? QStringLiteral("Device lists refreshed from BEACN")
        : operation == 7
            ? QStringLiteral("Profile selection sent to BEACN; live state will confirm the result")
            : QStringLiteral("Control applied through BEACN");
    refreshNativeStates();
    emit globalStateChanged();
    exportRecording(false);
    QTimer::singleShot(
        operation >= 10 && operation <= 14 ? 2500 : 1200, this, [this, operation, value, target, sequence] {
            if (!m_connected || sequence != m_globalSequence)
                return;
            refreshNativeStates();
            if (operation == 3 || operation == 7 || (operation >= 4 && operation <= 6) || operation >= 10) {
                const QString key = operation == 3    ? "activeA"
                                    : operation == 4  ? "deviceA"
                                    : operation == 5  ? "deviceB"
                                    : operation == 6  ? "audienceDevice"
                                    : operation == 14 ? "autoResetDefaults"
                                    : operation == 20 ? "themeId"
                                    : operation == 30 ? "screenBrightness"
                                    : operation == 31 ? "buttonBrightness"
                                    : operation == 32 ? "dimmerSeconds"
                                                      : "profile";
                const auto observed =
                    operation >= 10 && operation <= 13
                        ? QVariant(m_globalState.value("assignments").toStringList().value(operation - 10))
                        : m_globalState.value(key);
                const bool windowsMatch =
                    operation < 10 || operation > 13 ||
                    m_globalState.value("windowsDefaults").toStringList().value(operation - 10) == target;
                const bool matches = windowsMatch && m_globalState.value("valid").toBool() &&
                                     ((operation == 3 || operation == 14) ? observed.toBool() == (value != 0)
                                      : operation >= 20                   ? observed.toInt() == value
                                                                          : observed.toString() == target);
                m_globalStatus =
                    matches ? QStringLiteral("BEACN confirmed the selection")
                            : QStringLiteral("BEACN has not confirmed this selection; check its app");
                appendLog(QStringLiteral("GLOBAL READBACK operation=%1 requested=%2 observed=%3 matched=%4")
                              .arg(operation)
                              .arg((operation == 3 || operation >= 20) ? QString::number(value) : target)
                              .arg(observed.toString())
                              .arg(matches));
                emit globalStateChanged();
            }
            exportRecording(false);
        });
    return true;
#else
    Q_UNUSED(operation);
    Q_UNUSED(value);
    Q_UNUSED(target);
    return false;
#endif
}
