#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>

inline QString displaySpecifications(const QJsonObject &state) {
    QStringList lines{"Monitors"};
    if (state.contains("available") && !state["available"].toBool())
        return "Monitors\nWindows could not read the display configuration.";
    const auto monitors = state["monitors"].toArray();
    if (monitors.isEmpty())
        return lines.join('\n') + "\nNo connected monitors reported by Windows.";
    for (const auto &value : monitors) {
        const auto m = value.toObject();
        QString name = m["name"].toString("Monitor");
        const auto id = m["id"].toString();
        // Display-control IDs are useful in the editor, not in a shareable specs report.
        const auto suffix = QString::fromUtf8(" · ") + id.right(6);
        if (!id.isEmpty() && name.endsWith(suffix))
            name.chop(suffix.size());
        const bool active = m["enabled"].toBool();
        QStringList details{name, active ? "Enabled" : "Disabled"};
        if (active && m["primary"].toBool())
            details << "Primary";
        const int width = m["width"].toInt(), height = m["height"].toInt();
        if (width > 0 && height > 0) {
            details << QString("%1%2 × %3").arg(active ? "" : "Saved mode: ").arg(width).arg(height);
            const auto refresh = m["refresh"].toDouble();
            if (refresh > 0)
                details << QString::number(refresh, 'f', 2) + " Hz";
            const int rotation = m["rotation"].toInt(1);
            if (rotation >= 1 && rotation <= 4)
                details << QString("Rotation %1°").arg((rotation - 1) * 90);
        } else
            details << "Resolution unavailable";
        if (active && m["mode"].toString() == "mirrored")
            details << "Mirrored";
        lines << details.join(" | ");
        QStringList extra{"Connection: " + m["connection"].toString("Unavailable")};
        if (active) {
            extra << (m.contains("scalePercent") ? QString("Scaling: %1%").arg(m["scalePercent"].toInt())
                                                 : "Scaling: unavailable (" + m["scaleError"].toString("not reported") + ")");
            extra << (m.contains("bitsPerChannel")
                          ? QString("Colour depth: %1 bits per channel").arg(m["bitsPerChannel"].toInt())
                          : "Colour depth: unavailable (" + m["colourError"].toString("not reported") + ")");
            if (!m.contains("hdrSupported"))
                extra << "HDR support/state: unavailable (" + m["hdrError"].toString("not reported") + ")";
            else if (!m["hdrSupported"].toBool())
                extra << "HDR: not supported";
            else
                extra << (m["hdrEnabled"].toBool() ? "HDR: supported, on" : "HDR: supported, off");
        } else {
            extra << "Scaling, colour depth and HDR state: display inactive";
        }
        lines << "  " + extra.join(" | ");
    }
    return lines.join('\n');
}
