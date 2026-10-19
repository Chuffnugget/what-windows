#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QString>

struct HwinfoSnapshot {
    bool valid = false;
    QString status;
    QString details;
    qint64 pollTime = 0;
    quint32 pollPeriod = 2000;
    QJsonArray readings;
};

// Own parser; no bundled HWiNFO SDK/header or proprietary binaries.
class HwinfoReader {
public:
    static HwinfoSnapshot parse(const QByteArray &bytes);
    HwinfoSnapshot read(bool testMode = false) const;

private:
    mutable QByteArray previousBytes;
    mutable HwinfoSnapshot previousSnapshot;
};
