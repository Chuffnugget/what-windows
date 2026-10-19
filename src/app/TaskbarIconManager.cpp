#include "app/SettingsStore.h"
#include "TaskbarIconManager.h"
#include "SensorTrayManager.h"
#include "TaskbarIcon.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFontDialog>
#include <QCloseEvent>
#include <QTimer>
#include <functional>
#include <QCryptographicHash>
#include <QUuid>
#include <windows.h>
#include <shellapi.h>
#include <propsys.h>
#include <propkey.h>

namespace {
QVariantMap defaults(const QString &id, const QString &name) {
    QFont font("Segoe UI", 11, QFont::DemiBold);
    return {{"id", id},
            {"name", name},
            {"sensor", ""},
            {"image", ""},
            {"foreground", "#ff66caff"},
            {"background", "#00000000"},
            {"font", font.toString()}};
}

bool windowIdentity(HWND window, const QString &id, bool clear = false) {
    IPropertyStore *store = nullptr;
    if (FAILED(SHGetPropertyStoreForWindow(window, IID_PPV_ARGS(&store))))
        return false;
    PROPVARIANT value{};
    if (!clear) {
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
    }
    // These are live indicators, not independently launchable applications.
    const auto pin = store->SetValue(PKEY_AppUserModel_PreventPinning, value);
    value = {};
    if (!clear) {
        value.vt = VT_LPWSTR;
        value.pwszVal = const_cast<LPWSTR>(reinterpret_cast<LPCWSTR>(id.utf16()));
    }
    const auto result = store->SetValue(PKEY_AppUserModel_ID, value);
    store->Release();
    return SUCCEEDED(pin) && SUCCEEDED(result);
}

// Windows requires an HWND for a taskbar button. This backing window is never
// rendered on a monitor; activation routes to WHAT instead of opening a panel.
class SensorPanel : public QWidget {
public:
    std::function<void()> activate;

    SensorPanel() : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint) {
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        setGeometry(-32000, -32000, 1, 1);
        const auto hwnd = reinterpret_cast<HWND>(winId());
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE,
                          (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & ~WS_EX_TOOLWINDOW) | WS_EX_NOACTIVATE |
                              WS_EX_APPWINDOW);
    }

    ~SensorPanel() override {
        windowIdentity(reinterpret_cast<HWND>(winId()), {}, true);
    }

protected:
    bool nativeEvent(const QByteArray &type, void *message, qintptr *result) override {
        auto *msg = static_cast<MSG *>(message);
        if (msg->message == WM_WINDOWPOSCHANGING) {
            auto *position = reinterpret_cast<WINDOWPOS *>(msg->lParam);
            position->x = position->y = -32000;
            position->cx = position->cy = 1;
            position->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
            position->flags |= SWP_NOACTIVATE;
        }
        const auto command = msg->wParam & 0xfff0;
        if ((msg->message == WM_SYSCOMMAND &&
             (command == SC_RESTORE || command == SC_MAXIMIZE || command == SC_MINIMIZE)) ||
            (msg->message == WM_ACTIVATE && LOWORD(msg->wParam) != WA_INACTIVE)) {
            if (activate)
                QTimer::singleShot(0, this, [this] {
                    if (activate)
                        activate();
                });
            *result = 0;
            return true;
        }
        return QWidget::nativeEvent(type, message, result);
    }

    void closeEvent(QCloseEvent *event) override {
        event->ignore();
        if (activate)
            activate();
    }
};
} // namespace

TaskbarIconManager::TaskbarIconManager(const QString &file, bool testing, QObject *parent)
    : QObject(parent), settings(file, SettingsStore::format()), testMode(testing),
      renderer([](const IconRenderer::Request &r, int size) {
          return SensorTrayManager::render(r.text, r.style, size);
      }) {
    renderer.ready = [this] { refresh(); };
    configuration << defaults("main", "WHAT App Icon");
    QSet<QString> seen;
    for (const auto &value : QJsonDocument::fromJson(settings.value("taskbar/icons").toByteArray()).array()) {
        const auto row = value.toObject().toVariantMap();
        const auto id = row["id"].toString();
        if (id.isEmpty() || id.size() > 80 || seen.contains(id) || configuration.size() >= 33)
            continue;
        auto clean = defaults(id, row["name"].toString().left(120));
        clean["sensor"] = row["sensor"].toString().left(512);
        if (row["image"].toString().size() <= 1024 * 1024)
            clean["image"] = row["image"];
        for (const auto *key : {"foreground", "background"})
            if (QColor(row[key].toString()).isValid())
                clean[key] = row[key];
        QFont font;
        if (font.fromString(row["font"].toString()))
            clean["font"] = font.toString();
        if (id == "main") {
            clean["name"] = "WHAT App Icon";
            configuration[0] = clean;
        } else
            configuration << clean;
        seen.insert(id);
    }
    refresh();
}

TaskbarIconManager::~TaskbarIconManager() {
    qDeleteAll(panels);
}

void TaskbarIconManager::attachMainWindow(QWindow *window) {
    mainWindow = window;
    connect(window, &QWindow::screenChanged, this, [this] {
        rendered.clear();
        refresh();
    });
    rendered.clear();
    refresh();
}

void TaskbarIconManager::setReadings(const QVariantList &rows) {
    readings.clear();
    for (const auto &value : rows) {
        const auto row = value.toMap();
        readings[row["id"].toString()] = row;
    }
    refresh();
}

void TaskbarIconManager::addSelected(const QStringList &sensors) {
    for (const auto &sensor : sensors) {
        if (configuration.size() >= 33)
            break;
        if (!readings.contains(sensor))
            continue;
        auto row =
            defaults(QUuid::createUuid().toString(QUuid::WithoutBraces), readings[sensor]["name"].toString());
        row["sensor"] = sensor;
        configuration << row;
    }
    save();
}

void TaskbarIconManager::setSensor(const QString &id, const QString &sensor) {
    if (!readings.contains(sensor))
        return;
    change(id, "image", "");
    change(id, "sensor", sensor);
}

void TaskbarIconManager::change(const QString &id, const QString &key, const QVariant &value) {
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

void TaskbarIconManager::remove(const QString &id) {
    if (id == "main")
        return;
    for (int i = 1; i < configuration.size(); ++i)
        if (configuration[i].toMap()["id"] == id) {
            configuration.removeAt(i);
            save();
            return;
        }
}

void TaskbarIconManager::rename(const QString &id, const QString &name) {
    if (id != "main" && !name.trimmed().isEmpty())
        change(id, "name", name.trimmed().left(120));
}

void TaskbarIconManager::setColor(const QString &id, bool background, const QColor &colour) {
    if (colour.isValid())
        change(id, background ? "background" : "foreground", colour.name(QColor::HexArgb));
}

void TaskbarIconManager::chooseFont(const QString &id) {
    for (const auto &value : configuration) {
        const auto row = value.toMap();
        if (row["id"] != id)
            continue;
        QFont font;
        font.fromString(row["font"].toString());
        bool ok = false;
        font = QFontDialog::getFont(&ok, font, nullptr, "App Icon Font");
        if (ok)
            change(id, "font", font.toString());
        return;
    }
}

void TaskbarIconManager::importImage(const QString &id, bool adding) {
    if ((imageJob && imageJob->isRunning()) || (adding && configuration.size() >= 33))
        return;
    const auto path = SensorTrayManager::selectImageFile();
    if (path.isEmpty())
        return;
    if (!imageJob)
        imageJob = std::make_unique<BackgroundWorker>("App icon image");
    const auto generation = ++imageGeneration;
    imageJob->onStatus = [this](const QString &error) {
        if (!error.isEmpty()) {
            lastError = "The icon image could not be prepared.";
            emit valuesChanged();
        }
    };
    imageJob->submit([path] { return SensorTrayManager::readImageFile(path); },
        [this, id, adding, generation](QVariantMap result) {
            if (generation != imageGeneration)
                return;
            lastError = result["error"].toString();
            emit valuesChanged();
            if (!result.contains("image"))
                return;
            if (adding) {
                auto row = defaults(QUuid::createUuid().toString(QUuid::WithoutBraces), "Custom app icon");
                row["image"] = result["image"];
                configuration << row;
                save();
            } else {
                change(id, "sensor", "");
                change(id, "image", result["image"]);
            }
        });
}

void TaskbarIconManager::chooseImage(const QString &id) {
    importImage(id, false);
}

void TaskbarIconManager::addCustom() {
    importImage({}, true);
}

void TaskbarIconManager::restoreLogo() {
    configuration[0] = defaults("main", "WHAT App Icon");
    save();
}

void TaskbarIconManager::resetStyle(const QString &id) {
    const auto style = defaults({}, {});
    for (const auto *key : {"foreground", "background", "font"})
        change(id, key, style[key]);
}

void TaskbarIconManager::openPanel(const QString &id) {
    if (panels.contains(id))
        emit openRequested();
}

void TaskbarIconManager::save() {
    ++imageGeneration;
    settings.setValue(
        "taskbar/icons",
        QJsonDocument(QJsonArray::fromVariantList(configuration)).toJson(QJsonDocument::Compact));
    rendered.clear();
    refresh();
    emit configurationChanged();
}

void TaskbarIconManager::refresh() {
    QSet<QString> keep;
    QVariantMap next;
    for (const auto &value : configuration) {
        const auto style = value.toMap();
        const auto id = style["id"].toString();
        const auto sensor = style["sensor"].toString();
        const auto row = readings.value(sensor, {{"name", style["name"]}, {"available", false}});
        const auto text = sensor.isEmpty() ? QString() : SensorTrayManager::compact(row);
        const auto tip = sensor.isEmpty() ? style["name"].toString() : SensorTrayManager::tooltip(row);
        keep.insert(id);
        QString preview;
        const auto artwork = renderer.get({id, text, style, {16, 24, 32, 48, 64, 128, 256}, browsing});
        if (artwork) {
            preview = artwork->preview;
            if (!rendered.contains(id) || rendered[id] != text) {
                QIcon icon;
                for (const auto &pixels : artwork->images)
                    icon.addPixmap(QPixmap::fromImage(pixels));
                if (icon.isNull())
                    icon = QIcon(":/assets/app-icon.ico");
                cachedIcons[id] = icon;
                rendered[id] = text;
                if (id == "main" && mainWindow) {
                    mainWindow->setIcon(icon);
                    if (sensor.isEmpty() && style["image"].toString().isEmpty())
                        setNativeTaskbarIcon(reinterpret_cast<HWND>(mainWindow->winId()));
                }
                if (auto *panel = panels.value(id))
                    panel->setWindowIcon(icon);
            }
        }
        QString error;
        if (id != "main" && !testMode && cachedIcons.contains(id)) {
            auto *panel = static_cast<SensorPanel *>(panels.value(id));
            if (!panel) {
                panel = new SensorPanel;
                panels[id] = panel;
                panel->setObjectName("taskbarPanel_" + id);
                panel->activate = [this] { emit openRequested(); };
                const auto profile =
                    QCryptographicHash::hash(settings.fileName().toUtf8(), QCryptographicHash::Sha256)
                        .toHex()
                        .left(12);
                if (!windowIdentity(reinterpret_cast<HWND>(panel->winId()),
                                    "WHAT.Indicator." + QString::fromLatin1(profile) + "." + id))
                    panel->setProperty("taskbarError", "Windows could not assign a separate taskbar group.");
                panel->setWindowIcon(cachedIcons[id]);
                panel->show();
            }
            error = panel->property("taskbarError").toString();
            panel->setWindowTitle(style["name"].toString() + " - WHAT");
            panel->setToolTip(tip);
        }
        next[id] = QVariantMap{{"text", text}, {"tooltip", tip}, {"preview", preview}, {"error", error}};
    }
    for (const auto &id : panels.keys())
        if (!keep.contains(id))
            delete panels.take(id);
    for (const auto &id : cachedIcons.keys())
        if (!keep.contains(id)) {
            cachedIcons.remove(id);
            rendered.remove(id);
        }
    renderer.retain(keep);
    if (displayValues != next) {
        displayValues = next;
        emit valuesChanged();
    }
}
