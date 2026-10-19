#include "app/SettingsStore.h"
#include "SensorTrayManager.h"
#include "TrayUi.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <QFontDialog>
#include <QFontMetricsF>
#include <QPainter>
#include <QBuffer>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QSet>
#include <QCursor>
#include <QTimer>
#include <cmath>

namespace {
QString unit(const QVariantMap &reading) {
    const auto symbol = reading["unit"].toString();
    if (symbol == "C")
        return QString::fromUtf8("°C");
    if (symbol == "F")
        return QString::fromUtf8("°F");
    return symbol;
}

bool usable(const QVariantMap &reading) {
    return reading.value("available", false).toBool() && reading.contains("value") &&
           !reading["value"].isNull();
}

QVariantMap defaults(QString id, QString name) {
    QFont font("Segoe UI", 11, QFont::DemiBold);
    return {{"id", id},
            {"name", name},
            {"foreground", "#ff66caff"},
            {"background", "#00000000"},
            {"font", font.toString()}};
}

QVariantMap mainDefaults() {
    auto row = defaults("main", "WHAT System Tray Icon");
    row["sensor"] = "";
    row["image"] = "";
    return row;
}

void readStyle(QVariantMap &clean, const QVariantMap &row) {
    clean["source"] = row["source"].toString().left(64);
    for (const auto *key : {"foreground", "background"})
        if (QColor(row[key].toString()).isValid())
            clean[key] = row[key];
    QFont font;
    if (!row["font"].toString().isEmpty() && font.fromString(row["font"].toString()))
        clean["font"] = font.toString();
}
} // namespace

SensorTrayManager::SensorTrayManager(const QString &file, bool testing, QObject *parent)
    : QObject(parent), settings(file, SettingsStore::format()), testMode(testing) {
    renderer.ready = [this] { refresh(); };
    pickerJob.onStatus = [this](QString error) {
        if (error.isEmpty() || !pickerActive)
            return;
        lastError = "Could not prepare the sensor list: " + error;
        preparePicker();
        emit valuesChanged();
        emit pickerChanged();
    };
    taskbar = new TaskbarIconManager(file, testing, this);
    connect(taskbar, &TaskbarIconManager::openRequested, this, &SensorTrayManager::openRequested);
    auto primary = mainDefaults();
    const auto stored =
        QJsonDocument::fromJson(settings.value("tray/main").toByteArray()).object().toVariantMap();
    readStyle(primary, stored);
    primary["sensor"] = stored["sensor"].toString().left(512);
    if (stored["image"].toString().size() <= 1024 * 1024)
        primary["image"] = stored.value("image", "");
    configuration << primary;
    QSet<QString> seen{"main"};
    for (auto value : QJsonDocument::fromJson(settings.value("tray/sensors").toByteArray()).array()) {
        auto row = value.toObject().toVariantMap();
        auto id = row["id"].toString();
        if (id.isEmpty() || id.size() > 512 || seen.contains(id) || configuration.size() >= 65)
            continue;
        auto clean = defaults(id, row["name"].toString().left(255));
        readStyle(clean, row);
        configuration << clean;
        seen.insert(id);
    }
    refresh();
}

QString SensorTrayManager::compact(const QVariantMap &reading) {
    if (!usable(reading))
        return QString::fromUtf8("-");
    if (reading["kind"] == "binary")
        return reading["value"].toBool() ? "On" : "Off";
    if (reading["kind"] != "number")
        return reading["value"].toString().left(4);
    bool ok = false;
    const double number = reading["value"].toDouble(&ok);
    if (!ok || !std::isfinite(number))
        return QString::fromUtf8("-");
    const double magnitude = std::abs(number);
    if (magnitude >= 1e12)
        return QString::number(number, 'g', 2);
    for (const auto &suffix :
         {qMakePair(1e9, QString("G")), qMakePair(1e6, QString("M")), qMakePair(1e3, QString("k"))})
        if (magnitude >= suffix.first)
            return QString::number(number / suffix.first, 'f', magnitude / suffix.first < 10 ? 1 : 0) +
                   suffix.second;
    return QString::number(std::round(number) == 0 ? 0 : std::round(number), 'f', 0);
}

QString SensorTrayManager::moduleName(const QString &source) {
    static const QMap<QString, QString> names{{"windows", "Windows"}, {"hwinfo", "HWiNFO"},
        {"icue", "iCUE"}, {"nvidia", "NVIDIA"}, {"beacn", "BEACN"},
        {"coretemp", "Core Temp"}, {"wallpaper", "Wallpaper Engine"}, {"custom", "Custom"}};
    return names.value(source, source);
}

QString SensorTrayManager::tooltip(const QVariantMap &reading) {
    const auto source = moduleName(reading["source"].toString());
    const auto title = (source.isEmpty() ? QString() : source + " / ") +
        (reading["group"].toString().isEmpty() ? QString() : reading["group"].toString() + " / ") +
        reading["name"].toString();
    if (!usable(reading))
        return title + "\nUnavailable";
    QString value;
    if (reading["kind"] == "number") {
        const bool temperature = QStringList{"C", "°C", "F", "°F", "K"}.contains(reading["unit"].toString());
        value = QString::number(reading["value"].toDouble(), temperature ? 'f' : 'g', temperature ? 1 : 15);
    }
    else if (reading["kind"] == "binary")
        value = reading["value"].toBool() ? "On" : "Off";
    else
        value = reading["value"].toString();
    const auto symbol = unit(reading);
    if (!symbol.isEmpty())
        value += " " + symbol;
    // Windows limits tray tips: keep the formatted reading before a long sensor path.
    return value + "\n" + title.left(qMax(0, 126 - int(value.size())));
}

QImage SensorTrayManager::render(const QString &text, const QVariantMap &style, int size) {
    if (style.contains("sensor") && style["sensor"].toString().isEmpty()) {
        QImage image;
        if (!style["image"].toString().isEmpty())
            image.loadFromData(QByteArray::fromBase64(style["image"].toString().toLatin1()), "PNG");
        // QImage reads the ICO's first (small) frame; use the full-resolution source.
        if (image.isNull())
            image.load(":/assets/app-icon.png");
        return image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(style["background"].toString()));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font;
    font.fromString(style["font"].toString());
    font.setPixelSize(qBound(4, int(font.pointSizeF() * size / 16.0), size));
    while (font.pixelSize() > 4 &&
           (QFontMetricsF(font).horizontalAdvance(text) > size - 1 || QFontMetricsF(font).height() > size)) {
        font.setPixelSize(font.pixelSize() - 1);
    }
    painter.setFont(font);
    painter.setPen(QColor(style["foreground"].toString()));
    painter.drawText(image.rect(), Qt::AlignCenter, text);
    return image;
}

void SensorTrayManager::setReadings(const QVariantList &rows, bool complete) {
    taskbar->setReadings(rows);
    readings.clear();
    for (auto value : rows) {
        auto row = value.toMap();
        readings.insert(row["id"].toString(), row);
    }
    // Icon-only batches are incomplete; a search waits for the full browsing catalogue.
    readingsComplete = complete;
    preparePicker();
    refresh();
}

namespace {
QVariantList pickerRows(const QHash<QString, QVariantMap> &readings, const QString &query,
                       const QVariantMap &collapsed) {
    const auto words = query.simplified().toLower().split(' ', Qt::SkipEmptyParts);
    QMap<QString, QMap<QString, QVariantList>> groups;
    for (const auto &row : readings) {
        auto source = row["source"].toString();
        auto group = row["group"].toString();
        if (source.isEmpty())
            source = "Other";
        if (group.isEmpty())
            group = "General";
        const auto label = SensorTrayManager::moduleName(source) + " / " + group + " / " + row["name"].toString();
        const auto searchable = (source + " " + label).toLower();
        if (std::any_of(words.cbegin(), words.cend(),
                        [&](const QString &word) { return !searchable.contains(word); }))
            continue;
        groups[source][group].append(
            QVariantMap{{"id", row["id"]},
                        {"name", row["name"]},
                        {"label", label},
                        {"reading", usable(row) ? SensorTrayManager::tooltip(row).section('\n', 0, 0)
                                                : QString("Unavailable")},
                        {"header", false},
                        {"depth", 2}});
    }
    auto key = [](const QJsonArray &parts) {
        return QString::fromUtf8(QJsonDocument(parts).toJson(QJsonDocument::Compact));
    };
    QVariantList result;
    for (auto source = groups.begin(); source != groups.end(); ++source) {
        const auto sourceKey = key({source.key()});
        result.append(QVariantMap{{"header", true},
                                 {"depth", 0},
                                 {"key", sourceKey},
                                 {"name", SensorTrayManager::moduleName(source.key())}});
        if (words.isEmpty() && collapsed.value(sourceKey).toBool())
            continue;
        for (auto group = source.value().begin(); group != source.value().end(); ++group) {
            const auto groupKey = key({source.key(), group.key()});
            result.append(
                QVariantMap{{"header", true}, {"depth", 1}, {"key", groupKey}, {"name", group.key()}});
            if (words.isEmpty() && collapsed.value(groupKey).toBool())
                continue;
            auto &rows = group.value();
            std::sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
                const auto left = a.toMap(), right = b.toMap();
                const int order = left["name"].toString().localeAwareCompare(right["name"].toString());
                return order ? order < 0 : left["id"].toString() < right["id"].toString();
            });
            result.append(rows);
        }
    }
    return result;
}
} // namespace

void SensorTrayManager::searchPicker(const QString &query, const QVariantMap &collapsed) {
    pickerActive = true;
    pickerQuery = query;
    pickerCollapsed = collapsed;
    pickerPending = true;
    ++pickerGeneration;
    preparePicker();
    emit pickerChanged();
}

void SensorTrayManager::closePicker() {
    pickerActive = false;
    pickerPending = false;
    ++pickerGeneration;
    pickerModelData.replace({});
    emit pickerChanged();
}

void SensorTrayManager::preparePicker() {
    if (!pickerActive || !pickerPending || !readingsComplete || pickerJob.isRunning())
        return;
    pickerPending = false;
    const auto generation = pickerGeneration;
    // Copy-on-write snapshots keep sorting, grouping and text formatting off the GUI thread.
    const auto snapshot = readings;
    const auto query = pickerQuery;
    const auto collapsed = pickerCollapsed;
    pickerJob.submit([snapshot, query, collapsed] { return pickerRows(snapshot, query, collapsed); },
                     [this, generation](QVariantList rows) {
                         // A closed picker or newer search must never receive an older result.
                         if (pickerActive && generation == pickerGeneration)
                             pickerModelData.replace(rows);
                         preparePicker();
                         emit pickerChanged();
                     });
}

void SensorTrayManager::setContextMenu(QMenu *menu) {
    contextMenu = menu;
}

void SensorTrayManager::attachMainTray(QSystemTrayIcon *icon) {
    mainTray = icon;
    rendered.remove("main");
    refresh();
}

void SensorTrayManager::setSensor(const QString &id, const QString &sensor) {
    if (id != "main" || !readings.contains(sensor))
        return;
    auto primary = configuration[0].toMap();
    primary["sensor"] = sensor;
    primary["source"] = readings[sensor]["source"];
    primary["image"] = "";
    configuration[0] = primary;
    save();
}

QString SensorTrayManager::selectImageFile() {
    return QFileDialog::getOpenFileName(nullptr, "Choose Icon", {}, "Images (*.png *.ico *.jpg *.jpeg *.bmp)");
}

QVariantMap SensorTrayManager::readImageFile(const QString &path) {
    // File access, decoding, scaling and PNG encoding all run on the image worker.
    if (QFileInfo(path).size() > 32 * 1024 * 1024)
        return {{"error", "Choose an image file smaller than 32 MiB."}};
    QImageReader reader(path);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (!size.isValid() || size.width() > 8192 || size.height() > 8192)
        return {{"error", "Choose a readable image no larger than 8192 × 8192 pixels."}};
    // Formats such as JPEG can decode at the requested size without allocating
    // the full image. Other formats remain bounded by the dimensions above.
    reader.setScaledSize(size.scaled(256, 256, Qt::KeepAspectRatio));
    const auto image = reader.read().scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (image.isNull())
        return {{"error", "This image could not be read. Try a PNG or ICO file."}};
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        return {{"error", "The icon image could not be prepared."}};
    return {{"image", QString::fromLatin1(bytes.toBase64())}};
}

void SensorTrayManager::chooseImage(const QString &id) {
    if (id != "main" || (imageJob && imageJob->isRunning()))
        return;
    const auto path = selectImageFile();
    if (path.isEmpty())
        return;
    if (!imageJob)
        imageJob = std::make_unique<BackgroundWorker>("Tray icon image");
    const auto generation = ++imageGeneration;
    imageJob->onStatus = [this](const QString &error) {
        if (!error.isEmpty()) {
            lastError = "The icon image could not be prepared.";
            emit valuesChanged();
        }
    };
    imageJob->submit([path] { return readImageFile(path); }, [this, generation](QVariantMap result) {
        // A reset or a new sensor selection must win over an in-flight import.
        if (generation != imageGeneration)
            return;
        lastError = result["error"].toString();
        emit valuesChanged();
        if (!result.contains("image"))
            return;
        auto primary = configuration[0].toMap();
        primary["sensor"] = "";
        primary["image"] = result["image"];
        configuration[0] = primary;
        save();
    });
}

void SensorTrayManager::restoreLogo() {
    configuration[0] = mainDefaults();
    save();
}

void SensorTrayManager::addSelected(const QStringList &ids) {
    QSet<QString> existing;
    for (auto item : configuration)
        existing.insert(item.toMap()["id"].toString());
    bool changed = false;
    for (const auto &id : ids) {
        if (configuration.size() >= 65)
            break;
        if (existing.contains(id) || !readings.contains(id))
            continue;
        configuration << defaults(id, readings[id]["name"].toString());
        existing.insert(id);
        changed = true;
    }
    if (changed)
        save();
}

void SensorTrayManager::add(const QString &id) {
    if (!readings.contains(id) || configuration.size() >= 65)
        return;
    for (auto item : configuration)
        if (item.toMap()["id"] == id)
            return;
    auto entry = defaults(id, readings[id]["name"].toString());
    entry["source"] = readings[id]["source"];
    configuration << entry;
    save();
}

void SensorTrayManager::remove(const QString &id) {
    for (int i = 1; i < configuration.size(); ++i)
        if (configuration[i].toMap()["id"] == id) {
            configuration.removeAt(i);
            save();
            return;
        }
}

void SensorTrayManager::style(const QString &id, const QString &key, const QVariant &value) {
    for (int i = 0; i < configuration.size(); ++i) {
        auto row = configuration[i].toMap();
        if (row["id"] != id)
            continue;
        row[key] = value;
        configuration[i] = row;
        save();
        return;
    }
}

void SensorTrayManager::setColor(const QString &id, bool background, const QColor &colour) {
    if (colour.isValid())
        style(id, background ? "background" : "foreground", colour.name(QColor::HexArgb));
}

void SensorTrayManager::chooseFont(const QString &id) {
    for (auto item : configuration) {
        auto row = item.toMap();
        if (row["id"] != id)
            continue;
        QFont font;
        font.fromString(row["font"].toString());
        bool ok = false;
        auto chosen = QFontDialog::getFont(&ok, font, nullptr, "Tray Sensor Font");
        if (ok)
            style(id, "font", chosen.toString());
        return;
    }
}

void SensorTrayManager::resetStyle(const QString &id) {
    for (int i = 0; i < configuration.size(); ++i)
        if (configuration[i].toMap()["id"] == id) {
            auto row = configuration[i].toMap();
            const auto clean = defaults(id, row["name"].toString());
            for (const auto *key : {"foreground", "background", "font"})
                row[key] = clean[key];
            configuration[i] = row;
            save();
            return;
        }
}

void SensorTrayManager::save() {
    ++imageGeneration;
    settings.setValue(
        "tray/main",
        QJsonDocument(QJsonObject::fromVariantMap(configuration[0].toMap())).toJson(QJsonDocument::Compact));
    settings.setValue(
        "tray/sensors",
        QJsonDocument(QJsonArray::fromVariantList(configuration.mid(1))).toJson(QJsonDocument::Compact));
    rendered.clear();
    refresh();
    emit configurationChanged();
}

void SensorTrayManager::refresh() {
    QSet<QString> keep;
    QVariantMap next;
    for (auto item : configuration) {
        auto s = item.toMap();
        const auto id = s["id"].toString();
        keep.insert(id);
        const auto sensor = s.value("sensor", id).toString();
        auto row = readings.value(sensor, {{"name", s["name"]}, {"source", s["source"]}, {"available", false}});
        auto text = sensor.isEmpty() ? QString() : compact(row),
             tip = sensor.isEmpty() ? mainToolTip : tooltip(row);
        QString preview;
        auto *icon = icons.value(id);
        if (id != "main" && !testMode && !icon) {
            icon = new NativeSensorIcon(settings.fileName(), id, this);
            icons[id] = icon;
            icon->activated = [this](auto reason) {
                if (reason == QSystemTrayIcon::Context && contextMenu)
                    popupWhatTrayMenu(contextMenu);
                else if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick)
                    emit openRequested();
            };
            icon->statusChanged = [this] { refresh(); };
        }
        const auto sizes = id == "main" ? QList<int>{16, 24, 32, 48, 64, 128, 256} : QList<int>{16, 32, 48};
        const auto artwork = renderer.get({id, text, s, sizes, browsing});
        if (artwork) {
            preview = artwork->preview;
            if (!rendered.contains(id) || rendered.value(id) != text) {
                QIcon image;
                for (const auto &pixels : artwork->images)
                    image.addPixmap(QPixmap::fromImage(pixels));
                if (icon)
                    icon->setIcon(image);
                if (id == "main" && mainTray)
                    mainTray->setIcon(sensor.isEmpty() && s["image"].toString().isEmpty()
                                          ? QIcon(":/assets/app-icon.ico")
                                          : image);
                rendered[id] = text;
            }
        }
        if (id == "main" && mainTray && mainTray->toolTip() != tip)
            mainTray->setToolTip(tip);
        if (icon) {
            if (icon->toolTip() != tip)
                icon->setToolTip(tip);
            if (!icon->isVisible() && !icon->icon().isNull())
                icon->show();
        }
        next[id] = QVariantMap{{"text", text},
                               {"module", sensor.isEmpty() ? QString() : moduleName(row["source"].toString())},
                               {"tooltip", tip},
                               {"preview", preview},
                               {"error", icon ? icon->error() : QString{}}};
    }
    for (const auto &id : icons.keys())
        if (!keep.contains(id)) {
            auto *icon = icons.take(id);
            icon->hide();
            icon->deleteLater();
            rendered.remove(id);
        }
    renderer.retain(keep);
    if (displayValues != next) {
        displayValues = next;
        emit valuesChanged();
    }
}
