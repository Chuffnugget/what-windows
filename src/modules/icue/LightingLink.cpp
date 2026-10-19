#include "modules/icue/LightingLink.h"
#include <QColor>
#include <QMap>
#include <QSet>
#include <QRegularExpression>
#include <cmath>
#include <stdexcept>

namespace {
const QMap<QString, QStringList> parameters{
    {"StaticColor", {"Color1"}},
    {"RainbowWave", {"Speed", "Direction"}},
    {"Rainbow", {"Speed"}},
    {"ColorShift", {"Speed", "RandomColor", "Color1", "Color2"}},
    {"ColorWave", {"Speed", "Direction", "RandomColor", "Color1", "Color2"}},
    {"TypeLighting", {"Speed", "RandomColor", "Color1", "Color2"}},
    {"ColorPulse", {"Speed", "RandomColor", "Color1", "Color2"}},
    {"Rain", {"Speed", "RandomColor", "Color1", "Color2"}},
    {"Visor", {"Speed", "RandomColor", "Color1", "Color2"}},
    {"SpiralRainbow", {"Speed", "Rotation"}}};
const QMap<QString, QString> names{{"StaticColor", "Static colour"},
                                   {"RainbowWave", "Rainbow wave"},
                                   {"Rainbow", "Rainbow"},
                                   {"ColorShift", "Colour shift"},
                                   {"ColorWave", "Colour wave"},
                                   {"TypeLighting", "Type lighting"},
                                   {"ColorPulse", "Colour pulse"},
                                   {"Rain", "Rain"},
                                   {"Visor", "Visor"},
                                   {"SpiralRainbow", "Spiral rainbow"}};

bool integer(QJsonValue value, int maximum) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 &&
           value.toDouble() <= maximum && std::floor(value.toDouble()) == value.toDouble();
}

bool colour(QJsonValue value) {
    return value.isString() && QRegularExpression("^#[0-9a-fA-F]{6}$").match(value.toString()).hasMatch();
}

QJsonValue decoded(const QString &key, QJsonValue value) {
    if (key.startsWith("Color")) {
        if (!value.isDouble() || !std::isfinite(value.toDouble()) ||
            std::floor(value.toDouble()) != value.toDouble() || value.toDouble() < -2147483648.0 ||
            value.toDouble() > 4294967295.0)
            return QJsonValue(QJsonValue::Null);
        return QColor::fromRgba(quint32(qint64(value.toDouble()))).name();
    }
    if (key == "RandomColor") {
        if (value.isBool())
            return value;
        if (integer(value, 1))
            return value.toInt() != 0;
    } else if (integer(value, key == "Speed" ? 2 : key == "Direction" ? 3 : 1))
        return value;
    return QJsonValue(QJsonValue::Null);
}

QJsonObject effect(const QJsonObject &state, const QString &id) {
    for (auto v : state["effects"].toArray())
        if (v.toObject()["id"] == id)
            return v.toObject();
    return {};
}

void invalid() {
    throw std::runtime_error("Invalid Lighting Link response");
}
} // namespace

QJsonArray LightingLink::effects(const QJsonObject &reply) {
    if (!reply["items"].isArray() || reply["items"].toArray().size() > 32)
        invalid();
    QJsonArray result;
    QSet<QString> seen;
    for (auto v : reply["items"].toArray()) {
        const auto row = v.toObject();
        const auto id = row["type"].toString();
        if (!parameters.contains(id))
            continue;
        if (seen.contains(id) || !row["parameters"].isArray() || row["parameters"].toArray().size() > 16)
            invalid();
        seen.insert(id);
        QSet<QString> advertised;
        for (auto p : row["parameters"].toArray()) {
            if (!p.isString())
                invalid();
            advertised.insert(p.toString());
        }
        if (advertised.size() != row["parameters"].toArray().size())
            invalid();
        const auto expected = parameters[id];
        if (advertised != QSet<QString>(expected.begin(), expected.end()))
            continue;
        result.append(QJsonObject{
            {"id", id}, {"name", names[id]}, {"parameters", QJsonArray::fromStringList(expected)}});
    }
    return result;
}

QJsonObject LightingLink::state(const QJsonObject &reply, const QJsonArray &catalog, const QString &error) {
    QJsonObject result{{"available", false},
                       {"effect", QJsonValue::Null},
                       {"parameters", QJsonObject{}},
                       {"effects", catalog}};
    if (!error.isEmpty())
        return result;
    if (reply["_noActiveEffect"].toBool()) {
        result["available"] = true;
        return result;
    }
    const auto current = reply["effect"].toObject();
    const auto type = current["type"].toString();
    if (!parameters.contains(type) || !current["parameters"].isArray() ||
        current["parameters"].toArray().size() > 16)
        return result;
    QJsonObject values;
    QSet<QString> seen;
    for (auto item : current["parameters"].toArray()) {
        const auto p = item.toObject();
        const auto key = p["type"].toString();
        if (!parameters[type].contains(key))
            continue;
        if (seen.contains(key))
            return result;
        seen.insert(key);
        auto value = decoded(key, p["value"]);
        if (!value.isNull())
            values[key] = value;
    }
    result["available"] = true;
    result["effect"] = type;
    result["parameters"] = values;
    return result;
}

QJsonArray LightingLink::readings(const QJsonObject &state) {
    QJsonArray rows;
    const auto type = state["effect"].toString();
    const auto values = state["parameters"].toObject();
    auto add = [&](QString id, QString name, QJsonValue value, QString kind = "text") {
        const bool available = state["available"].toBool() && !value.isNull() && !value.isUndefined();
        rows.append(
            QJsonObject{{"id", "icue_automation_lighting_" + id},
                        {"source", "icue"},
                        {"icueAutomation", true},
                        {"categoryKey", "icue_automation_lighting"},
                        {"categoryName", "Lighting Link"},
                        {"group", "Lighting Link"},
                        {"name", name},
                        {"kind", kind},
                        {"unit", ""},
                        {"type", 0},
                        {"state_class", ""},
                        {"recommended", true},
                        {"value", available ? value : QJsonValue(QJsonValue::Null)},
                        {"available", available},
                        {"unavailable_reason",
                         available                      ? QString()
                         : !state["available"].toBool() ? "Lighting Link readback unavailable"
                         : type.isEmpty()               ? "No active Automation Lighting Link effect"
                                          : "iCUE did not report this parameter for the current effect"}});
    };
    add("effect", "Active Lighting Link effect", type.isEmpty() ? QString("None") : names.value(type, type));
    for (auto pair : {qMakePair(QString("Speed"), QString("speed")),
                      qMakePair(QString("Direction"), QString("direction")),
                      qMakePair(QString("Rotation"), QString("rotation"))})
        add(pair.second, "Lighting Link " + pair.second,
            values.contains(pair.first) ? QJsonValue(QString::number(values[pair.first].toInt()))
                                        : QJsonValue(QJsonValue::Null));
    add("colour_1", "Lighting Link colour 1", values["Color1"]);
    add("colour_2", "Lighting Link colour 2", values["Color2"]);
    add("random_colours", "Lighting Link random colours", values["RandomColor"], "binary");
    return rows;
}

QString LightingLink::validate(const QJsonObject &args, const QJsonObject &state) {
    if (!state["available"].toBool())
        return "Lighting Link is unavailable";
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!QStringList{"stop", "effect", "parameters", "expected_effect"}.contains(it.key()))
            return "Unknown Lighting Link argument";
    if (args.contains("stop"))
        return args.size() == 1 && args["stop"].isBool() && args["stop"].toBool()
                   ? QString()
                   : "Stop must be true and supplied alone";
    if (args.contains("effect") && !args["effect"].isString())
        return "Invalid Lighting Link effect";
    const auto id = args.contains("effect") ? args["effect"].toString() : state["effect"].toString();
    const auto target = effect(state, id);
    if (target.isEmpty())
        return "Lighting Link effect is no longer supported";
    if (args.contains("expected_effect") &&
        (!args["expected_effect"].isString() || args["expected_effect"] != state["effect"]))
        return "Lighting Link effect changed; refresh the controls";
    if (!args.contains("effect") && !args.contains("expected_effect"))
        return "Parameter changes require the current effect identity";
    if (args.contains("parameters") && !args["parameters"].isObject())
        return "Invalid Lighting Link parameters";
    const auto values = args["parameters"].toObject();
    if (!args.contains("effect") && values.isEmpty())
        return "No Lighting Link change supplied";
    for (auto it = values.begin(); it != values.end(); ++it) {
        const auto key = it.key();
        if (!target["parameters"].toArray().contains(key))
            return "Parameter is not supported by this effect";
        if (key.startsWith("Color") ? !colour(it.value())
            : key == "RandomColor"  ? !it.value().isBool()
                                    : !integer(it.value(), key == "Speed"       ? 2
                                                           : key == "Direction" ? 3
                                                                                : 1))
            return "Invalid Lighting Link parameter value";
    }
    return {};
}

QJsonObject LightingLink::activation(const QJsonObject &args, const QJsonObject &state) {
    const auto id = args.contains("effect") ? args["effect"].toString() : state["effect"].toString();
    QJsonObject values{{"Speed", 1},           {"Direction", 0},      {"Rotation", 0},
                       {"RandomColor", false}, {"Color1", "#ffffff"}, {"Color2", "#2060a0"}};
    if (state["effect"] == id) {
        const auto previous = state["parameters"].toObject();
        for (auto it = previous.begin(); it != previous.end(); ++it)
            values[it.key()] = it.value();
    }
    const auto changes = args["parameters"].toObject();
    for (auto it = changes.begin(); it != changes.end(); ++it)
        values[it.key()] = it.value();
    QJsonArray result;
    for (auto key : parameters.value(id)) {
        auto value = values[key];
        if (key.startsWith("Color"))
            value = double(qint32(QColor(value.toString()).rgba()));
        result.append(QJsonObject{{"type", key}, {"value", value}});
    }
    return {{"effect", QJsonObject{{"type", id}, {"parameters", result}}}};
}

bool LightingLink::matches(const QJsonObject &payload, const QJsonObject &actual) {
    auto expected = state(payload, {});
    return actual["available"].toBool() && actual["effect"] == expected["effect"] &&
           actual["parameters"] == expected["parameters"];
}
