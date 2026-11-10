#include "modules/hwinfo/HwinfoReader.h"
#include <QJsonObject>
#include <QStringDecoder>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <windows.h>
#include <tlhelp32.h>

namespace {
constexpr quint64 MaxMap = 32 * 1024 * 1024;

quint32 u32(const char *p) {
    return qFromLittleEndian<quint32>(p);
}

quint64 u64(const char *p) {
    return qFromLittleEndian<quint64>(p);
}

QString str(const char *p, int size) {
    int n = 0;
    while (n < size && p[n])
        ++n;
    return QString::fromLocal8Bit(p, n).trimmed();
}

QString extendedText(const char *record, quint32 stride, quint32 offset, quint32 length) {
    if (stride < offset + length)
        return {};
    const auto bytes = QByteArrayView(record + offset, length);
    const auto end = bytes.indexOf('\0');
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder(end < 0 ? bytes : bytes.first(end));
    return decoder.hasError() ? QString() : text.trimmed();
}

bool section(quint32 offset, quint32 stride, quint32 count, quint32 minimum, quint64 size) {
    return offset >= 44 && stride >= minimum && count <= 10000 &&
           quint64(offset) + quint64(stride) * count <= size;
}

HwinfoSnapshot error(const QString &s, const QString &details = {}) {
    HwinfoSnapshot r;
    r.status = s;
    r.details = details;
    return r;
}

QString windowsError(const QString &operation, DWORD code) {
    return QString("%1: Windows error %2").arg(operation).arg(code);
}

const QString MissingMemory =
    "Cannot connect to HWiNFO shared memory. Open Sensors in HWiNFO and enable Shared Memory Support.";
} // namespace

HwinfoSnapshot HwinfoReader::parse(const QByteArray &bytes) {
    if (bytes.size() < 44)
        return error(
            "HWiNFO shared memory contains an incomplete header. Restart HWiNFO; check for updates if this continues.",
            QString("Header truncated: %1 bytes; at least 44 required").arg(bytes.size()));
    const char *h = bytes.constData();
    // The mapping contains literal ASCII bytes H W i S. Decode as bytes, not a
    // compiler multi-character integer literal (which reverses this on Windows).
    if (bytes.startsWith("DEAD"))
        return error(
            "HWiNFO has stopped publishing shared memory. Reopen Sensors and check Shared Memory Support in HWiNFO settings.",
            "Publisher marked the mapping DEAD. The mapping does not identify whether monitoring stopped, sharing was disabled or its time limit expired.");
    if (!bytes.startsWith("HWiS"))
        return error(
            "HWiNFO shared-memory format is not recognised. Update WHAT and HWiNFO; this does not mean sharing is disabled.",
            QString("Unrecognised signature: %1").arg(QString::fromLatin1(bytes.left(4).toHex(' '))));
    if (u32(h + 4) != 2 && u32(h + 4) != 1)
        return error("HWiNFO uses an unsupported shared-memory version. Check for a WHAT update.",
                     QString("Unsupported version %1; revision %2").arg(u32(h + 4)).arg(u32(h + 8)));
    const auto so = u32(h + 20), ss = u32(h + 24), sn = u32(h + 28);
    const auto ro = u32(h + 32), rs = u32(h + 36), rn = u32(h + 40);
    HwinfoSnapshot result;
    result.details =
        QString("Signature HWiS; version %1, revision %2; %3 sensor groups, %4 readings; strides %5/%6 bytes")
            .arg(u32(h + 4))
            .arg(u32(h + 8))
            .arg(sn)
            .arg(rn)
            .arg(ss)
            .arg(rs);
    result.pollTime = qint64(u64(h + 12));
    if (u32(h + 8) >= 1 && so >= 48 && bytes.size() >= 48)
        result.pollPeriod = qBound(100u, u32(h + 44), 3600000u);
    // An initial or empty catalogue is not a working sensor connection. Do not
    // infer that the Sensors window is closed: disabled monitoring looks the same.
    if (!sn && !rn) {
        result.status =
            result.pollTime > 0
                ? "HWiNFO shared memory is connected, but no sensors are published. Open Sensors and check that monitoring is enabled."
                : "HWiNFO shared memory is connected; waiting for the first sensor sample. Open Sensors if this continues.";
        return result;
    }
    if (!section(so, ss, sn, 264, bytes.size()) || !section(ro, rs, rn, 316, bytes.size()))
        return error(
            "HWiNFO shared memory contains invalid sensor data. Restart HWiNFO; check for updates if this continues.",
            "Invalid section bounds. " + result.details);
    if (!rn) {
        result.status =
            "HWiNFO shared memory is connected, but no readings are published. Check sensor monitoring in HWiNFO.";
        return result;
    }
    for (quint32 i = 0; i < rn; ++i) {
        const char *r = h + ro + quint64(i) * rs;
        const auto index = u32(r + 4);
        if (index >= sn)
            return error(
                "HWiNFO shared memory contains invalid sensor data. Restart HWiNFO; check for updates if this continues.",
                QString("Reading %1 references invalid sensor index %2. %3")
                    .arg(i)
                    .arg(index)
                    .arg(result.details));
        const char *s = h + so + quint64(index) * ss;
        const auto sensorId = u32(s), instance = u32(s + 4), readingId = u32(r + 8);
        // Version 2 appends UTF-8 copies. Record strides, not the revision,
        // determine whether each optional field is present.
        const bool unicode = u32(h + 4) >= 2;
        QString group = unicode ? extendedText(s, ss, 264, 128) : QString();
        if (group.isEmpty())
            group = str(s + 136, 128);
        if (group.isEmpty())
            group = str(s + 8, 128);
        QString label = unicode ? extendedText(r, rs, 316, 128) : QString();
        if (label.isEmpty())
            label = str(r + 140, 128);
        if (label.isEmpty())
            label = str(r + 12, 128);
        QString unit = unicode ? extendedText(r, rs, 444, 16) : QString();
        if (unit.isEmpty())
            unit = str(r + 268, 16);
        const quint64 bits = u64(r + 284);
        double value;
        std::memcpy(&value, &bits, sizeof(value));
        result.readings.append(
            QJsonObject{{"id", QString("%1_%2_%3")
                                   .arg(sensorId, 8, 16, QLatin1Char('0'))
                                   .arg(instance, 8, 16, QLatin1Char('0'))
                                   .arg(readingId, 8, 16, QLatin1Char('0'))},
                        {"group", group},
                        {"name", label},
                        {"unit", unit},
                        {"type", int(u32(r))},
                        {"value", std::isfinite(value) ? QJsonValue(value) : QJsonValue()}});
    }
    result.valid = result.pollTime > 0;
    result.status =
        result.valid
            ? "Shared memory connected"
            : "HWiNFO shared memory is connected; waiting for the first sensor sample. Open Sensors if this continues.";
    return result;
}

QString HwinfoReader::missingMemoryStatus() const {
    // Called on the HWiNFO worker only, and only while the mapping is absent.
    // Never enumerate every process at the normal 100 ms sensor check interval.
    if (processCheckAge.isValid() && processCheckAge.elapsed() < 5000)
        return missingMemoryMessage;
    missingMemoryMessage = MissingMemory;
    processCheckAge.start();
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return missingMemoryMessage;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    bool found = false;
    BOOL more = Process32FirstW(snapshot, &entry);
    while (more) {
        if (!_wcsicmp(entry.szExeFile, L"HWiNFO64.exe") || !_wcsicmp(entry.szExeFile, L"HWiNFO32.exe") ||
            !_wcsicmp(entry.szExeFile, L"HWiNFO_ARM64.exe")) {
            found = true;
            break;
        }
        more = Process32NextW(snapshot, &entry);
    }
    const bool complete = found || GetLastError() == ERROR_NO_MORE_FILES;
    CloseHandle(snapshot);
    if (found)
        missingMemoryMessage =
            "HWiNFO is running, but its sensor shared memory is unavailable. Open Sensors and enable Shared Memory Support in HWiNFO settings.";
    else if (complete)
        missingMemoryMessage =
            "No running HWiNFO process was detected. Start HWiNFO, open Sensors and enable Shared Memory Support.";
    return missingMemoryMessage;
}

HwinfoSnapshot HwinfoReader::read(bool testMode) const {
    const auto fail = [this](const QString &status, const QString &details) {
        // A later busy read must not resurrect samples from a lost connection.
        previousBytes.clear();
        previousSnapshot = {};
        return error(status, details);
    };
    const auto mappingName = testMode ? L"Local\\WHAT_TEST_SM2" : L"Global\\HWiNFO_SENS_SM2";
    const auto mutexName = testMode ? L"Local\\WHAT_TEST_MUTEX" : L"Global\\HWiNFO_SM2_MUTEX";
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, mappingName);
    if (!mapping) {
        const DWORD code = GetLastError();
        const auto details =
            windowsError("OpenFileMapping(" + QString::fromWCharArray(mappingName) + ")", code);
        if (code == ERROR_ACCESS_DENIED)
            return fail(
                "Windows denied access to HWiNFO shared memory. Check that WHAT and HWiNFO run under the same Windows user and permission level.",
                details);
        if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            return fail(testMode ? MissingMemory : missingMemoryStatus(), details);
        return fail("Cannot open HWiNFO shared memory. See Diagnostics for the Windows error.", details);
    }
    processCheckAge.invalidate();
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName);
    if (!mutex) {
        const DWORD code = GetLastError();
        CloseHandle(mapping);
        return fail(
            code == ERROR_ACCESS_DENIED
                ? "HWiNFO shared memory was found, but Windows denied access to its reading lock. Check the apps' user and permission levels."
                : "HWiNFO shared memory was found, but its reading lock is unavailable. HWiNFO may be starting or stopping; restart it if this continues.",
            windowsError("OpenMutex(" + QString::fromWCharArray(mutexName) + ")", code));
    }
    const DWORD wait = WaitForSingleObject(mutex, 25);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        const DWORD code = wait == WAIT_FAILED ? GetLastError() : 0;
        CloseHandle(mutex);
        CloseHandle(mapping);
        if (wait == WAIT_TIMEOUT) {
            if (previousSnapshot.valid) {
                auto held = previousSnapshot;
                held.details +=
                    "; HWiNFO reading lock busy for 25 ms; retaining the last sample and its original timestamp";
                return held;
            }
            return fail("HWiNFO shared memory is busy. Waiting for HWiNFO to finish updating its sensors.",
                        "WaitForSingleObject: reading lock busy for 25 ms; no previous sample available");
        }
        return fail("Cannot synchronise with HWiNFO shared memory. Restart HWiNFO if this continues.",
                    windowsError("WaitForSingleObject", code) + QString("; wait result %1").arg(wait));
    }
    const char *view = static_cast<const char *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    QByteArray bytes;
    QString copyStatus, copyDetails;
    if (view) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(view, &info, sizeof(info))) {
            copyStatus = "Cannot inspect HWiNFO shared memory safely. See Diagnostics for the Windows error.";
            copyDetails = windowsError("VirtualQuery", GetLastError());
        } else if (info.RegionSize < 44) {
            bytes = QByteArray(view, qsizetype(info.RegionSize));
        } else if (std::memcmp(view, "HWiS", 4) || (u32(view + 4) != 1 && u32(view + 4) != 2)) {
            // A stopped publisher or unknown format need not have usable offsets.
            bytes = QByteArray(view, 44);
        } else {
            const quint64 sensorEnd = quint64(u32(view + 20)) + quint64(u32(view + 24)) * u32(view + 28);
            const quint64 readingEnd = quint64(u32(view + 32)) + quint64(u32(view + 36)) * u32(view + 40);
            const quint64 length = qMax(quint64(48), qMax(sensorEnd, readingEnd));
            if (length <= MaxMap && length <= info.RegionSize)
                bytes = QByteArray(view, qsizetype(length));
            else {
                copyStatus =
                    "HWiNFO shared memory contains invalid sensor data. Restart HWiNFO; check for updates if this continues.";
                copyDetails = QString("Unsafe mapping bounds: requested %1 bytes; mapped %2; limit %3")
                                  .arg(length)
                                  .arg(quint64(info.RegionSize))
                                  .arg(MaxMap);
            }
        }
        UnmapViewOfFile(view);
    } else {
        const DWORD code = GetLastError();
        copyStatus =
            code == ERROR_ACCESS_DENIED
                ? "Windows denied access to the HWiNFO shared-memory view. Check the apps' user and permission levels."
                : "HWiNFO shared memory was found, but WHAT could not map its data. See Diagnostics for the Windows error.";
        copyDetails = windowsError("MapViewOfFile", code);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    CloseHandle(mapping);
    if (!copyStatus.isEmpty())
        return fail(copyStatus, copyDetails);
    if (bytes != previousBytes || previousBytes.isEmpty()) {
        previousSnapshot = parse(bytes);
        previousBytes = bytes;
    }
    return previousSnapshot;
}
