#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>

namespace InstallationConfig {
inline QString validate(const QJsonObject &value) {
    static const QRegularExpression identifier("^[a-f0-9]{32}$");
    const auto id = value["id"].toString();
    const auto name = value["name"].toString();
    if (!identifier.match(id).hasMatch() || name.trimmed().isEmpty() || name.size() > 80 ||
        name.contains(QRegularExpression("[\\x00-\\x1f]")))
        return "Enter an installation name of 1 to 80 characters.";
    if (!value["enabled"].isBool() || !value["controls"].isBool())
        return "Invalid installation permissions.";
    if (value["type"] != "api" && value["type"] != "mqtt")
        return "Choose Home Assistant API or MQTT.";
    {
        if (!value["url"].isString())
            return "Enter a Home Assistant URL or leave it empty for local pairing.";
        const auto address = value["url"].toString();

        const QUrl url(address, QUrl::StrictMode);
        if (!address.isEmpty() && (address.size() > 2048 || !url.isValid() || url.scheme() != "https" ||
                                   url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() ||
                                   url.hasFragment() || (!url.path().isEmpty() && url.path() != "/")))
            return "Use the Home Assistant HTTPS address, without a path, query or embedded login.";
    }
    const auto mqtt = value["mqtt"].toObject();
    const auto host = mqtt["host"].toString();
    if (host.isEmpty() && value["type"] != "mqtt")
        return {};
    const int port = mqtt["port"].toInt();
    if (host.isEmpty() || host.size() > 253 || host.contains(QRegularExpression("[\\s/@?#\\x00]")) ||
        port < 1 || port > 65535 || mqtt["port"].toDouble() != port || !mqtt["tls"].isBool() ||
        (mqtt["scope"] != "local" && mqtt["scope"] != "cloud") ||
        (mqtt["scope"] == "cloud" && !mqtt["tls"].toBool()))
        return "Enter a broker hostname and port. Cloud connections require TLS.";
    for (const auto *key : {"username", "ca", "certificate", "key"})
        if (!mqtt[key].isString() || mqtt[key].toString().size() > 32767 ||
            mqtt[key].toString().contains(QChar(0)))
            return "Invalid MQTT username or certificate path.";
    if (mqtt["certificate"].toString().isEmpty() != mqtt["key"].toString().isEmpty())
        return "Provide both the MQTT client certificate and its private key.";
    return {};
}

inline QJsonObject sanitise(QJsonObject value) {
    QJsonObject result;
    for (const auto *key : {"id", "name", "type", "enabled", "controls", "url"})
        result[key] = value[key];
    QJsonObject mqtt;
    for (const auto *key : {"host", "port", "tls", "scope", "username", "ca", "certificate", "key"})
        if (value["mqtt"].toObject().contains(key))
            mqtt[key] = value["mqtt"].toObject()[key];
    result["mqtt"] = mqtt;
    return result;
}
} // namespace InstallationConfig
