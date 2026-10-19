#pragma once
#include <windows.h>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

namespace WindowsSecurity {
// WscGetSecurityProviderHealth uses a DWORD category mask and a 32-bit health enum.
// Resolve the system DLL because MinGW does not ship wscapi.h/import libraries.
using Query = HRESULT(WINAPI *)(DWORD, int *);

inline QJsonArray read(Query query) {
    struct Category {
        DWORD mask;
        const char *id;
        const char *name;
    };

    QJsonArray rows;
    for (const auto &category :
         {Category{4, "antivirus", "Antivirus health"}, Category{1, "firewall", "Firewall health"},
          Category{2, "updates", "Automatic update settings health"}}) {
        int health = -1;
        const HRESULT result = query ? query(category.mask, &health) : E_NOTIMPL;
        const bool available = result == S_OK && health >= 0 && health <= 3;
        const QStringList states{"Good", "Not monitored", "Poor", "Snoozed"};
        QJsonObject row{{"id", "win_security_" + QString(category.id)},
                        {"source", "windows"},
                        {"group", "Windows Security"},
                        {"name", category.name},
                        {"value", available ? QJsonValue(states[health]) : QJsonValue(QJsonValue::Null)},
                        {"kind", "text"},
                        {"unit", ""},
                        {"type", 0},
                        {"available", available},
                        {"recommended", false},
                        {"state_class", ""}};
        if (!available)
            row["unavailable_reason"] = result == S_FALSE ? "Windows Security Center service is not running"
                                                          : "Windows Security Center status is unavailable";
        rows.append(row);
    }
    return rows;
}

inline QJsonArray read() {
    const auto library = LoadLibraryExW(L"wscapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto query =
        library ? reinterpret_cast<Query>(GetProcAddress(library, "WscGetSecurityProviderHealth")) : nullptr;
    const auto rows = read(query);
    if (library)
        FreeLibrary(library);
    return rows;
}
} // namespace WindowsSecurity
