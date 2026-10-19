#pragma once
#include <QByteArray>
#include <QJsonObject>

namespace CoreTemp {
QJsonObject parse(const QByteArray &bytes, bool extended = true);
QJsonObject read();
} // namespace CoreTemp
