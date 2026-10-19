#pragma once
#include <QJsonObject>
#include <functional>

namespace ShellCommand {
QString validate(const QJsonObject &args);
QJsonObject run(const QJsonObject &args, const std::function<bool()> &cancelled,
                const std::function<void(bool, const QString &)> &stream = {});
} // namespace ShellCommand
