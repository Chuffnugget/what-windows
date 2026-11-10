#include "modules/hwinfo/HwinfoReader.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QtEndian>
#include <cstring>
#include <iostream>
#include <limits>
#include <windows.h>
#include <thread>
#include <future>

void put(QByteArray &b, int at, quint32 value) {
    qToLittleEndian(value, b.data() + at);
}

void require(bool ok, const char *what) {
    if (!ok) {
        std::cerr << what << '\n';
        std::exit(1);
    }
}

QByteArray fixture() {
    QByteArray b(48 + 264 + 316, '\0');
    put(b, 0, 0x53695748);
    put(b, 4, 1);
    put(b, 8, 1);
    qToLittleEndian(quint64(1700000000), b.data() + 12);
    put(b, 20, 48);
    put(b, 24, 264);
    put(b, 28, 1);
    put(b, 32, 312);
    put(b, 36, 316);
    put(b, 40, 1);
    put(b, 44, 2000);
    put(b, 48, 42);
    put(b, 52, 2);
    memcpy(b.data() + 56, "CPU", 4);
    put(b, 312, 1);
    put(b, 320, 99);
    memcpy(b.data() + 324, "CPU Package", 12);
    memcpy(b.data() + 580, "C", 2);
    double temperature = 62.5;
    memcpy(b.data() + 596, &temperature, 8);
    return b;
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--live")) {
        const auto live = HwinfoReader().read();
        std::cout << "Live HWiNFO: " << live.status.toStdString() << "; readings=" << live.readings.size()
                  << "; poll_time=" << live.pollTime << '\n';
        return live.valid && !live.readings.isEmpty() ? 0 : 1;
    }
    auto bytes = fixture();
    auto parsed = HwinfoReader::parse(bytes);
    require(bytes.left(4) == QByteArray("HWiS"),
            "fixture signature must match literal bytes from live HWiNFO");
    // Captured live v2 header: 392-byte sensor and 460-byte reading strides.
    QByteArray liveShape = QByteArray::fromHex(
        "485769530200000001000000b1fb9e6a0000000030000000880100001e000000202e0000cc010000f1020000d0070000");
    liveShape.resize(11808 + 460 * 753);
    liveShape.fill('\0', liveShape.size());
    const auto header = QByteArray::fromHex(
        "485769530200000001000000b1fb9e6a0000000030000000880100001e000000202e0000cc010000f1020000d0070000");
    memcpy(liveShape.data(), header.data(), header.size());
    require(HwinfoReader::parse(liveShape).valid, "live v2 header layout rejected");
    require(parsed.valid && parsed.readings.size() == 1, "valid fixture rejected");
    require(parsed.readings[0].toObject()["id"] == "0000002a_00000002_00000063", "unstable identifier");
    require(parsed.readings[0].toObject()["value"].toDouble() == 62.5, "wrong value offset");
    auto pending = bytes;
    qToLittleEndian(quint64(0), pending.data() + 12);
    const auto waiting = HwinfoReader::parse(pending);
    require(!waiting.valid && waiting.status.contains("waiting for the first sensor sample"),
            "connected reader without an initial sample must explain the wait");
    auto empty = bytes;
    put(empty, 28, 0);
    put(empty, 40, 0);
    const auto noSensors = HwinfoReader::parse(empty);
    require(!noSensors.valid && noSensors.status.contains("connected, but no sensors"),
            "empty sensor catalogue incorrectly reported healthy");
    put(empty, 28, 1);
    const auto noReadings = HwinfoReader::parse(empty);
    require(!noReadings.valid && noReadings.status.contains("connected, but no readings"),
            "groups without readings incorrectly reported healthy");
    auto extended = fixture();
    const auto sensor = extended.mid(48, 264), reading = extended.mid(312, 316);
    extended.resize(48 + 392 + 460);
    extended.fill('\0');
    memcpy(extended.data(), bytes.constData(), 48);
    put(extended, 4, 2);
    put(extended, 24, 392);
    put(extended, 32, 440);
    put(extended, 36, 460);
    memcpy(extended.data() + 48, sensor.constData(), sensor.size());
    memcpy(extended.data() + 440, reading.constData(), reading.size());
    const QByteArray group = QString::fromUtf8("Процессор").toUtf8();
    const QByteArray label = QString::fromUtf8("核心温度").toUtf8();
    const QByteArray unit = QString::fromUtf8("°C").toUtf8();
    memcpy(extended.data() + 48 + 264, group.constData(), group.size());
    memcpy(extended.data() + 440 + 316, label.constData(), label.size());
    memcpy(extended.data() + 440 + 444, unit.constData(), unit.size());
    const auto unicode = HwinfoReader::parse(extended).readings.first().toObject();
    require(unicode["group"] == QString::fromUtf8(group), "UTF-8 group lost");
    require(unicode["name"] == QString::fromUtf8(label), "UTF-8 label lost");
    require(unicode["unit"] == QString::fromUtf8(unit), "UTF-8 unit lost");
    extended[440 + 316] = char(0xff);
    require(HwinfoReader::parse(extended).readings.first().toObject()["name"] == "CPU Package",
            "invalid UTF-8 must use the bounded legacy field");

    // A real mutex timeout must retain the timestamp, while a stopped producer must fail.
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         DWORD(bytes.size()), L"Local\\WHAT_TEST_SM2");
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\WHAT_TEST_MUTEX");
    require(mapping && mutex, "test shared memory could not be created");
    auto view = static_cast<char *>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, bytes.size()));
    require(view, "test view could not be mapped");
    memcpy(view, bytes.constData(), bytes.size());
    HwinfoReader cached;
    const auto first = cached.read(true);
    require(first.valid, "initial shared-memory read failed");
    std::promise<void> locked, release;
    auto released = release.get_future();
    std::thread holder([&] {
        WaitForSingleObject(mutex, INFINITE);
        locked.set_value();
        released.wait();
        ReleaseMutex(mutex);
    });
    locked.get_future().wait();
    const auto busy = cached.read(true);
    const auto busyWithoutSample = HwinfoReader().read(true);
    release.set_value();
    holder.join();
    require(busy.valid && busy.pollTime == first.pollTime && busy.readings == first.readings,
            "mutex contention discarded or refreshed the cached sample");
    require(!busyWithoutSample.valid && busyWithoutSample.status.contains("shared memory is busy"),
            "initial mutex contention must be distinguished from missing shared memory");
    require(busy.details.contains("retaining the last sample"),
            "cached read must explain contention in diagnostics");
    memcpy(view, "DEAD", 4);
    // A stopped publisher can clear its offsets. Still report stopped, not malformed data.
    memset(view + 20, 0xff, 24);
    const auto stopped = cached.read(true);
    require(!stopped.valid && stopped.status.contains("stopped publishing") &&
                stopped.details.contains("DEAD"),
            "stopped producer was misdiagnosed or served cached readings");
    memcpy(view, bytes.constData(), bytes.size());
    require(cached.read(true).valid, "reader did not recover after publishing resumed");
    CloseHandle(mutex);
    const auto noLock = cached.read(true);
    require(!noLock.valid && noLock.status.contains("reading lock is unavailable") &&
                noLock.details.contains("OpenMutex(Local\\WHAT_TEST_MUTEX): Windows error 2"),
            "missing reading lock must preserve its specific failure and Windows code");
    mutex = CreateMutexW(nullptr, FALSE, L"Local\\WHAT_TEST_MUTEX");
    require(mutex, "could not recreate test mutex");
    std::promise<void> relocked, rerelease;
    auto rereleased = rerelease.get_future();
    std::thread newHolder([&] {
        WaitForSingleObject(mutex, INFINITE);
        relocked.set_value();
        rereleased.wait();
        ReleaseMutex(mutex);
    });
    relocked.get_future().wait();
    const auto reconnectBusy = cached.read(true);
    rerelease.set_value();
    newHolder.join();
    require(!reconnectBusy.valid && reconnectBusy.status.contains("shared memory is busy"),
            "reconnection resurrected a cached sample from a failed connection");
    require(cached.read(true).valid, "reader did not recover after lock contention");
    put(empty, 28, 0);
    memcpy(view, empty.constData(), empty.size());
    require(!cached.read(true).valid && cached.read(true).status.contains("no sensors"),
            "empty live catalogue did not clear a previous healthy sample");
    UnmapViewOfFile(view);
    CloseHandle(mutex);
    CloseHandle(mapping);
    const auto missing = cached.read(true);
    require(!missing.valid && missing.status.contains("Cannot connect") &&
                missing.details.contains("OpenFileMapping(Local\\WHAT_TEST_SM2): Windows error 2"),
            "missing mapping must explain connection failure and retain the real mapping name/code");

    // Deny new readers of this test-only mapping without changing any real app or user permissions.
    SECURITY_DESCRIPTOR descriptor{};
    ACL denyAll{};
    require(InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) &&
                InitializeAcl(&denyAll, sizeof(denyAll), ACL_REVISION) &&
                SetSecurityDescriptorDacl(&descriptor, TRUE, &denyAll, FALSE),
            "cannot create test-only access-denied descriptor");
    SECURITY_ATTRIBUTES security{sizeof(security), &descriptor, FALSE};
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0, DWORD(bytes.size()),
                                 L"Local\\WHAT_TEST_SM2");
    require(mapping, "cannot create test-only protected mapping");
    const auto denied = cached.read(true);
    CloseHandle(mapping);
    require(!denied.valid && denied.status.contains("Windows denied access") &&
                denied.details.contains("Windows error 5"),
            "access denied was misdiagnosed as a missing mapping");

    auto rename = bytes;
    memcpy(rename.data() + 452, "Renamed", 8);
    require(HwinfoReader::parse(rename).readings[0].toObject()["id"] == parsed.readings[0].toObject()["id"],
            "rename changed identity");
    require(!HwinfoReader::parse(bytes.left(43)).valid, "truncated header accepted");
    require(!HwinfoReader::parse(bytes.left(400)).valid, "truncated reading accepted");
    auto invalid = bytes;
    put(invalid, 40, 0xffffffff);
    require(!HwinfoReader::parse(invalid).valid, "overflow accepted");
    invalid = bytes;
    put(invalid, 316, 99);
    require(!HwinfoReader::parse(invalid).valid, "bad index accepted");
    invalid = bytes;
    put(invalid, 0, 0x44414544);
    require(!HwinfoReader::parse(invalid).valid, "inactive signature accepted");
    invalid = bytes;
    put(invalid, 4, 999);
    const auto unsupported = HwinfoReader::parse(invalid);
    require(!unsupported.valid && unsupported.status.contains("unsupported shared-memory version") &&
                unsupported.details.contains("999"),
            "unknown version must explain compatibility failure");
    double nan = std::numeric_limits<double>::quiet_NaN();
    memcpy(bytes.data() + 596, &nan, 8);
    require(HwinfoReader::parse(bytes).readings[0].toObject()["value"].isNull(), "NaN was not null");
    std::cout
        << "Shared-memory parser, Unicode, failure diagnosis, recovery and mutex contention checks passed\n";
}
