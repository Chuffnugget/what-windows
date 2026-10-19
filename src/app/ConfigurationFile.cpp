#include "app/UiMotion.h"
#include "ConfigurationFile.h"
#include "InstallationConfig.h"
#include "modules/haweb/WebOptions.h"
#include <QFile>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QUrl>
#include <cmath>

namespace ConfigurationFile {
namespace {
const QSet<QString> booleanKeys = {"start_minimised",
                                   "startup_notice",
                                   "ui/animations",
                                   "ui/window_animations",
                                   "ui/sidebar_collapsed",
                                   "updates/automatic",
                                   "updates/beta",
                                   "service_enabled",
                                   "remote/enabled",
                                   "browser/selected",
                                   "browser/unshared",
                                   "browser/favourites",
                                   "sources/hwinfo",
                                   "sources/session",
                                   "sources/wifi",
                                   "sources/auto_drives",
                                   "sources/foreground",
                                   "sources/window_titles",
                                   "sources/microphone",
                                   "sources/events",
                                   "sources/event_messages",
                                   "sources/media",
                                   "sources/media_titles",
                                   "sources/notifications",
                                   "sources/notification_contents",
                                   "sources/security",
                                   "modules/coretemp/enabled",
                                   "modules/nvidia/enabled",
                                   "modules/haweb/enabled",
                                   "modules/icue/enabled",
                                   "modules/windows/enabled",
                                   "modules/icue/direct",
                                   "modules/icue/automation",
                                   "modules/icue/automationControl",
                                   "modules/icue/control",
                                   "modules/windows/displays",
                                   "modules/windows/lighting",
                                   "modules/windows/lighting_cloud",
                                   "modules/beacn/enabled",
                                   "modules/beacn/control",
                                   "modules/beacn/cloud",
                                   "modules/wallpaper/enabled",
                                   "modules/wallpaper/control",
                                   "modules/wallpaper/share_path",
                                   "modules/wallpaper/cloud",
                                   "controls/what",
                                   "controls/media",
                                   "controls/power",
                                   "controls/power_plan",
                                   "controls/urls",
                                   "controls/monitor",
                                   "controls/shell",
                                   "controls/notifications",
                                   "controls/announcements",
                                   "controls/input",
                                   "controls/displays",
                                   "controls/dynamic_lighting"};
const QMap<QString, QPair<double, double>> ranges = {{"sample_seconds", {0.5, 60}},
                                                     {"publish_seconds", {1, 300}},
                                                     {"port", {1024, 65535}},
                                                     {"hwinfo_check_ms", {100, 10000}},
                                                     {"publishingNormalMs", {100, 60000}},
                                                     {"publishingHighMs", {50, 60000}},
                                                     {"modules/coretemp/poll_ms", {500, 60000}},
                                                     {"modules/nvidia/poll_ms", {500, 60000}},
                                                     {"modules/haweb/scale", {50, 200}},
                                                     {"ui/sidebar_width", {180, 360}},
                                                     {"ui/timing/hover", {0, 2000}},
                                                     {"ui/timing/tooltips", {0, 2000}},
                                                     {"ui/timing/controls", {0, 2000}},
                                                     {"ui/timing/panels", {0, 2000}},
                                                     {"ui/timing/lists", {0, 2000}},
                                                     {"ui/timing/pages", {0, 2000}},
                                                     {"ui/timing/titles", {0, 2000}},
                                                     {"ui/timing/dialogEnter", {0, 2000}},
                                                     {"ui/timing/dialogExit", {0, 2000}},
                                                     {"ui/timing/windowEnter", {0, 2000}},
                                                     {"ui/timing/windowExit", {0, 2000}},
                                                     {"ui/refraction_strength", {0, 150}},
                                                     {"ui/timing/logo", {250, 10000}},
                                                     {"ui/timing/logoPause", {0, 5000}},
                                                     {"ui/timing/toast", {500, 30000}}};

bool string(const QJsonValue &value, int maximum = 512) {
    return value.isString() && value.toString().size() <= maximum && !value.toString().contains(QChar(0));
}

bool number(const QJsonValue &value, double minimum, double maximum, bool integer = true) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= minimum &&
           value.toDouble() <= maximum && (!integer || std::floor(value.toDouble()) == value.toDouble());
}

bool valid(const QString &key, const QJsonValue &value) {
    if (key == "connections/installations") {
        if (!value.isArray()) return false;
        QSet<QString> ids;
        for (const auto &entry : value.toArray()) {
            const auto row = entry.toObject();
            const auto id = row["id"].toString();
            if (ids.contains(id) || !InstallationConfig::validate(row).isEmpty() ||
                InstallationConfig::sanitise(row) != row) return false;
            ids.insert(id);
        }
        return true;
    }
    if (key == "modules/haweb/options")
        return value.isObject() && WebOptions::valid(value.toObject().toVariantMap());
    if (booleanKeys.contains(key))
        return value.isBool();
    if (ranges.contains(key))
        return number(value, ranges[key].first, ranges[key].second, key != "sample_seconds");
    if (key == "modules/haweb/mode")
        return value == "windowed" || value == "fullscreen" || value == "desktop" || value == "pip";
    if (key == "modules/haweb/hotkey")
        return value.isString() && WebOptions::validHotkey(value.toString());
    if (key == "modules/haweb/url") {
        if (!string(value, 2048)) return false;
        const QUrl url(value.toString(), QUrl::StrictMode);
        return value.toString().isEmpty() || (url.isValid() && !url.host().isEmpty()
            && url.userInfo().isEmpty() && (url.scheme() == "http" || url.scheme() == "https"));
    }
    if (key == "ui/theme")
        return value.isString() && QStringList{"blue", "dark-grey", "slate", "forest", "plum"}.contains(value.toString());
    if (key == "ui/transition_style")
        return value.isString() && UiMotion::find(value.toString());
    if (key == "ui/window_mode")
        return value.isString() && QStringList{"normal", "maximised", "fullscreen"}.contains(value.toString());
    if (key == "license")
        return value.isString() && QStringList{"unknown", "free", "pro"}.contains(value.toString());
    if (key == "browser/source" || key == "browser/query")
        return string(value, 256);
    if (key == "remote/url") {
        if (!string(value, 2048))
            return false;
        if (value.toString().isEmpty())
            return true;
        const QUrl url(value.toString(), QUrl::StrictMode);
        return url.isValid() && url.scheme() == "https" && !url.host().isEmpty() &&
               url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment() &&
               (url.path().isEmpty() || url.path() == "/");
    }
    if (key == "colours/recent" || key == "colours/favourites") {
        const int limit = key == "colours/recent" ? 16 : 32;
        if (!value.isArray() || value.toArray().size() > limit)
            return false;
        static const QRegularExpression colour("^#(?:[0-9a-fA-F]{6}|[0-9a-fA-F]{8})$");
        for (const auto &entry : value.toArray())
            if (!entry.isString() || !colour.match(entry.toString()).hasMatch())
                return false;
        return true;
    }
    if (key == "ui/module_order") {
        if (!value.isArray() || value.toArray().size() > 64)
            return false;
        for (const auto &entry : value.toArray())
            if (!string(entry, 64))
                return false;
        return true;
    }
    if (key == "selected" || key == "favourites" || key == "collapsed") {
        if (!value.isArray() || value.toArray().size() > 10000)
            return false;
        for (auto entry : value.toArray())
            if (!string(entry))
                return false;
        return true;
    }
    if (key.startsWith("publishing/") && key.size() > 11 && key.size() <= 522) {
        const auto policy = value.toObject();
        return value.isObject() && policy.size() == 3 && number(policy["scope"], 0, 3) &&
               policy["high"].isBool() && number(policy["decimals"], -1, 6);
    }
    if (key == "mqtt/options") {
        if (!value.isObject())
            return false;
        const auto object = value.toObject();
        if (object.size() != 5)
            return false;
        const auto host = object["host"].toString();
        return string(object["host"], 253) && !host.isEmpty() &&
               !host.contains(QRegularExpression("[\\s/@?#]")) && number(object["port"], 1, 65535) &&
               object["tls"].isBool() && string(object["username"], 1024) &&
               (object["scope"] == "local" || (object["scope"] == "cloud" && object["tls"].toBool()));
    }
    if (key == "tray/sensors") {
        if (!value.isArray() || value.toArray().size() > 64)
            return false;
        for (auto entry : value.toArray()) {
            if (!entry.isObject())
                return false;
            const auto row = entry.toObject();
            const bool hasSource = row.contains("source");
            if (row.size() != (hasSource ? 6 : 5) || (hasSource && !string(row["source"], 64)) ||
                !string(row["id"]) || row["id"].toString().isEmpty() ||
                !string(row["name"], 255) || !string(row["foreground"], 32) ||
                !string(row["background"], 32) || !string(row["font"], 512))
                return false;
        }
        return true;
    }
    return false;
}

QByteArray scalar(const QJsonValue &value) {
    const auto array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}
} // namespace

bool permission(const QString &key) {
    return key.startsWith("controls/") || key.startsWith("sources/") || key.startsWith("modules/") ||
           key.startsWith("publishing/") || key == "selected" || key == "remote/enabled" ||
           key == "service_enabled" || key == "connections/installations";
}

QJsonObject snapshot(QSettings &settings) {
    settings.sync();
    QJsonObject values;
    for (const auto &key : settings.allKeys()) {
        // Allowlist before reading: never serialize identities, tokens, private keys or sensor caches.
        if (!booleanKeys.contains(key) && !ranges.contains(key) && key != "license" &&
            key != "modules/haweb/url" && key != "modules/haweb/mode" && key != "modules/haweb/hotkey" && key != "modules/haweb/options" &&
            key != "ui/window_mode" && key != "ui/theme" && key != "ui/transition_style" && key != "ui/module_order" && key != "browser/source" &&
            key != "remote/url" && key != "selected" && key != "favourites" && key != "colours/recent" &&
            key != "colours/favourites" && key != "mqtt/options" && key != "tray/sensors" &&
            key != "connections/installations" && !key.startsWith("publishing/"))
            continue;
        auto value = QJsonValue::fromVariant(settings.value(key));
        if (booleanKeys.contains(key))
            value = settings.value(key).toBool();
        if (ranges.contains(key))
            value = settings.value(key).toDouble();
        if (key == "selected" || key == "favourites" || key == "colours/recent" ||
            key == "colours/favourites" || key == "ui/module_order")
            value = QJsonArray::fromStringList(settings.value(key).toStringList());
        if (key == "tray/sensors")
            value = QJsonDocument::fromJson(settings.value(key).toByteArray()).array();
        if (key == "connections/installations") {
            QJsonArray entries;
            for (const auto &entry : value.toArray())
                entries.append(InstallationConfig::sanitise(entry.toObject()));
            value = entries;
        }
        if (key == "mqtt/options") {
            const auto original = value.toObject();
            QJsonObject safe;
            for (const auto field : {"host", "port", "tls", "scope", "username"})
                safe[field] = original[field];
            value = safe;
        }
        if (valid(key, value))
            values[key] = value;
    }
    return values;
}

QByteArray encode(const QJsonObject &values) {
    QByteArray data =
        "# WHAT configuration - YAML 1.2 (JSON flow values)\n# No passwords, pairing identity or private "
        "keys. Review before sharing.\nformat: \"WHAT configuration\"\nversion: 1\nsettings:\n";
    for (auto it = values.begin(); it != values.end(); ++it)
        data += "  " + scalar(it.key()) + ": " + scalar(it.value()) + "\n";
    return data;
}

Result decode(const QByteArray &data) {
    Result result;
    auto fail = [&](const QString &message) { return Result{{}, message}; };
    if (data.size() > maximumBytes)
        return fail("Configuration is larger than 4 MiB.");
    bool format = false, version = false, settings = false;
    const QRegularExpression entry(R"yaml(^  ("(?:[^"\\]|\\.)*"):\s*(.+)$)yaml");
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString text = utf8(data);
    if (utf8.hasError())
        return fail("Configuration must use valid UTF-8 text.");
    int lineNumber = 0;
    for (auto line : text.split('\n')) {
        ++lineNumber;
        if (line.endsWith('\r'))
            line.chop(1);
        if (line.trimmed().isEmpty() || line.trimmed().startsWith('#'))
            continue;
        if (!format && line == "format: \"WHAT configuration\"") {
            format = true;
            continue;
        }
        if (format && !version && line == "version: 1") {
            version = true;
            continue;
        }
        if (version && !settings && line == "settings:") {
            settings = true;
            continue;
        }
        const auto match = entry.match(line);
        if (!settings || !match.hasMatch())
            return fail(QString("Unsupported YAML or schema at line %1. Use a WHAT-exported configuration.")
                            .arg(lineNumber));
        QJsonParseError error;
        const auto keyArray =
            QJsonDocument::fromJson(("[" + match.captured(1) + "]").toUtf8(), &error).array();
        if (error.error != QJsonParseError::NoError || keyArray.size() != 1 || !keyArray[0].isString())
            return fail("Invalid setting name.");
        const auto key = keyArray[0].toString();
        const auto valueArray =
            QJsonDocument::fromJson(("[" + match.captured(2) + "]").toUtf8(), &error).array();
        // Discard the retired UI preference without rejecting an otherwise valid backup.
        if (key == "hide_connection" && error.error == QJsonParseError::NoError &&
            valueArray.size() == 1 && valueArray[0].isBool())
            continue;
        if (error.error != QJsonParseError::NoError || valueArray.size() != 1 || !valid(key, valueArray[0]) ||
            result.values.contains(key))
            return fail(QString("Invalid, duplicate or unsupported setting at line %1: %2")
                            .arg(lineNumber)
                            .arg(key.left(100)));
        result.values[key] = valueArray[0];
        if (result.values.size() > 11000)
            return fail("Too many settings.");
    }
    if (!settings || result.values.isEmpty())
        return fail("No supported configuration settings found.");
    return result;
}

Result read(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {{}, file.errorString()};
    const auto data = file.read(maximumBytes + 1);
    auto result = decode(data);
    if (result.error.isEmpty()) {
        result.preview = QString::fromUtf8(data.left(64 * 1024));
        if (data.size() > 64 * 1024)
            result.preview += "\n\n[Preview limited to 64 KiB. Review the full YAML file in a text editor "
                              "before importing.]";
    }
    return result;
}

QString write(const QString &path, const QByteArray &data) {
    if (data.size() > maximumBytes)
        return "Configuration is larger than 4 MiB.";
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        return file.errorString();
    return {};
}

QString applyPending(QSettings &settings) {
    const auto pending = settings.fileName() + ".import.yaml";
    if (!QFile::exists(pending))
        return {};
    const auto result = read(pending);
    if (!result.error.isEmpty())
        return "Configuration import rejected: " + result.error;
    // Prepare a complete merged YAML separately, then replace atomically. Keep a private local rollback copy.
    settings.sync();
    if (settings.status() != QSettings::NoError)
        return "Could not read the current configuration.";
    QTemporaryDir temporary(QFileInfo(settings.fileName()).absolutePath() + "/config-import-XXXXXX");
    if (!temporary.isValid())
        return temporary.errorString();
    const auto temporaryPath = temporary.filePath("settings.yaml");
    {
        QSettings merged(temporaryPath, settings.format());
        for (const auto &key : settings.allKeys())
            merged.setValue(key, settings.value(key));
        for (auto it = result.values.begin(); it != result.values.end(); ++it) {
            if (it.key() == "collapsed" || it.key() == "browser/query")
                continue;
            if (it.key() == "tray/sensors")
                merged.setValue(it.key(), QJsonDocument(it.value().toArray()).toJson(QJsonDocument::Compact));
            else if (it.key() == "selected" || it.key() == "favourites") {
                QStringList ids;
                for (auto id : it.value().toArray())
                    ids << id.toString();
                merged.setValue(it.key(), ids);
            } else
                merged.setValue(it.key(), it.value().toVariant());
        }
        if (result.values.contains("connections/installations")) {
            auto entries = merged.value("connections/installations").toList();
            for (auto &entry : entries) {
                auto row = entry.toMap();
                row["enabled"] = false;
                entry = row;
            }
            merged.setValue("connections/installations", entries);
            // Credentials never travel in an export. Keep only this PC's existing
            // credentials for matching IDs; new entries receive tokens at startup.
        }
        if (result.values.contains("selected")) {
            merged.setValue("selection_initialized", true);
            merged.setValue("windows_selection_initialized", true);
        }
        if (result.values.contains("remote/enabled"))
            merged.setValue("remote/enabled", false);
        if (result.values.contains("remote/url")) {
            QUrl url(result.values["remote/url"].toString());
            url.setScheme("wss");
            url.setPath("/api/what/connect");
            merged.setValue("remote/endpoint", url.toString());
            // New destinations must be explicitly enabled after review on this PC.
            merged.setValue("remote/enabled", false);
        }
        if (result.values.contains("mqtt/options")) {
            merged.remove("mqtt/password");
            merged.setValue("mqtt/preferred", false);
            merged.setValue("mqtt/paused", true);
        }
        if (merged.value("publishingHighMs", 100).toInt() > merged.value("publishingNormalMs", 1000).toInt())
            merged.setValue("publishingHighMs", merged.value("publishingNormalMs", 1000));
        merged.sync();
        if (merged.status() != QSettings::NoError)
            return "Could not prepare imported settings.";
    }
    QFile original(settings.fileName());
    if (original.exists()) {
        if (!original.open(QIODevice::ReadOnly))
            return original.errorString();
        QSaveFile backup(settings.fileName() + ".before-import");
        const auto currentBytes = original.readAll();
        if (!backup.open(QIODevice::WriteOnly) || backup.write(currentBytes) != currentBytes.size() ||
            !backup.commit())
            return "Could not back up current settings: " + backup.errorString();
        original.close();
    }
    QFile replacement(temporaryPath);
    if (!replacement.open(QIODevice::ReadOnly))
        return replacement.errorString();
    // The local profile may contain caches larger than the portable export limit.
    QSaveFile destination(settings.fileName());
    const auto bytes = replacement.readAll();
    if (!destination.open(QIODevice::WriteOnly) || destination.write(bytes) != bytes.size() ||
        !destination.commit())
        return destination.errorString();
    settings.sync();
    if (!QFile::remove(pending))
        return "Configuration imported, but its pending file could not be removed.";
    return "Configuration imported. Connection credentials may need re-entering.";
}
} // namespace ConfigurationFile
