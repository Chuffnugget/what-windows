#pragma once
#include <QJsonObject>

namespace DiagnosticReport {
QString build(const QJsonObject &snapshot);
QString activity(QString text);
QString moduleForMessage(const QString &message);
} // namespace DiagnosticReport
