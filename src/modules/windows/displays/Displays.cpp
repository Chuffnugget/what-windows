#include "Displays.h"
#include "modules/windows/specs/SpecsDisplayDetails.h"
#include <windows.h>
#include <QCryptographicHash>
#include <QSet>
#include <QMap>
#include <QRegularExpression>
#include <vector>
#include <cmath>
#include <algorithm>

namespace {
QString adapter(LUID id) {
    return QString::number(id.HighPart) + ":" + QString::number(id.LowPart);
}

QString sourceKey(const DISPLAYCONFIG_PATH_INFO &p) {
    return adapter(p.sourceInfo.adapterId) + ":" + QString::number(p.sourceInfo.id);
}

struct Configuration {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    QMap<QString, QList<int>> targets;
    QJsonArray monitors;
    QMap<QString, QString> sourceNames;
    QString error;
};

QString failure(const QString &operation, LONG code) {
    return QString("%1 failed (Windows %2). %3")
        .arg(operation)
        .arg(code)
        .arg(
            code == ERROR_ACCESS_DENIED
                ? "Display control requires the signed-in console session; Remote Desktop sessions are not supported."
            : code == ERROR_BAD_CONFIGURATION ? "The graphics driver cannot use this display arrangement."
                                              : "Refresh displays and check Windows Display settings.");
}

// Match the physical monitor interface before using GDI's saved desktop mode.
// An arbitrary inactive CCD path can belong to a different desktop source.
QMap<QString, QJsonObject> savedMonitorModes() {
    QMap<QString, QJsonObject> result;
    for (DWORD a = 0;; ++a) {
        DISPLAY_DEVICEW device{};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, a, &device, 0))
            break;
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsExW(device.DeviceName, ENUM_REGISTRY_SETTINGS, &mode, 0))
            continue;
        if (!(mode.dmFields & DM_DISPLAYORIENTATION) || mode.dmDisplayOrientation > DMDO_270)
            continue;
        // DEVMODE uses counter-clockwise rotations; CCD uses clockwise rotations.
        const int rotation = 1 + (4 - int(mode.dmDisplayOrientation)) % 4;
        for (DWORD m = 0;; ++m) {
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesW(device.DeviceName, m, &monitor, EDD_GET_DEVICE_INTERFACE_NAME))
                break;
            const auto path = QString::fromWCharArray(monitor.DeviceID).toLower();
            if (path.isEmpty())
                continue;
            QJsonObject saved{{"rotation", rotation}};
            if ((mode.dmFields & DM_PELSWIDTH) && (mode.dmFields & DM_PELSHEIGHT) && mode.dmPelsWidth &&
                mode.dmPelsHeight) {
                saved["width"] = int(mode.dmPelsWidth);
                saved["height"] = int(mode.dmPelsHeight);
            }
            result.insert(path, saved);
        }
    }
    return result;
}

Configuration read(bool includeSpecifications = false) {
    Configuration c;
    LONG rc = ERROR_INSUFFICIENT_BUFFER;
    for (int retry = 0; retry < 4 && rc == ERROR_INSUFFICIENT_BUFFER; ++retry) {
        UINT32 paths = 0, modes = 0;
        rc = GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &paths, &modes);
        if (rc != ERROR_SUCCESS)
            break;
        if (paths > 16384 || modes > 32768) {
            c.error = "Display configuration exceeds supported bounds";
            return c;
        }
        c.paths.resize(paths);
        c.modes.resize(modes);
        rc = QueryDisplayConfig(QDC_ALL_PATHS, &paths, c.paths.data(), &modes, c.modes.data(), nullptr);
        if (rc == ERROR_SUCCESS) {
            c.paths.resize(paths);
            c.modes.resize(modes);
        }
    }
    if (rc != ERROR_SUCCESS) {
        c.error = failure("Reading displays", rc);
        return c;
    }
    const auto savedModes = savedMonitorModes();
    QMap<QString, QJsonObject> monitors;
    QMap<QString, QString> groups;
    for (size_t i = 0; i < c.paths.size(); ++i) {
        const auto &p = c.paths[i];
        const bool active = p.flags & DISPLAYCONFIG_PATH_ACTIVE;
        if (!p.targetInfo.targetAvailable && !active)
            continue;
        DISPLAYCONFIG_TARGET_DEVICE_NAME name{};
        name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(name), p.targetInfo.adapterId,
                       p.targetInfo.id};
        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS)
            continue;
        QString path = QString::fromWCharArray(name.monitorDevicePath).toLower();
        if (path.isEmpty())
            continue; // No fabricated persistent IDs for unidentified virtual outputs.
        const auto id = QString::fromLatin1(
            QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
        c.targets[id].append(int(i));
        if (monitors.contains(id) && !active)
            continue;
        auto label = QString::fromWCharArray(name.monitorFriendlyDeviceName).trimmed();
        if (label.isEmpty())
            label = "Monitor";
        QJsonObject row{{"id", id},
                        {"name", label + " · " + id.right(6)},
                        {"connected", true},
                        {"enabled", active},
                        {"primary", false},
                        {"group", id},
                        {"mode", active ? "extended" : "disconnected"},
                        {"width", 0},
                        {"height", 0},
                        {"x", 0},
                        {"y", 0},
                        {"refresh", 0.0},
                        {"rotation", 1}};
        if (!active) {
            DISPLAYCONFIG_TARGET_PREFERRED_MODE preferred{};
            preferred.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE, sizeof(preferred),
                                p.targetInfo.adapterId, p.targetInfo.id};
            if (DisplayConfigGetDeviceInfo(&preferred.header) == ERROR_SUCCESS) {
                row["width"] = int(preferred.width);
                row["height"] = int(preferred.height);
            }
        }
        if (!active && savedModes.contains(path)) {
            const auto saved = savedModes[path];
            row["rotation"] = saved["rotation"];
            if (saved.contains("width")) {
                row["width"] = saved["width"];
                row["height"] = saved["height"];
            } else if (row["rotation"].toInt() == 2 || row["rotation"].toInt() == 4) {
                const auto width = row["width"];
                row["width"] = row["height"];
                row["height"] = width;
            }
        }
        if (active)
            row["rotation"] = int(p.targetInfo.rotation);
        if (active && p.sourceInfo.modeInfoIdx < c.modes.size()) {
            const auto &m = c.modes[p.sourceInfo.modeInfoIdx];
            if (m.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
                const auto &s = m.sourceMode;
                row["width"] = int(s.width);
                row["height"] = int(s.height);
                row["x"] = int(s.position.x);
                row["y"] = int(s.position.y);
                row["primary"] = s.position.x == 0 && s.position.y == 0;
            }
            const auto key = sourceKey(p);
            if (!groups.contains(key))
                groups[key] = id;
            row["group"] = groups[key];
            row["rotation"] = int(p.targetInfo.rotation);
            if (p.targetInfo.refreshRate.Denominator)
                row["refresh"] =
                    double(p.targetInfo.refreshRate.Numerator) / p.targetInfo.refreshRate.Denominator;
        }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(sourceName),
                             p.sourceInfo.adapterId, p.sourceInfo.id};
        if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS)
            c.sourceNames[id] = QString::fromWCharArray(sourceName.viewGdiDeviceName);
        if (includeSpecifications)
            SpecsDisplayDetails::append(row, p, c.sourceNames.value(id));
        monitors[id] = row;
    }
    for (auto row : monitors) {
        if (row["enabled"].toBool()) {
            int count = 0;
            for (const auto &other : monitors)
                if (other["enabled"].toBool() && other["group"] == row["group"])
                    ++count;
            if (count > 1)
                row["mode"] = "mirrored";
        }
        QJsonArray supported;
        QSet<QString> seen;
        const auto source = c.sourceNames.value(row["id"].toString());
        // Specs needs the current mode only, not thousands of selectable modes.
        if (!includeSpecifications && !source.isEmpty())
            for (DWORD index = 0; index < 2048 && supported.size() < 256; ++index) {
                DEVMODEW mode{};
                mode.dmSize = sizeof(mode);
                if (!EnumDisplaySettingsExW(reinterpret_cast<LPCWSTR>(source.utf16()), index, &mode, 0))
                    break;
                int width = int(mode.dmPelsWidth), height = int(mode.dmPelsHeight);
                if (mode.dmDisplayOrientation == DMDO_90 || mode.dmDisplayOrientation == DMDO_270)
                    std::swap(width, height);
                const int refresh = int(mode.dmDisplayFrequency);
                if (width < 1 || height < 1 || width > 32768 || height > 32768 || refresh < 1 ||
                    refresh > 10000)
                    continue;
                const auto key = QString("%1:%2:%3").arg(width).arg(height).arg(refresh);
                if (seen.contains(key))
                    continue;
                seen.insert(key);
                supported.append(QJsonObject{{"width", width}, {"height", height}, {"refresh", refresh}});
            }
        row["modes"] = supported;
        c.monitors.append(row);
    }
    return c;
}
} // namespace

QJsonObject Displays::query(bool includeSpecifications) {
    auto c = read(includeSpecifications);
    return {{"available", c.error.isEmpty()},
            {"status", c.error.isEmpty() ? QString("%1 connected displays").arg(c.monitors.size()) : c.error},
            {"monitors", c.monitors}};
}

QJsonArray Displays::layout(const QJsonObject &state) {
    QJsonArray result;
    for (auto v : state["monitors"].toArray()) {
        auto m = v.toObject();
        if (!m["enabled"].toBool())
            continue;
        QJsonObject r;
        for (auto key : {"id", "group", "x", "y", "width", "height", "rotation", "refresh"})
            if (m.contains(key))
                r[key] = m[key];
        result.append(r);
    }
    return result;
}

QString Displays::validateLayout(const QJsonArray &rows) {
    if (rows.isEmpty() || rows.size() > 32)
        return "A layout must enable between 1 and 32 displays";
    QSet<QString> ids;
    QMap<QString, QJsonObject> groups;
    bool primary = false;
    for (auto v : rows) {
        if (!v.isObject())
            return "Invalid display layout";
        auto m = v.toObject();
        const auto id = m["id"].toString(), group = m["group"].toString();
        static const QRegularExpression valid("^[0-9a-f]{24}$");
        if (!valid.match(id).hasMatch() || !valid.match(group).hasMatch() || ids.contains(id))
            return "Invalid or duplicate monitor identity";
        ids.insert(id);
        for (auto key : {"x", "y", "width", "height", "rotation"})
            if (!m[key].isDouble() || !std::isfinite(m[key].toDouble()) ||
                m[key].toDouble() != m[key].toInt())
                return "Invalid display geometry";
        if (qAbs(m["x"].toInt()) > 65536 || qAbs(m["y"].toInt()) > 65536 || m["width"].toInt() < 1 ||
            m["width"].toInt() > 32768 || m["height"].toInt() < 1 || m["height"].toInt() > 32768 ||
            m["rotation"].toInt() < 1 || m["rotation"].toInt() > 4)
            return "Display geometry outside supported bounds";
        if (m.contains("refresh") && (!m["refresh"].isDouble() || !std::isfinite(m["refresh"].toDouble()) ||
                                      m["refresh"].toDouble() < 0 || m["refresh"].toDouble() > 10000))
            return "Invalid display refresh rate";
        if (groups.contains(group)) {
            for (auto key : {"x", "y", "width", "height"})
                if (groups[group][key] != m[key])
                    return "Mirrored displays must share desktop geometry";
        } else {
            if (m["x"].toInt() == 0 && m["y"].toInt() == 0) {
                if (primary)
                    return "Only one desktop group can be primary";
                primary = true;
            }
            groups[group] = m;
        }
    }
    return primary ? QString() : QString("A layout must include a primary display at 0,0");
}

QJsonObject Displays::edit(const QJsonObject &state, const QJsonObject &args) {
    auto fail = [](QString e) { return QJsonObject{{"error", e}}; };
    const auto action = args["operation"].toString(), id = args["monitor"].toString();
    if (action == "apply") {
        const auto wanted = args["layout"].toArray(), expected = args["expected"].toArray();
        const auto error = validateLayout(wanted);
        if (!error.isEmpty())
            return fail(error);
        if (!validateLayout(expected).isEmpty() || !matches(expected, layout(state)))
            return fail("The Windows display layout changed. Reload the preview before applying.");
        QSet<QString> connected;
        for (auto v : state["monitors"].toArray())
            connected.insert(v.toObject()["id"].toString());
        QMap<QString, QJsonObject> groups;
        for (auto v : wanted) {
            auto row = v.toObject();
            if (!connected.contains(row["id"].toString()))
                return fail("A monitor is no longer connected");
            groups[row["group"].toString()] = row;
        }
        for (auto a = groups.begin(); a != groups.end(); ++a)
            for (auto b = std::next(a); b != groups.end(); ++b) {
                const auto l = a.value(), r = b.value();
                if (l["x"].toInt() < r["x"].toInt() + r["width"].toInt() &&
                    r["x"].toInt() < l["x"].toInt() + l["width"].toInt() &&
                    l["y"].toInt() < r["y"].toInt() + r["height"].toInt() &&
                    r["y"].toInt() < l["y"].toInt() + l["height"].toInt())
                    return fail("Extended displays must not overlap. Use Duplicate explicitly.");
            }
        return {{"layout", wanted}};
    }
    QJsonObject target;
    for (auto v : state["monitors"].toArray())
        if (v.toObject()["id"] == id)
            target = v.toObject();
    if (target.isEmpty())
        return fail("Monitor is no longer connected");
    auto current = layout(state);
    QJsonArray next;
    QJsonObject row;
    for (auto v : current) {
        if (v.toObject()["id"] == id)
            row = v.toObject();
        else
            next.append(v);
    }
    if (action == "primary") {
        if (row.isEmpty())
            return fail("Enable the monitor before making it primary");
        const int x = row["x"].toInt(), y = row["y"].toInt();
        next = {};
        for (auto v : current) {
            auto r = v.toObject();
            r["x"] = r["x"].toInt() - x;
            r["y"] = r["y"].toInt() - y;
            next.append(r);
        }
    } else if (action == "disconnect") {
        if (row.isEmpty())
            return fail("Monitor is already disconnected from the desktop");
        if (next.isEmpty())
            return fail("Cannot disconnect the last active display");
        bool primary = false;
        for (auto v : next)
            if (v.toObject()["x"].toInt() == 0 && v.toObject()["y"].toInt() == 0)
                primary = true;
        if (!primary) {
            auto first = next.first().toObject();
            QJsonArray moved;
            for (auto v : next) {
                auto r = v.toObject();
                r["x"] = r["x"].toInt() - first["x"].toInt();
                r["y"] = r["y"].toInt() - first["y"].toInt();
                moved.append(r);
            }
            next = moved;
        }
    } else if (action == "mirror") {
        QJsonObject source;
        for (auto v : next)
            if (v.toObject()["id"] == args["source"])
                source = v.toObject();
        if (source.isEmpty())
            return fail("Choose a different, enabled mirror source");
        row = source;
        row["id"] = id;
        row["rotation"] = target["rotation"].toInt(1);
        next.append(row);
        // Moving the old primary to another group needs a new desktop origin.
        bool primary = false;
        for (auto v : next)
            if (v.toObject()["x"].toInt() == 0 && v.toObject()["y"].toInt() == 0)
                primary = true;
        if (!primary) {
            QJsonArray moved;
            for (auto v : next) {
                auto r = v.toObject();
                r["x"] = r["x"].toInt() - source["x"].toInt();
                r["y"] = r["y"].toInt() - source["y"].toInt();
                moved.append(r);
            }
            next = moved;
        }
    } else if (action == "extend") {
        if (row.isEmpty())
            row = {{"id", id},
                   {"width", target["width"].toInt(1920)},
                   {"height", target["height"].toInt(1080)},
                   {"rotation", target["rotation"].toInt(1)}};
        int right = 0;
        for (auto v : next) {
            auto r = v.toObject();
            right = qMax(right, r["x"].toInt() + r["width"].toInt());
        }
        // Keep an already independent display in place; split clones to the right.
        bool shared = false;
        for (auto v : next)
            if (v.toObject()["group"] == row["group"])
                shared = true;
        if (!target["enabled"].toBool() || shared) {
            row["x"] = next.isEmpty() ? 0 : right;
            row["y"] = 0;
        }
        if (shared) {
            QString replacement;
            for (auto v : next)
                if (v.toObject()["group"] == id) {
                    replacement = v.toObject()["id"].toString();
                    break;
                }
            if (!replacement.isEmpty())
                for (int i = 0; i < next.size(); ++i) {
                    auto r = next[i].toObject();
                    if (r["group"] == id) {
                        r["group"] = replacement;
                        next[i] = r;
                    }
                }
        }
        row["group"] = id;
        next.append(row);
    } else
        return fail("Unknown display operation");
    auto error = validateLayout(next);
    return error.isEmpty() ? QJsonObject{{"layout", next}} : fail(error);
}

QString Displays::apply(const QJsonArray &wanted, bool validateOnly) {
    auto error = validateLayout(wanted);
    if (!error.isEmpty())
        return error;
    const auto c = read();
    if (!c.error.isEmpty())
        return c.error;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    QMap<QString, DISPLAYCONFIG_PATH_INFO> groups;
    QMap<QString, UINT32> indexes;
    QSet<QString> used;
    for (auto v : wanted) {
        const auto row = v.toObject();
        const auto id = row["id"].toString(), group = row["group"].toString();
        if (!c.targets.contains(id))
            return "A display required by this layout is unplugged or its connector identity changed";
        int chosen = -1;
        // Prefer current active paths, then other driver-enumerated source/target combinations.
        auto candidates = c.targets[id];
        std::stable_sort(candidates.begin(), candidates.end(), [&](int a, int b) {
            return bool(c.paths[a].flags & DISPLAYCONFIG_PATH_ACTIVE) >
                   bool(c.paths[b].flags & DISPLAYCONFIG_PATH_ACTIVE);
        });
        for (int index : candidates) {
            auto p = c.paths[index];
            if (groups.contains(group)) {
                if (sourceKey(p) == sourceKey(groups[group])) {
                    chosen = index;
                    break;
                }
            } else if (!used.contains(sourceKey(p))) {
                chosen = index;
                break;
            }
        }
        if (chosen < 0)
            return "The graphics driver has no compatible display path (cross-adapter mirroring may be unsupported)";
        auto p = c.paths[chosen];
        p.flags = DISPLAYCONFIG_PATH_ACTIVE;
        if (!groups.contains(group)) {
            DISPLAYCONFIG_MODE_INFO m{};
            m.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
            m.id = p.sourceInfo.id;
            m.adapterId = p.sourceInfo.adapterId;
            m.sourceMode.width = row["width"].toInt();
            m.sourceMode.height = row["height"].toInt();
            m.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
            m.sourceMode.position = {row["x"].toInt(), row["y"].toInt()};
            indexes[group] = UINT32(modes.size());
            modes.push_back(m);
            groups[group] = p;
            used.insert(sourceKey(p));
        }
        p.sourceInfo.modeInfoIdx = indexes[group];
        p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        p.targetInfo.rotation = DISPLAYCONFIG_ROTATION(row["rotation"].toInt());
        p.targetInfo.scaling = DISPLAYCONFIG_SCALING_PREFERRED;
        p.targetInfo.refreshRate = {0, 0};
        p.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_UNSPECIFIED;
        // Preserve the existing output timing whenever its desktop size/rotation
        // still fits. Newly enabled or resized outputs use the driver's best mode.
        for (int original : c.targets[id]) {
            const auto &old = c.paths[original];
            if (!(old.flags & DISPLAYCONFIG_PATH_ACTIVE) || old.sourceInfo.modeInfoIdx >= c.modes.size() ||
                old.targetInfo.modeInfoIdx >= c.modes.size())
                continue;
            const auto &sm = c.modes[old.sourceInfo.modeInfoIdx], &tm = c.modes[old.targetInfo.modeInfoIdx];
            if (sm.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
                tm.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET ||
                int(sm.sourceMode.width) != row["width"].toInt() ||
                int(sm.sourceMode.height) != row["height"].toInt() ||
                old.targetInfo.rotation != p.targetInfo.rotation)
                continue;
            auto target = tm;
            target.adapterId = p.targetInfo.adapterId;
            target.id = p.targetInfo.id;
            p.targetInfo.modeInfoIdx = UINT32(modes.size());
            modes.push_back(target);
            p.targetInfo.refreshRate = old.targetInfo.refreshRate;
            p.targetInfo.scanLineOrdering = old.targetInfo.scanLineOrdering;
            p.targetInfo.scaling = old.targetInfo.scaling;
            break;
        }
        if (row["refresh"].toDouble() > 0 && std::abs(double(p.targetInfo.refreshRate.Numerator) /
                                                          qMax(1u, p.targetInfo.refreshRate.Denominator) -
                                                      row["refresh"].toDouble()) > 0.01) {
            p.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
            p.targetInfo.refreshRate = {UINT32(std::round(row["refresh"].toDouble() * 1000)), 1000};
        }
        paths.push_back(p);
    }
    const UINT32 flags = SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;
    auto rc = SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(modes.size()), modes.data(),
                               flags | SDC_VALIDATE);
    if (rc != ERROR_SUCCESS)
        return failure("Validating display layout", rc);
    if (validateOnly)
        return {};
    rc = SetDisplayConfig(UINT32(paths.size()), paths.data(), UINT32(modes.size()), modes.data(),
                          flags | SDC_APPLY);
    return rc == ERROR_SUCCESS ? QString() : failure("Applying display layout", rc);
}

bool Displays::matches(const QJsonArray &wanted, const QJsonArray &actual) {
    if (wanted.size() != actual.size())
        return false;
    QMap<QString, QJsonObject> a, b;
    for (auto v : wanted)
        a[v.toObject()["id"].toString()] = v.toObject();
    for (auto v : actual)
        b[v.toObject()["id"].toString()] = v.toObject();
    if (a.keys() != b.keys())
        return false;
    for (auto it = a.begin(); it != a.end(); ++it) {
        auto r = b[it.key()];
        for (auto k : {"x", "y", "width", "height", "rotation"})
            if (it.value()[k] != r[k])
                return false;
        if (it.value()["refresh"].toDouble() > 0 &&
            std::abs(it.value()["refresh"].toDouble() - r["refresh"].toDouble()) > 0.05)
            return false;
        for (auto jt = a.begin(); jt != a.end(); ++jt)
            if ((it.value()["group"] == jt.value()["group"]) != (r["group"] == b[jt.key()]["group"]))
                return false;
    }
    return true;
}

QJsonArray Displays::readings(const QJsonObject &state) {
    QJsonArray rows;
    for (auto v : state["monitors"].toArray()) {
        auto m = v.toObject();
        for (auto key : {"connected", "enabled", "primary", "mode", "width", "height", "refresh", "x", "y",
                         "rotation"}) {
            QString k = key;
            const bool binary = k == "connected" || k == "enabled" || k == "primary";
            rows.append(QJsonObject{{"id", "win_display_" + m["id"].toString() + "_" + k},
                                    {"source", "windows"},
                                    {"group", "Displays / " + m["name"].toString()},
                                    {"name", k == "enabled" ? "Desktop enabled"
                                             : k == "mode"  ? "Desktop mode"
                                                            : k.left(1).toUpper() + k.mid(1)},
                                    {"value", m[k]},
                                    {"available", state["available"]},
                                    {"kind", binary        ? "binary"
                                             : k == "mode" ? "text"
                                                           : "number"},
                                    {"unit", k == "refresh" ? "Hz" : ""},
                                    {"type", 0},
                                    {"recommended", false}});
        }
    }
    return rows;
}
