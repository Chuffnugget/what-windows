#pragma once
#include <QString>

// These read-only native queries run on the specifications worker, never in QML.
QString graphicsSpecifications();
QString firmwareSpecifications();
QString windowsActivationStatus();
QString inputSpecifications();
QString inputDeviceName(const QString &friendlyName, const QString &description, const QString &productName);
