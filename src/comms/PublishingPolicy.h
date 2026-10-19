#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QSet>
#include <cmath>

// Each transport peer owns a cache: scope changes never reuse another peer's values.
class PublishingPolicy {
    QHash<QString, QJsonObject> values;
    QHash<QString, qint64> emitted;
    int wake = 0;

public:
    void reset() {
        values.clear();
        emitted.clear();
    }

    int nextWake() const {
        return wake;
    }

    QJsonObject apply(QJsonObject frame, bool cloud, qint64 now, bool event = false) {
        wake = 0;
        if (cloud) {
            frame.remove("selected_sensor_ids");
            frame.remove("entity_cleanup");
        }
        const auto policies = frame.take("_publishing").toObject();
        const int defaultScope = frame.take("_default_scope").toInt(3);
        const auto eventIds = frame.take("_event_ids").toArray();
        const int normal = qBound(100, frame.take("_normal_ms").toInt(1000), 60000);
        const int fast = qBound(50, frame.take("_fast_ms").toInt(100), normal);
        QJsonArray rows;
        QSet<QString> present;
        for (auto value : frame["sensors"].toArray()) {
            auto row = value.toObject();
            const auto id = row["id"].toString();
            if (row["source"] == "beacn" && cloud && !frame["beacn"].toObject()["cloud"].toBool())
                continue;
            if (row["source"] == "custom" && cloud && !frame["custom"].toObject()["cloud"].toBool())
                continue;
            const auto policy = policies[id].toObject();
            const int mask = policy["scope"].toInt(defaultScope);
            if (!(mask & (cloud ? 2 : 1)))
                continue;
            present.insert(id);
            if (row["kind"] == "number" && row["value"].isDouble()) {
                const int digits = row["unit"] == "%" ? 2 : policy["decimals"].toInt(-1);
                if (digits >= 0 && digits <= 6) {
                    const double scale = std::pow(10.0, digits);
                    const auto number = row["value"].toDouble();
                    if (std::isfinite(number * scale))
                        row["value"] = std::round(number * scale) / scale;
                }
            }
            const bool immediate = (event && eventIds.contains(id)) || !values.contains(id) ||
                                   row["available"] != values[id]["available"] || row["kind"] != "number" ||
                                   row["value"].isNull();
            if (immediate || now - emitted.value(id) >= (policy["high"].toBool() ? fast : normal)) {
                if (values.value(id) != row) {
                    values[id] = row;
                    emitted[id] = now;
                }
            }
            if (values.value(id) != row) {
                const int due =
                    qMax(1, int((policy["high"].toBool() ? fast : normal) - (now - emitted.value(id))));
                if (!wake || due < wake)
                    wake = due;
            }
            rows.append(values.value(id, row));
        }
        for (const auto &id : values.keys())
            if (!present.contains(id)) {
                values.remove(id);
                emitted.remove(id);
            }
        // Media controls carry copies of sensor values; explicit per-sensor denials also apply there.
        auto controls = frame["controls"].toObject(), controlValues = controls["values"].toObject();
        for (const auto &id : controlValues.keys()) {
            const auto policy = policies[id].toObject();
            if (!(policy["scope"].toInt(1) & (cloud ? 2 : 1))) {
                controlValues.remove(id);
                continue;
            }
            if (values.contains(id))
                controlValues[id] = values[id]["value"];
            else if (id == "win_audio_volume" && controlValues[id].isDouble())
                controlValues[id] = std::round(controlValues[id].toDouble() * 100) / 100;
        }
        if (frame.contains("controls")) {
            controls["values"] = controlValues;
            frame["controls"] = controls;
        }
        auto lighting = frame["dynamic_lighting"].toObject();
        if (cloud && !lighting["cloud"].toBool())
            frame["dynamic_lighting"] = QJsonObject{
                {"enabled", false}, {"control_enabled", false}, {"cloud", false}, {"devices", QJsonArray{}}};
        if (cloud && !frame["beacn"].toObject()["cloud"].toBool())
            frame["beacn"] = QJsonObject{
                {"enabled", false}, {"connected", false}, {"ready", false}, {"control_enabled", false}};
        auto wallpaper = frame["wallpaper"].toObject();
        auto custom = frame["custom"].toObject();
        const auto cloudActions = custom.take("_cloud_actions").toArray();
        if (cloud) {
            QJsonArray actions;
            for (auto value : custom["actions"].toArray())
                if (cloudActions.contains(value.toObject()["id"]))
                    actions.append(value);
            custom["actions"] = actions;
        }
        if (frame.contains("custom"))
            frame["custom"] = custom;
        if (cloud && !frame["custom"].toObject()["cloud"].toBool())
            frame["custom"] = QJsonObject{{"enabled", false}, {"control_enabled", false},
                                          {"cloud", false}, {"actions", QJsonArray{}}};
        if (cloud && !wallpaper["cloud"].toBool())
            frame["wallpaper"] =
                QJsonObject{{"enabled", false}, {"running", false}, {"control_enabled", false}};
        frame["sensors"] = rows;
        return frame;
    }
};
