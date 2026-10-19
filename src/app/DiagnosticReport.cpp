#include "DiagnosticReport.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QRegularExpression>
#include <QStringList>
#include <algorithm>

static QString redact(QString text, const QStringList &secrets) {
    for (const auto &secret : secrets)
        text.replace(secret, "[redacted]", Qt::CaseInsensitive);
    text.replace(QRegularExpression(R"(https?://[^\s<>"']+)", QRegularExpression::CaseInsensitiveOption),
                 "[URL]");
    text.replace(QRegularExpression(R"(\b(?:\d{1,3}\.){3}\d{1,3}\b)"), "[IP]");
    text.replace(QRegularExpression(R"(\b(?:[a-fA-F0-9]{0,4}:){3,}[a-fA-F0-9:%]+\b)"), "[IPv6]");
    text.replace(QRegularExpression(R"(\b[A-Za-z]:[\\/][^\r\n"<>]+)"), "[path]");
    text.replace(QRegularExpression(R"(\\\\[^\s]+)"), "[path]");
    text.replace(QRegularExpression(R"([\w.+-]+@[\w.-]+\.[A-Za-z]{2,})"), "[email]");
    text.replace(QRegularExpression(R"(\b[a-fA-F0-9]{8}-[a-fA-F0-9-]{27,}\b)"), "[ID]");
    text.replace(QRegularExpression(R"(\b[a-fA-F0-9]{32,}\b)"), "[ID]");
    text.replace(QRegularExpression(
                     R"(\b(?!\d{4}-\d{2}-\d{2}(?:T|\b))(?=[A-Z0-9-]{10,}\b)(?=[A-Z0-9-]*\d)[A-Z0-9-]+\b)"),
                 "[serial/model-code]");
    return text;
}

QString DiagnosticReport::activity(QString text) {
    static const QRegularExpression privateOperation(
        "command|notification|foreground|clipboard|profile|preset|token|password|secret|fingerprint|authorization",
        QRegularExpression::CaseInsensitiveOption);
    if (privateOperation.match(text).hasMatch()) {
        const bool failed = text.contains("fail", Qt::CaseInsensitive) ||
                            text.contains("reject", Qt::CaseInsensitive) ||
                            text.contains("error", Qt::CaseInsensitive);
        QString result =
            failed ? "Operation failed; private details omitted" : "Private operation details omitted";
        static const QRegularExpression codes("\\b(state|bridge|Win32|exit|PID|code)[=: ]+(-?[0-9]{1,10})\\b",
                                              QRegularExpression::CaseInsensitiveOption);
        auto matches = codes.globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            result += " " + match.captured(1) + "=" + match.captured(2);
        }
        return result;
    }
    text.replace('\n', ' ');
    text.replace('\r', ' ');
    return redact(text, {}).left(2000);
}

QString DiagnosticReport::moduleForMessage(const QString &message) {
    if (message.startsWith("WHAT ") || message.startsWith("Configuration ") ||
        message.startsWith("Worker threads:") || message.startsWith("Streaming:"))
        return "WHAT";
    if (message.contains("iCUE Automation", Qt::CaseInsensitive))
        return "iCUE Automation";
    if (message.contains("Dynamic Lighting", Qt::CaseInsensitive))
        return "Dynamic Lighting";
    if (message.startsWith("Home Assistant Web:"))
        return "Home Assistant Web";
    for (const auto *name : {"BEACN", "HWiNFO", "Core Temp", "NVIDIA", "iCUE", "Wallpaper", "Media",
                             "Lighting", "MQTT", "WebSocket", "Update", "Notification", "Storage", "Display",
                             "Custom", "Announcement"})
        if (message.contains(QString::fromLatin1(name), Qt::CaseInsensitive))
            return QString::fromLatin1(name);
    return "WHAT";
}

QString DiagnosticReport::build(const QJsonObject &s) {
    QStringList secrets;
    for (auto value : s["private"].toArray())
        if (value.toString().size() >= 3)
            secrets.append(value.toString());
    std::sort(secrets.begin(), secrets.end(),
              [](const QString &a, const QString &b) { return a.size() > b.size(); });
    auto clean = [&](QString text) { return redact(text, secrets); };
    QString report = "WHAT diagnostic report\n";
    report +=
        "Redacted by default. Review before sharing; custom device labels may contain personal information.\n";
    report +=
        "Sensor values, command text/output, notification contents, pairing secrets and arbitrary settings are excluded.\n\n";
    report += QString::fromUtf8(QJsonDocument(s["system"].toObject()).toJson(QJsonDocument::Indented));
    report += "\nMODULES / CONNECTION\n" +
              QString::fromUtf8(QJsonDocument(s["state"].toObject()).toJson(QJsonDocument::Indented));
    report += "\nSAFE SETTINGS\n" +
              QString::fromUtf8(QJsonDocument(s["settings"].toObject()).toJson(QJsonDocument::Indented));
    report += "\nSOURCE DETAILS\n" + s["details"].toString();
    report += "\nAPI DEVICE / CONTROL INVENTORY\n" +
              QString::fromUtf8(QJsonDocument(s["devices"].toArray()).toJson(QJsonDocument::Indented));
    QMap<QString, QMap<QString, QStringList>> tree;
    int count = 0;
    for (auto value : s["readings"].toArray()) {
        const auto row = value.toObject();
        auto oneLine = [](QString text) {
            text.replace('\n', ' ');
            text.replace('\r', ' ');
            return text.left(240);
        };
        const auto source = oneLine(row["source"].toString()), group = oneLine(row["group"].toString());
        QString label = oneLine(row["name"].toString());
        label += QString(" | %1 | %2 | unit: %3")
                     .arg(row["available"].toBool() ? "available" : "unavailable",
                          row["shared"].toBool() ? "shared" : "not shared", oneLine(row["unit"].toString()));
        tree[source][group].append(label);
        ++count;
    }
    report +=
        QString("\n\nDISCOVERED SENSOR TREE (%1 cached readings; includes disabled modules)\n").arg(count);
    for (auto source = tree.cbegin(); source != tree.cend(); ++source) {
        report += "\n" + source.key() + "\n";
        int device = 0;
        for (auto group = source.value().cbegin(); group != source.value().cend(); ++group) {
            report += QString("  + Device/group %1: %2 (%3 readings)\n")
                          .arg(++device)
                          .arg(group.key())
                          .arg(group.value().size());
            for (const auto &reading : group.value())
                report += "    - " + reading + "\n";
        }
    }
    report += "\nRECENT ACTIVITY\n";
    const QRegularExpression sensitive("command|notification|foreground|clipboard|profile|preset",
                                       QRegularExpression::CaseInsensitiveOption);
    for (auto value : s["logs"].toArray()) {
        auto line = value.toString();
        if (sensitive.match(line).hasMatch())
            line = line.left(19) + "  [Private operation details omitted]";
        report += line.left(2000) + "\n";
    }
    report = clean(report);
    // Four-part software versions resemble IPv4 addresses. Only these named numeric fields
    // bypass address redaction; free text and every other field remain fully redacted.
    const auto versions = s["versions"].toObject();
    const QRegularExpression numericVersion("^[0-9]+(?:\\.[0-9]+){0,3}$");
    report += "\nSOFTWARE VERSIONS\n";
    for (auto label : {"BEACN app", "BEACN driver", "BEACN API", "BEACN mixer", "HWiNFO app",
                       "Core Temp app", "Core Temp shared memory", "NVIDIA driver", "NVIDIA NVML library", "iCUE app",
                       "iCUE SDK client", "iCUE SDK server", "iCUE Automation server",
                       "Wallpaper Engine app", "Windows helper", "WebView2 Runtime"}) {
        const auto version = versions[label].toVariant().toString();
        report += QString::fromLatin1(label) + ": " +
            (numericVersion.match(version).hasMatch() ? version : QString("Not reported")) + "\n";
    }
    return report;
}
