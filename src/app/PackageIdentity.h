#pragma once
#include <windows.h>
#include <appmodel.h>
#include <QString>

inline QString packageFamily() {
    UINT32 size = 0;
    if (GetCurrentPackageFamilyName(&size, nullptr) != ERROR_INSUFFICIENT_BUFFER)
        return {};
    std::wstring value(size, L'\0');
    if (GetCurrentPackageFamilyName(&size, value.data()) != ERROR_SUCCESS)
        return {};
    return QString::fromWCharArray(value.c_str());
}
