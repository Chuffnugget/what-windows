#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QElapsedTimer>

// Used by one background worker at a time. Never touches the GUI or HWiNFO.
class WindowsCollector {
public:
    QJsonArray collect(const QJsonObject &options);
    void restoreVolumes(const QJsonArray &catalog);
    QString wifiIssue() const { return wifiAccessIssue; }

private:
    QMap<QString, QJsonObject> volumes;
    QMap<QString, QPair<quint64, quint64>> networkBytes;
    QElapsedTimer networkClock, slowClock, bridgeClock;
    QJsonArray slowRows, bridgeRows, securityRows;
    QString wifiAccessIssue;
};
