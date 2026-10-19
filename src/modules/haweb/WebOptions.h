#pragma once
#include <QKeySequence>
#include <QUrl>
#include <QVariantMap>
#include <cmath>

// Shared by the live module and configuration import. Keep validation atomic so
// an invalid address cannot partially change the browser's saved preferences.
namespace WebOptions {
inline constexpr int defaultScale = 80;
inline QVariantMap defaults() {
    return {{"cloudUrl", ""},         {"fallback", false},         {"autoRecover", true},
            {"desktopDashboard", ""}, {"fullscreenDashboard", ""}, {"pipDashboard", ""},
            {"desktopMonitor", ""},   {"overlayMonitor", ""},      {"autoHideToolbar", false},
            {"pipSnap", true},        {"pipAspectRatio", "free"},  {"muted", false},
            {"keepAudio", false},     {"pipHotkey", "Ctrl+Alt+P"}, {"trayAction", "what"},
            {"pipScale", defaultScale}, {"pipWindowScale", 100}, {"sidebarWidth", 256}};
}

inline bool validHotkey(const QString &text) {
    if (text.isEmpty())
        return true;
    const QKeySequence sequence(text, QKeySequence::PortableText);
    if (text.size() > 80 || sequence.count() != 1)
        return false;
    const auto key = sequence[0].key();
    const auto modifiers = sequence[0].keyboardModifiers();
    return (modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) &&
           ((key >= Qt::Key_A && key <= Qt::Key_Z) || (key >= Qt::Key_0 && key <= Qt::Key_9) ||
            (key >= Qt::Key_F1 && key <= Qt::Key_F24) || key == Qt::Key_Space);
}

inline bool validUrl(const QString &text) {
    const QUrl url(text, QUrl::StrictMode);
    return text.isEmpty() || (text.size() <= 2048 && !text.contains(QChar(0)) && url.isValid() &&
                              (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty() &&
                              url.userInfo().isEmpty());
}

inline bool valid(const QVariantMap &values) {
    const auto known = defaults();
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        // JSON imports use doubles, QSettings uses ints. Accept whole numbers
        // from either representation, but never coerce text or booleans.
        if (it.key() == "pipScale" || it.key() == "pipWindowScale" || it.key() == "sidebarWidth") {
            const auto type = it.value().metaType().id();
            if (type != QMetaType::Int && type != QMetaType::LongLong && type != QMetaType::Double)
                return false;
            const double number = it.value().toDouble();
            const bool sidebar = it.key() == "sidebarWidth";
            if (!std::isfinite(number) || std::floor(number) != number ||
                number < (sidebar ? 180 : 50) || number > (sidebar ? 420 : 200))
                return false;
            continue;
        }
        if (!known.contains(it.key()) || it.value().metaType() != known[it.key()].metaType())
            return false;
        if (it.value().metaType().id() == QMetaType::QString) {
            const auto text = it.value().toString();
            if (text.size() > 2048 || text.contains(QChar(0)))
                return false;
            if (it.key() == "cloudUrl" &&
                (!validUrl(text) || (!text.isEmpty() && QUrl(text).scheme() != "https")))
                return false;
            if (it.key().endsWith("Dashboard") && !validUrl(text)) {
                const QUrl path(text, QUrl::StrictMode);
                if (!text.startsWith('/') || text.startsWith("//") || text.contains('\\') ||
                    !path.isValid() || !path.isRelative() || !path.authority().isEmpty())
                    return false;
            }
            if (it.key() == "pipAspectRatio" && !QStringList{"free", "16:9", "4:3", "1:1"}.contains(text))
                return false;
            if (it.key() == "pipHotkey" && !validHotkey(text))
                return false;
            if (it.key() == "trayAction" &&
                !QStringList{"what", "windowed", "fullscreen", "pip", "desktop"}.contains(text))
                return false;
        }
    }
    return true;
}

inline QVariantMap merged(const QVariantMap &values) {
    auto result = defaults();
    if (valid(values))
        for (auto it = values.cbegin(); it != values.cend(); ++it)
            result[it.key()] = it.value();
    return result;
}

inline QString origin(const QString &address) {
    if (address.isEmpty() || !validUrl(address))
        return {};
    QUrl url(address);
    url.setPath({});
    url.setQuery({});
    url.setFragment({});
    if ((url.scheme() == "https" && url.port() == 443) || (url.scheme() == "http" && url.port() == 80))
        url.setPort(-1);
    return url.toString();
}

inline QString dashboard(const QString &base, const QString &preset) {
    return preset.isEmpty() ? base : QUrl(base).resolved(QUrl(preset)).toString();
}
} // namespace WebOptions
