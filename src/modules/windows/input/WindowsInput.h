#pragma once
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QMap>
#include <QVector>
#include <cmath>
#include <windows.h>
#include <QScopeGuard>

namespace WindowsInput {
inline WORD key(const QString &name) {
    const auto n = name.toUpper();
    if (n.size() == 1 && ((n[0] >= 'A' && n[0] <= 'Z') || (n[0] >= '0' && n[0] <= '9')))
        return n[0].unicode();
    static const QMap<QString, WORD> named{
        {"CTRL", VK_CONTROL},   {"ALT", VK_MENU},      {"SHIFT", VK_SHIFT},   {"WIN", VK_LWIN},
        {"ENTER", VK_RETURN},   {"TAB", VK_TAB},       {"ESC", VK_ESCAPE},    {"SPACE", VK_SPACE},
        {"BACKSPACE", VK_BACK}, {"DELETE", VK_DELETE}, {"INSERT", VK_INSERT}, {"HOME", VK_HOME},
        {"END", VK_END},        {"PAGEUP", VK_PRIOR},  {"PAGEDOWN", VK_NEXT}, {"LEFT", VK_LEFT},
        {"RIGHT", VK_RIGHT},    {"UP", VK_UP},         {"DOWN", VK_DOWN},     {"PRINTSCREEN", VK_SNAPSHOT}};
    if (named.contains(n))
        return named[n];
    bool ok = false;
    int f = n.mid(1).toInt(&ok);
    if (n.startsWith('F') && ok && f >= 1 && f <= 24)
        return WORD(VK_F1 + f - 1);
    return 0;
}

inline bool integer(const QJsonValue &v, int low, int high) {
    return v.isDouble() && std::isfinite(v.toDouble()) && std::floor(v.toDouble()) == v.toDouble() &&
           v.toDouble() >= low && v.toDouble() <= high;
}

inline QString validate(const QString &action, const QJsonObject &a) {
    if (action == "keyboard") {
        if (a.size() != 1 || (!a.contains("keys") && !a.contains("text")))
            return "Provide either keys or text";
        if (a.contains("text")) {
            if (!a["text"].isString() || a["text"].toString().isEmpty() ||
                a["text"].toString().size() > 1024 || a["text"].toString().contains(QChar::Null))
                return "Text must contain 1-1024 UTF-16 units without NUL";
            const auto text = a["text"].toString();
            for (int i = 0; i < text.size(); ++i) {
                if (text[i].isHighSurrogate()) {
                    if (++i >= text.size() || !text[i].isLowSurrogate())
                        return "Invalid Unicode text";
                } else if (text[i].isLowSurrogate())
                    return "Invalid Unicode text";
            }
        } else {
            if (!a["keys"].isArray() || a["keys"].toArray().isEmpty() || a["keys"].toArray().size() > 6)
                return "Provide 1-6 key names";
            QSet<WORD> seen;
            for (const auto &v : a["keys"].toArray()) {
                WORD k = key(v.toString());
                if (!v.isString() || !k || seen.contains(k))
                    return "Unknown or duplicate key";
                seen.insert(k);
            }
        }
        return {};
    }
    if (action != "mouse")
        return "Unknown input action";
    for (auto it = a.begin(); it != a.end(); ++it)
        if (!QStringList{"x", "y", "button", "clicks"}.contains(it.key()))
            return "Unknown mouse argument";
    if (!integer(a["x"], -100000, 100000) || !integer(a["y"], -100000, 100000))
        return "x and y must be integer desktop coordinates";
    if (a.contains("button") &&
        (!a["button"].isString() ||
         !QStringList{"none", "left", "right", "middle"}.contains(a["button"].toString())))
        return "Invalid mouse button";
    if (a.contains("clicks") && !integer(a["clicks"], 1, 2))
        return "Click count must be 1 or 2";
    if (a["button"].toString("none") == "none" && a.contains("clicks"))
        return "Clicks require a button";
    return {};
}

inline bool extended(WORD k) {
    return k == VK_LWIN || k == VK_INSERT || k == VK_DELETE || k == VK_HOME || k == VK_END || k == VK_PRIOR ||
           k == VK_NEXT || k == VK_LEFT || k == VK_RIGHT || k == VK_UP || k == VK_DOWN || k == VK_SNAPSHOT;
}

inline QString execute(const QString &action, const QJsonObject &args) {
    const auto error = validate(action, args);
    if (!error.isEmpty())
        return error;
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop)
        return "Input desktop unavailable; unlock Windows first";
    auto close = qScopeGuard([&] { CloseDesktop(desktop); });
    wchar_t name[128]{};
    DWORD length = 0;
    if (!GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &length) ||
        QString::fromWCharArray(name).compare("Default", Qt::CaseInsensitive) != 0)
        return "Keyboard/mouse input is unavailable on the lock or secure desktop";
    for (int k : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN, VK_LBUTTON, VK_RBUTTON, VK_MBUTTON})
        if (GetAsyncKeyState(k) & 0x8000)
            return "Release locally held modifiers and mouse buttons before sending input";
    QVector<INPUT> events, releases;
    auto addKey = [&](WORD k, DWORD flags, bool unicode = false) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        if (unicode)
            input.ki.wScan = k;
        else
            input.ki.wVk = k;
        input.ki.dwFlags = flags;
        events.append(input);
    };
    if (action == "keyboard") {
        if (args.contains("text")) {
            for (auto c : args["text"].toString()) {
                addKey(c.unicode(), KEYEVENTF_UNICODE, true);
                addKey(c.unicode(), KEYEVENTF_UNICODE | KEYEVENTF_KEYUP, true);
            }
        } else {
            QVector<WORD> keys;
            for (auto v : args["keys"].toArray()) {
                auto k = key(v.toString());
                if (GetAsyncKeyState(k) & 0x8000)
                    return "A requested key is already held locally";
                keys.append(k);
                addKey(k, extended(k) ? KEYEVENTF_EXTENDEDKEY : 0);
            }
            for (auto it = keys.crbegin(); it != keys.crend(); ++it)
                addKey(*it, KEYEVENTF_KEYUP | (extended(*it) ? KEYEVENTF_EXTENDEDKEY : 0));
        }
    } else {
        auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        auto restore = qScopeGuard([&] {
            if (previous)
                SetThreadDpiAwarenessContext(previous);
        });
        const int left = GetSystemMetrics(SM_XVIRTUALSCREEN), top = GetSystemMetrics(SM_YVIRTUALSCREEN),
                  width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN),
                  x = args["x"].toInt(), y = args["y"].toInt();
        if (width <= 0 || height <= 0 || x < left || x >= left + width || y < top || y >= top + height)
            return "Coordinates are outside the virtual desktop";
        POINT point{x, y};
        if (!MonitorFromPoint(point, MONITOR_DEFAULTTONULL))
            return "Coordinates fall in a gap between monitors";
        INPUT move{};
        move.type = INPUT_MOUSE;
        move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        move.mi.dx = LONG((double(x - left) + 0.5) * 65536 / width);
        move.mi.dy = LONG((double(y - top) + 0.5) * 65536 / height);
        events.append(move);
        const auto button = args["button"].toString("none");
        if (button != "none") {
            DWORD down = button == "left"    ? MOUSEEVENTF_LEFTDOWN
                         : button == "right" ? MOUSEEVENTF_RIGHTDOWN
                                             : MOUSEEVENTF_MIDDLEDOWN;
            DWORD up = button == "left"    ? MOUSEEVENTF_LEFTUP
                       : button == "right" ? MOUSEEVENTF_RIGHTUP
                                           : MOUSEEVENTF_MIDDLEUP;
            for (int i = 0; i < args["clicks"].toInt(1); ++i) {
                INPUT click{};
                click.type = INPUT_MOUSE;
                click.mi.dwFlags = down;
                events.append(click);
                click.mi.dwFlags = up;
                events.append(click);
            }
        }
    }
    const UINT sent = SendInput(UINT(events.size()), events.data(), sizeof(INPUT));
    if (sent != UINT(events.size())) {
        // Release only the keys/buttons whose down events were actually inserted.
        for (UINT i = 0; i < sent; ++i) {
            auto e = events[i];
            if (e.type == INPUT_KEYBOARD && !(e.ki.dwFlags & KEYEVENTF_KEYUP)) {
                e.ki.dwFlags |= KEYEVENTF_KEYUP;
                releases.prepend(e);
            } else if (e.type == INPUT_MOUSE) {
                DWORD up = e.mi.dwFlags == MOUSEEVENTF_LEFTDOWN     ? MOUSEEVENTF_LEFTUP
                           : e.mi.dwFlags == MOUSEEVENTF_RIGHTDOWN  ? MOUSEEVENTF_RIGHTUP
                           : e.mi.dwFlags == MOUSEEVENTF_MIDDLEDOWN ? MOUSEEVENTF_MIDDLEUP
                                                                    : 0;
                if (up) {
                    e.mi.dwFlags = up;
                    releases.append(e);
                }
            }
        }
        if (!releases.isEmpty())
            SendInput(UINT(releases.size()), releases.data(), sizeof(INPUT));
        return "Windows rejected some input (possibly an elevated window); action was not retried";
    }
    return {};
}
} // namespace WindowsInput
