#include "SettingsStore.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QSaveFile>
#include <QSet>
#include <QCache>
#include <QStringDecoder>
#include <yaml-cpp/yaml.h>
#include <cmath>
#include <stdexcept>

namespace {
constexpr qint64 maximumBytes = 64 * 1024 * 1024;
QMutex errorMutex;
QSet<QString> unreadableFiles;

struct EncodedSetting {
    QVariant value;
    QByteArray yaml;
};

QMutex encodingMutex;
QCache<QString, QHash<QString, EncodedSetting>> encodedProfiles(4);

bool sameSetting(const QVariant &left, const QVariant &right) {
    // QVariant equality permits conversions, but YAML must retain scalar types.
    if (left.metaType() != right.metaType())
        return false;
    if (left.typeId() == QMetaType::QVariantMap) {
        const auto a = left.toMap(), b = right.toMap();
        if (a.isSharedWith(b))
            return true;
        if (a.size() != b.size())
            return false;
        for (auto i = a.cbegin(), j = b.cbegin(); i != a.cend(); ++i, ++j)
            if (i.key() != j.key() || !sameSetting(i.value(), j.value()))
                return false;
        return true;
    }
    if (left.typeId() == QMetaType::QVariantList) {
        const auto a = left.toList(), b = right.toList();
        if (a.size() != b.size())
            return false;
        for (qsizetype i = 0; i < a.size(); ++i)
            if (!sameSetting(a[i], b[i]))
                return false;
        return true;
    }
    return left == right;
}

QString devicePath(QIODevice &device) {
    auto *file = qobject_cast<QFileDevice *>(&device);
    return file ? QFileInfo(file->fileName()).absoluteFilePath() : QString();
}

QVariant valueFromYaml(const YAML::Node &node, int depth, int &remaining) {
    if (depth > 32 || --remaining < 0)
        throw std::runtime_error("Settings contain too many values or too much nesting");
    const auto tag = node.Tag();
    if (tag == "tag:yaml.org,2002:binary") {
        const auto text = QByteArray::fromStdString(node.as<std::string>());
        const auto decoded = QByteArray::fromBase64Encoding(text, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded)
            throw std::runtime_error("Invalid binary setting");
        return decoded.decoded;
    }
    if (!tag.empty() && tag != "?" && tag != "!" && tag != "tag:yaml.org,2002:str")
        throw std::runtime_error("Unsupported YAML tag");
    if (node.IsNull())
        return {};
    if (node.IsSequence()) {
        QVariantList values;
        for (const auto &child : node)
            values.append(valueFromYaml(child, depth + 1, remaining));
        return values;
    }
    if (node.IsMap()) {
        QVariantMap values;
        for (const auto &entry : node) {
            const auto key = QString::fromStdString(entry.first.as<std::string>());
            if (values.contains(key))
                throw std::runtime_error("Duplicate YAML setting");
            values.insert(key, valueFromYaml(entry.second, depth + 1, remaining));
        }
        return values;
    }
    const auto text = QString::fromStdString(node.as<std::string>());
    // Quoted strings must stay strings, including names such as "false" or "123".
    if (tag == "!" || tag == "tag:yaml.org,2002:str")
        return text;
    if (text == "true" || text == "false")
        return text == "true";
    bool ok = false;
    const auto integer = text.toLongLong(&ok);
    if (ok)
        return integer;
    const auto unsignedInteger = text.toULongLong(&ok);
    if (ok)
        return unsignedInteger;
    const auto number = text.toDouble(&ok);
    if (ok && std::isfinite(number))
        return number;
    return text;
}

QByteArray quoteString(const QString &text) {
    // YAML treats these characters as non-printable or line separators, while
    // JSON may leave them literal. Keep YAML's escaping for those uncommon values.
    for (const auto character : text) {
        const auto value = character.unicode();
        if ((value >= 0x7f && value <= 0x9f) || value == 0x2028 || value == 0x2029) {
            YAML::Emitter quoted;
            quoted << YAML::DoubleQuoted << text.toStdString();
            if (!quoted.good())
                throw std::runtime_error("Could not encode setting string");
            return QByteArray(quoted.c_str(), quoted.size());
        }
    }
    // Qt's encoder avoids constructing an emitter for every ordinary key and value.
    const auto array = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}

void appendValue(QByteArray &out, const QVariant &value, int indent) {
    const QByteArray padding(indent, ' ');
    switch (value.typeId()) {
    case QMetaType::QVariantMap: {
        const auto values = value.toMap();
        if (values.isEmpty()) {
            out += " {}\n";
            break;
        }
        out += '\n';
        for (auto it = values.begin(); it != values.end(); ++it) {
            out += padding + quoteString(it.key()) + ':';
            appendValue(out, it.value(), indent + 2);
        }
        break;
    }
    case QMetaType::QStringList:
    case QMetaType::QVariantList: {
        const auto values = value.toList();
        if (values.isEmpty()) {
            out += " []\n";
            break;
        }
        out += '\n';
        for (const auto &item : values) {
            out += padding + '-';
            appendValue(out, item, indent + 2);
        }
        break;
    }
    case QMetaType::UnknownType:
        out += " null\n";
        break;
    case QMetaType::Bool:
        out += value.toBool() ? " true\n" : " false\n";
        break;
    case QMetaType::Int:
    case QMetaType::LongLong:
        out += ' ' + QByteArray::number(value.toLongLong()) + '\n';
        break;
    case QMetaType::UInt:
    case QMetaType::ULongLong:
        out += ' ' + QByteArray::number(value.toULongLong()) + '\n';
        break;
    case QMetaType::Double:
    case QMetaType::Float:
        if (!std::isfinite(value.toDouble()))
            throw std::runtime_error("Non-finite setting value");
        out += ' ' + QByteArray::number(value.toDouble(), 'g', 17) + '\n';
        break;
    case QMetaType::QString:
        out += ' ' + quoteString(value.toString()) + '\n';
        break;
    case QMetaType::QByteArray:
        // Sensor catalogs can be megabytes. Append base64 in one operation
        // rather than sending every character through yaml-cpp's Debug emitter.
        out += " !!binary \"" + value.toByteArray().toBase64() + "\"\n";
        break;
    default:
        throw std::runtime_error("Unsupported settings value type");
    }
}

bool readSettings(QIODevice &device, QSettings::SettingsMap &settings) {
    const auto path = devicePath(device);
    try {
        const auto bytes = device.read(maximumBytes + 1);
        QStringDecoder utf8(QStringDecoder::Utf8);
        const QString text = utf8(bytes);
        if (bytes.size() > maximumBytes || utf8.hasError())
            throw std::runtime_error("Settings must be UTF-8 and at most 64 MiB");
        const auto documents = YAML::LoadAll(text.toStdString());
        if (documents.size() != 1)
            throw std::runtime_error("Expected one settings document");
        int remaining = 200000;
        const auto root = valueFromYaml(documents.front(), 0, remaining).toMap();
        if (root.size() != 3 || root.value("format") != "WHAT settings" || root.value("version") != 1 ||
            root.value("settings").typeId() != QMetaType::QVariantMap)
            throw std::runtime_error("Unsupported settings document");
        const auto values = root.value("settings").toMap();
        for (auto it = values.begin(); it != values.end(); ++it) {
            if (it.key().isEmpty() || it.key().startsWith('/') || it.key().endsWith('/') ||
                it.key().contains("//") || it.key().contains(QChar(0)))
                throw std::runtime_error("Invalid setting name");
        }
        settings = values;
        QMutexLocker lock(&errorMutex);
        unreadableFiles.remove(path);
        return true;
    } catch (const std::exception &) {
        // Qt can call its writer after a failed read. Never replace that file with defaults.
        QMutexLocker lock(&errorMutex);
        unreadableFiles.insert(path);
        return false;
    }
}

bool writeSettings(QIODevice &device, const QSettings::SettingsMap &settings) {
    {
        QMutexLocker lock(&errorMutex);
        if (unreadableFiles.contains(devicePath(device)))
            return false;
    }
    try {
        QByteArray bytes = "# WHAT local settings. Contains private identity and protected credentials.\n"
                           "format: WHAT settings\nversion: 1\nsettings:";
        // QSettings writes the whole document, even for a single changed key.
        // Retain encoded entries so a click does not serialize every sensor policy.
        QMutexLocker lock(&encodingMutex);
        const auto path = devicePath(device);
        auto *entries = encodedProfiles.object(path);
        if (!entries) {
            entries = new QHash<QString, EncodedSetting>;
            encodedProfiles.insert(path, entries);
        }
        bytes += settings.isEmpty() ? " {}\n" : "\n";
        for (auto it = settings.cbegin(); it != settings.cend(); ++it) {
            auto cached = entries->find(it.key());
            if (cached == entries->end() || !sameSetting(cached->value, it.value())) {
                QByteArray yaml = "  " + quoteString(it.key()) + ':';
                appendValue(yaml, it.value(), 4);
                cached = entries->insert(it.key(), {it.value(), std::move(yaml)});
            }
            bytes += cached->yaml;
        }
        for (auto it = entries->begin(); it != entries->end();) {
            if (!settings.contains(it.key()))
                it = entries->erase(it);
            else
                ++it;
        }
        lock.unlock();
        if (bytes.size() > maximumBytes)
            return false;
        return device.write(bytes) == bytes.size();
    } catch (const std::exception &) {
        return false;
    }
}

void fail(const QString &message, const QString &path) {
    throw std::runtime_error((message + "\n" + QDir::toNativeSeparators(path)).toStdString());
}
} // namespace

QSettings::Format SettingsStore::format() {
    static const auto yaml =
        QSettings::registerFormat("yaml", readSettings, writeSettings, Qt::CaseSensitive);
    return yaml;
}

QString SettingsStore::profilePath(const QString &directory, bool testing) {
    const auto base = QDir(directory).filePath(testing ? "test-settings" : "settings");
    const auto path = base + ".yaml";
    const auto legacy = base + ".ini";
    const auto backup = legacy + ".migrated";
    if (!QFile::exists(path)) {
        if (QFile::exists(backup))
            fail("Settings YAML is missing. Restore settings.yaml from your backup; INI is no longer used.",
                 path);
        QSettings::SettingsMap values;
        if (QFile::exists(legacy)) {
            QSettings previous(legacy, QSettings::IniFormat);
            for (const auto &key : previous.allKeys())
                values.insert(key, previous.value(key));
            if (previous.status() != QSettings::NoError)
                fail("Could not read the settings to migrate. The original file has been kept.", legacy);
        }
        if (!QDir().mkpath(directory))
            fail("Could not create the settings directory.", directory);
        QSaveFile destination(path);
        if (!destination.open(QIODevice::WriteOnly) || !writeSettings(destination, values) ||
            !destination.commit())
            fail("Could not save settings.yaml. Existing settings have been kept.", path);
    }
    // Validate through QSettings so the controller and updater reuse its parsed
    // file cache. Parsing the catalog separately here doubles startup work.
    QSettings validation(path, format());
    bool invalid = validation.status() != QSettings::NoError;
    {
        QMutexLocker lock(&errorMutex);
        // Qt treats a zero-byte settings file as an empty profile without calling
        // the reader. YAML profiles must contain the document header.
        if (QFileInfo(path).size() == 0)
            unreadableFiles.insert(QFileInfo(path).absoluteFilePath());
        invalid = invalid || unreadableFiles.contains(QFileInfo(path).absoluteFilePath());
    }
    if (invalid)
        fail(
            "Could not read settings.yaml. Check its YAML format or restore a YAML backup. The file has not been replaced.",
            path);
    // Archive only after the YAML has been committed and read back successfully.
    if (QFile::exists(legacy) && !QFile::exists(backup) && !QFile::rename(legacy, backup))
        fail(
            "Settings were converted, but the old INI could not be archived. Close other copies of WHAT and retry.",
            legacy);
    const auto pending = legacy + ".import.yaml";
    if (QFile::exists(pending) && !QFile::rename(pending, path + ".import.yaml"))
        fail("Could not move the queued configuration import to the YAML profile.", pending);
    return path;
}
