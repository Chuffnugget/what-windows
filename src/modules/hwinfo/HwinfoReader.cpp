#include "modules/hwinfo/HwinfoReader.h"
#include <QJsonObject>
#include <QStringDecoder>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <windows.h>

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

HwinfoSnapshot error(const QString &s) {
    HwinfoSnapshot r;
    r.status = s;
    return r;
}
} // namespace

HwinfoSnapshot HwinfoReader::parse(const QByteArray &bytes) {
    if (bytes.size() < 44)
        return error("Shared-memory header is truncated");
    const char *h = bytes.constData();
    // The mapping contains literal ASCII bytes H W i S. Decode as bytes, not a
    // compiler multi-character integer literal (which reverses this on Windows).
    if (bytes.startsWith("DEAD"))
        return error(
            "HWiNFO has stopped publishing shared memory. Open HWiNFO Settings > General / User Interface > Shared Memory Support.");
    if (!bytes.startsWith("HWiS"))
        return error(
            QString(
                "Unrecognised HWiNFO shared-memory signature %1; this is a reader compatibility issue, not proof that sharing is disabled")
                .arg(QString::fromLatin1(bytes.left(4).toHex(' '))));
    if (u32(h + 4) != 2 && u32(h + 4) != 1)
        return error("Unsupported HWiNFO shared-memory version");
    const auto so = u32(h + 20), ss = u32(h + 24), sn = u32(h + 28);
    const auto ro = u32(h + 32), rs = u32(h + 36), rn = u32(h + 40);
    if (!section(so, ss, sn, 264, bytes.size()) || !section(ro, rs, rn, 316, bytes.size()))
        return error("Invalid HWiNFO shared-memory section bounds");
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
    for (quint32 i = 0; i < rn; ++i) {
        const char *r = h + ro + quint64(i) * rs;
        const auto index = u32(r + 4);
        if (index >= sn)
            return error("Invalid HWiNFO sensor index");
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
    result.status = result.valid ? "Shared memory connected" : "Waiting for the first HWiNFO sample";
    return result;
}

HwinfoSnapshot HwinfoReader::read(bool testMode) const {
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE,
                                      testMode ? L"Local\\WHAT_TEST_SM2" : L"Global\\HWiNFO_SENS_SM2");
    if (!mapping) {
        const DWORD code = GetLastError();
        return error(
            QString("OpenFileMapping(Global\\HWiNFO_SENS_SM2) failed: Windows error %1. %2")
                .arg(code)
                .arg(
                    code == ERROR_ACCESS_DENIED
                        ? "Access denied; check process/user permissions."
                        : "Mapping not found or unavailable; check HWiNFO Sensors and Shared Memory Support."));
    }
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                              testMode ? L"Local\\WHAT_TEST_MUTEX" : L"Global\\HWiNFO_SM2_MUTEX");
    if (!mutex) {
        const DWORD code = GetLastError();
        CloseHandle(mapping);
        return error(QString("OpenMutex(Global\\HWiNFO_SM2_MUTEX) failed: Windows error %1").arg(code));
    }
    const DWORD wait = WaitForSingleObject(mutex, 25);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
        CloseHandle(mutex);
        CloseHandle(mapping);
        if (wait == WAIT_TIMEOUT && previousSnapshot.valid)
            return previousSnapshot;
        return error(QString("HWiNFO mutex wait failed/timed out after 25 ms; wait result %1").arg(wait));
    }
    const char *view = static_cast<const char *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0));
    QByteArray bytes;
    if (view) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(view, &info, sizeof(info)) && info.RegionSize >= 44) {
            const quint64 sensorEnd = quint64(u32(view + 20)) + quint64(u32(view + 24)) * u32(view + 28);
            const quint64 readingEnd = quint64(u32(view + 32)) + quint64(u32(view + 36)) * u32(view + 40);
            const quint64 length = qMax(quint64(48), qMax(sensorEnd, readingEnd));
            if (length <= MaxMap && length <= info.RegionSize)
                bytes = QByteArray(view, qsizetype(length));
        }
        UnmapViewOfFile(view);
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    CloseHandle(mapping);
    if (bytes.isEmpty())
        return error("Cannot copy HWiNFO shared memory safely");
    if (bytes != previousBytes) {
        previousSnapshot = parse(bytes);
        previousBytes = bytes;
    }
    return previousSnapshot;
}
