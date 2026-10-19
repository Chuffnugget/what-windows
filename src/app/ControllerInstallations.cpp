#include "Controller.h"
#include "InstallationConfig.h"
#include <QApplication>
#include <QClipboard>
#include <QHostInfo>
#include <QRandomGenerator>
#include <QUuid>
#include <QColor>
#include <algorithm>
#include <cmath>

namespace {
QString credentialKey(const QString &id) {
    return "connections/credentials/" + id;
}

QString newPairingToken() {
    QByteArray bytes(32, Qt::Uninitialized);
    for (int i = 0; i < bytes.size(); i += 4) {
        const auto value = QRandomGenerator::system()->generate();
        memcpy(bytes.data() + i, &value, 4);
    }
    return QString::fromLatin1(bytes.toHex());
}
} // namespace

void Controller::initialiseInstallations() {
    if (!settings.contains("connections/installations")) {
        auto mqtt = settings.value("mqtt/options").toMap();
        for (const auto *key : {"username", "ca", "certificate", "key"})
            if (!mqtt.contains(key))
                mqtt[key] = QString();
        if (!mqtt.contains("tls"))
            mqtt["tls"] = true;
        if (!mqtt.contains("port"))
            mqtt["port"] = 8883;
        if (!mqtt.contains("scope"))
            mqtt["scope"] = "cloud";
        const bool useMqtt = mqtt.value("enabled").toBool() && settings.value("mqtt/preferred").toBool();
        const auto id = QUuid::createUuid().toString(QUuid::Id128);
        QVariantMap definition{{"id", id},
                               {"name", "Home Assistant"},
                               {"type", useMqtt ? "mqtt" : "api"},
                               {"enabled", !useMqtt || !settings.value("mqtt/paused").toBool()},
                               {"controls", true},
                               {"url", settings.value("remote/enabled").toBool()
                                           ? settings.value("remote/url").toString()
                                           : QString()},
                               {"mqtt", mqtt}};
        QVariantMap credentials{{"token", token.isEmpty()
            ? QByteArray::fromBase64(settings.value("token").toByteArray())
            : protectSecret(token.toLatin1())}};
        if (settings.contains("mqtt/password"))
            credentials["password"] = settings.value("mqtt/password");
        settings.setValue(credentialKey(id), credentials);
        QVariantList entries{definition};
        // Keep an unused, configured route as a disabled entry for review. It gets
        // a separate token, so enabling it cannot impersonate the existing pairing.
        const bool otherConfigured = useMqtt ? !settings.value("remote/url").toString().isEmpty()
                                             : !mqtt.value("host").toString().isEmpty();
        if (otherConfigured) {
            const auto otherId = QUuid::createUuid().toString(QUuid::Id128);
            definition["id"] = otherId;
            definition["name"] = useMqtt ? "Previous Home Assistant API" : "Previous MQTT Broker";
            definition["type"] = useMqtt ? "api" : "mqtt";
            definition["url"] = settings.value("remote/url").toString();
            definition["enabled"] = false;
            credentials["token"] = protectSecret(newPairingToken().toLatin1());
            settings.setValue(credentialKey(otherId), credentials);
            entries.append(definition);
        }
        settings.setValue("connections/installations", entries);
        scheduleSettingsFlush();
    }
    for (const auto &value : settings.value("connections/installations").toList()) {
        const auto row = QJsonObject::fromVariantMap(value.toMap());
        if (InstallationConfig::validate(row).isEmpty() &&
            !settings.contains(credentialKey(row["id"].toString()))) {
            settings.setValue(credentialKey(row["id"].toString()),
                              QVariantMap{{"token", protectSecret(newPairingToken().toLatin1())}});
            scheduleSettingsFlush();
        }
    }
    connect(&server, &NetworkService::installationState, this, [this](const QString &id, QVariantMap state) {
        bool present = false;
        for (const auto &value : settings.value("connections/installations").toList())
            present |= value.toMap().value("id").toString() == id;
        if (!present || installationStates.value(id) == state)
            return;
        auto previous = installationStates.value(id);
        installationStates[id] = state;
        emit installationsChanged();
        previous.remove("lastCommunication");
        state.remove("lastCommunication");
        // Reply timestamps have their own notifier; do not rebuild every UI status
        // binding on each heartbeat. Connection changes still refresh the overview.
        if (previous != state)
            emit changed();
    });
    connect(
        &server, &NetworkService::installationTransportFailed, this,
        [this](const QString &id, const QString &requested, const QString &retained, const QString &message) {
            for (const auto &value : settings.value("connections/installations").toList()) {
                auto row = value.toMap();
                if (row["id"] == id && row["type"] == requested) {
                    row["type"] = retained;
                    saveInstallation(row);
                    log(message);
                    break;
                }
            }
        });
    applyInstallations();
}

QVariantList Controller::installations() const {
    QVariantList result;
    for (const auto &value : settings.value("connections/installations").toList()) {
        auto row = value.toMap();
        const auto id = row.value("id").toString();
        const auto state = installationStates.value(id);
        row["hasPassword"] = settings.value(credentialKey(id)).toMap().contains("password");
        row["state"] = state;
        row["connected"] = settings.value("service_enabled", true).toBool() &&
                           row.value("enabled").toBool() && state.value("clients").toInt() > 0;
        row["status"] = !row.value("enabled").toBool()                      ? "Disabled"
                        : !settings.value("service_enabled", true).toBool() ? "Sharing stopped"
                        : row.value("connected").toBool()
                            ? "Connected"
                            : state.value("status", "Waiting for Home Assistant").toString();
        result.append(row);
    }
    return result;
}

QVariantList Controller::installationConfiguration() const {
    auto entries = installations();
    for (auto &value : entries) {
        auto row = value.toMap();
        for (const auto *key : {"state", "connected", "status"})
            row.remove(key);
        row["colour"] =
            settings.value("ui/installationColours").toMap().value(row.value("id").toString(), "#59c6f5");
        value = row;
    }
    return entries;
}

QVariantMap Controller::installationStatus() const {
    QVariantMap result;
    for (const auto &value : installations()) {
        const auto row = value.toMap();
        auto state = row.value("state").toMap();
        state["status"] = row.value("status");
        state["connected"] = row.value("connected");
        result[row.value("id").toString()] = state;
    }
    return result;
}

void Controller::ensureInstallationColours() {
    const auto saved = settings.value("ui/installationColours").toMap();
    QVariantMap colours;
    QSet<QString> used;
    const auto entries = settings.value("connections/installations").toList();
    for (const auto &value : entries) {
        const auto id = value.toMap().value("id").toString();
        const QColor colour(saved.value(id).toString());
        if (colour.isValid() && !used.contains(colour.name())) {
            colours[id] = colour.name();
            used.insert(colour.name());
        }
    }
    const QStringList palette{"#59c6f5", "#f5b45c", "#bd8ce8", "#76d4a0",
                              "#f28c9a", "#74d8d5", "#ddd077", "#9fa8ff"};
    int next = 0;
    for (const auto &value : entries) {
        const auto id = value.toMap().value("id").toString();
        if (colours.contains(id))
            continue;
        QString colour;
        do {
            colour = next < palette.size()
                         ? palette[next]
                         : QColor::fromHslF(std::fmod(next * 0.61803398875, 1.0),
                                            0.50 + (next / 360 % 10) * 0.04, 0.60 + (next / 3600 % 6) * 0.035)
                               .name();
            ++next;
        } while (used.contains(colour));
        colours[id] = colour;
        used.insert(colour);
    }
    // Keep each colour through renames, deletions of other entries and restarts.
    if (colours != saved) {
        settings.setValue("ui/installationColours", colours);
        scheduleSettingsFlush();
    }
}

void Controller::applyInstallations() {
    ensureInstallationColours();
    QJsonArray definitions;
    for (const auto &value : settings.value("connections/installations").toList()) {
        auto definition = QJsonObject::fromVariantMap(value.toMap());
        const auto id = definition["id"].toString();
        const auto validation = InstallationConfig::validate(definition);
        try {
            if (!validation.isEmpty())
                throw std::runtime_error("Invalid installation configuration");
            const auto credentials = settings.value(credentialKey(id)).toMap();
            definition["token"] =
                QString::fromLatin1(unprotectSecret(credentials.value("token").toByteArray()));
            auto mqtt = definition["mqtt"].toObject();
            if (credentials.contains("password"))
                mqtt["password"] = QString::fromUtf8(unprotectSecret(credentials["password"].toByteArray()));
            definition["mqtt"] = mqtt;
        } catch (const std::exception &) {
            definition["enabled"] = false;
            const auto error =
                validation.isEmpty()
                    ? "Pairing credentials unavailable. Replace this installation's token in Pair This PC, "
                      "re-enter its MQTT password if used, then reconfigure its existing Home Assistant entry."
                    : validation;
            definition["configurationError"] = error;
            installationStates[id] = {{"status", error}};
        }
        QUrl endpoint(definition["url"].toString());
        if (!endpoint.isEmpty()) {
            endpoint.setScheme("wss");
            endpoint.setPath("/api/what/connect");
        }
        definition["endpoint"] = testMode ? QString() : endpoint.toString();
        definition["addresses"] = QJsonArray::fromStringList(cachedAddresses);
        definitions.append(definition);
    }
    server.setInstallations(definitions);
    emit installationConfigurationChanged();
    emit installationsChanged();
    emit changed();
}

bool Controller::saveInstallation(const QVariantMap &input, const QString &password, bool clearPassword) {
    auto definition = InstallationConfig::sanitise(QJsonObject::fromVariantMap(input));
    QString id = definition["id"].toString();
    auto values = settings.value("connections/installations").toList();
    int index = -1;
    for (int i = 0; i < values.size(); ++i)
        if (values[i].toMap().value("id").toString() == id)
            index = i;
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::Id128);
        definition["id"] = id;
    } else if (index < 0) {
        installationFailure = "This installation was removed. Add a new installation instead.";
        emit installationsChanged();
        return false;
    }
    definition["name"] = definition["name"].toString().trimmed();
    definition["url"] = definition["url"].toString().trimmed();
    installationFailure = InstallationConfig::validate(definition);
    if (password.size() > 4096)
        installationFailure = "The MQTT password is too long.";
    if (!installationFailure.isEmpty()) {
        emit installationsChanged();
        return false;
    }
    auto credentials = settings.value(credentialKey(id)).toMap();
    try {
        if (!credentials.contains("token"))
            credentials["token"] = protectSecret(newPairingToken().toLatin1());
        else
            unprotectSecret(credentials["token"].toByteArray());
        if (!password.isEmpty())
            credentials["password"] = protectSecret(password.toUtf8());
        else if (clearPassword)
            credentials.remove("password");
    } catch (const std::exception &) {
        installationFailure = "Windows could not protect the installation credentials. Nothing was saved.";
        emit installationsChanged();
        return false;
    }
    if (index < 0)
        values.append(definition.toVariantMap());
    else
        values[index] = definition.toVariantMap();
    settings.setValue(credentialKey(id), credentials);
    settings.setValue("connections/installations", values);
    scheduleSettingsFlush();
    applyInstallations();
    log("Home Assistant installation saved: " + definition["name"].toString());
    emit installationSaved(id);
    return true;
}

bool Controller::setInstallationEnabled(const QString &id, bool enabled) {
    for (const auto &value : settings.value("connections/installations").toList()) {
        auto row = value.toMap();
        if (row.value("id").toString() == id) {
            row["enabled"] = enabled;
            return saveInstallation(row);
        }
    }
    return false;
}

bool Controller::deleteInstallation(const QString &id) {
    auto values = settings.value("connections/installations").toList();
    const auto count = values.size();
    values.removeIf([&](const QVariant &value) { return value.toMap().value("id").toString() == id; });
    if (values.size() == count)
        return false;
    settings.setValue("connections/installations", values);
    settings.remove(credentialKey(id));
    installationStates.remove(id);
    installationFailure.clear();
    scheduleSettingsFlush();
    applyInstallations();
    log("Home Assistant installation removed; its pairing access was revoked");
    return true;
}

void Controller::copyInstallationToken(const QString &id) {
    const auto credentials = settings.value(credentialKey(id)).toMap();
    try {
        QApplication::clipboard()->setText(
            QString::fromLatin1(unprotectSecret(credentials.value("token").toByteArray())));
    } catch (const std::exception &) {
        installationFailure = "The pairing token could not be read.";
        emit installationsChanged();
    }
}

void Controller::copyInstallationPairing(const QString &id) {
    for (const auto &value : settings.value("connections/installations").toList()) {
        const auto row = value.toMap();
        if (row.value("id").toString() != id)
            continue;
        try {
            const auto credentials = settings.value(credentialKey(id)).toMap();
            const auto secret =
                QString::fromLatin1(unprotectSecret(credentials.value("token").toByteArray()));
            QApplication::clipboard()->setText(
                QString(
                    "WHAT: %1\nInstallation: %2\nIP: %3\nPort: %4\nToken: %5\nSHA-256 fingerprint: %6\nDevice ID: %7")
                    .arg(QHostInfo::localHostName(), row.value("name").toString(),
                         info()["addresses"].toString())
                    .arg(activePort)
                    .arg(secret, fingerprint, deviceId));
        } catch (const std::exception &) {
            installationFailure = "The pairing details could not be read.";
            emit installationsChanged();
        }
        return;
    }
}

bool Controller::rotateInstallationToken(const QString &id) {
    const auto values = settings.value("connections/installations").toList();
    const bool exists = std::any_of(values.cbegin(), values.cend(), [&](const QVariant &value) {
        return value.toMap().value("id").toString() == id;
    });
    if (!exists) {
        installationFailure = "This installation no longer exists. No tokens were changed.";
        emit installationsChanged();
        return false;
    }

    // Replace only this secret. Keep the installation ID and broker password,
    // and leave every other installation's pairing untouched.
    const auto key = credentialKey(id);
    auto credentials = settings.value(key).toMap();
    try {
        credentials["token"] = protectSecret(newPairingToken().toLatin1());
    } catch (const std::exception &) {
        installationFailure = "Windows could not protect the new pairing token. Nothing was changed.";
        emit installationsChanged();
        return false;
    }
    settings.setValue(key, credentials);
    installationFailure.clear();
    scheduleSettingsFlush();
    applyInstallations();
    log("Installation pairing token replaced. Reconfigure its existing Home Assistant entry.");
    return true;
}

void Controller::rotateToken() {
    QVariantMap replacements;
    try {
        for (const auto &value : settings.value("connections/installations").toList()) {
            const auto key = credentialKey(value.toMap().value("id").toString());
            auto credentials = settings.value(key).toMap();
            credentials["token"] = protectSecret(newPairingToken().toLatin1());
            replacements[key] = credentials;
        }
    } catch (const std::exception &) {
        log("Windows could not protect the replacement pairing tokens. Nothing was changed.");
        return;
    }
    for (auto it = replacements.cbegin(); it != replacements.cend(); ++it)
        settings.setValue(it.key(), it.value());
    scheduleSettingsFlush();
    applyInstallations();
    log("All installation pairing tokens changed. Reconfigure each Home Assistant connection.");
}
