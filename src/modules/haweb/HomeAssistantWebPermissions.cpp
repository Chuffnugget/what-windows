#include "HomeAssistantWebState.h"
#include <QSet>
#include <algorithm>

namespace {
ComPtr<ICoreWebView2Profile4> permissionProfile(ICoreWebView2 *browser) {
    ComPtr<ICoreWebView2_13> web;
    ComPtr<ICoreWebView2Profile> profile;
    ComPtr<ICoreWebView2Profile4> permissions;
    if (browser && SUCCEEDED(browser->QueryInterface(IID_PPV_ARGS(&web))) &&
        SUCCEEDED(web->get_Profile(&profile)))
        profile.As(&permissions);
    return permissions;
}

QString permissionName(int kind) {
    const QStringList names{"Unknown",           "Microphone",          "Camera",
                            "Location",          "Notifications",       "Other Sensors",
                            "Read Clipboard",    "Automatic Downloads", "File Access",
                            "Autoplay",          "Local Fonts",         "MIDI System Messages",
                            "Window Management", "Persistent Storage"};
    return kind > 0 && kind < names.size() ? names[kind] : QString("Permission %1").arg(kind);
}

QVariantMap permissionRow(const QString &origin, int kind, int state) {
    return {{"origin", origin}, {"kind", kind}, {"label", permissionName(kind)}, {"state", state}};
}
} // namespace

QVariantList HomeAssistantWeb::permissions() const {
    return d->permissions;
}

bool HomeAssistantWeb::permissionsBusy() const {
    return d->permissionBusy;
}

QString HomeAssistantWeb::permissionsStatus() const {
    return d->permissionStatus;
}

void HomeAssistantWeb::refreshPermissions() {
    if (d->permissionBusy || d->clearing)
        return;
    const auto profile = permissionProfile(d->web.Get());
    if (!profile) {
        d->permissionStatus = d->web ? "Update the WebView2 Runtime to manage site permissions."
                                     : "Open the dashboard, then refresh to inspect site permissions.";
        emit permissionsChanged();
        return;
    }
    d->permissionBusy = true;
    d->permissionStatus = "Reading site permissions…";
    emit permissionsChanged();
    const QPointer<HomeAssistantWeb> owner(this);
    const auto generation = d->generation;
    const auto result = profile->GetNonDefaultPermissionSettings(
        Callback<ICoreWebView2GetNonDefaultPermissionSettingsCompletedHandler>(
            [owner, generation](HRESULT error,
                                ICoreWebView2PermissionSettingCollectionView *settings) -> HRESULT {
                if (!owner || owner->d->generation != generation)
                    return S_OK;
                owner->d->permissionBusy = false;
                if (FAILED(error) || !settings) {
                    owner->d->permissionStatus =
                        "Site permissions could not be read. Try Refresh Permissions.";
                    emit owner->permissionsChanged();
                    return S_OK;
                }
                QVariantList rows;
                QSet<QString> seen;
                UINT32 count = 0;
                settings->get_Count(&count);
                for (UINT32 index = 0; index < count; ++index) {
                    ComPtr<ICoreWebView2PermissionSetting> setting;
                    if (FAILED(settings->GetValueAtIndex(index, &setting)) || !setting)
                        continue;
                    LPWSTR raw = nullptr;
                    COREWEBVIEW2_PERMISSION_KIND kind{};
                    COREWEBVIEW2_PERMISSION_STATE state{};
                    setting->get_PermissionOrigin(&raw);
                    const auto origin = WebOptions::origin(takeWebString(raw));
                    setting->get_PermissionKind(&kind);
                    setting->get_PermissionState(&state);
                    if (origin.isEmpty())
                        continue;
                    rows.append(permissionRow(origin, int(kind), int(state)));
                    seen.insert(origin + '/' + QString::number(int(kind)));
                }
                // Common permissions remain editable before the site first asks.
                QSet<QString> origins{WebOptions::origin(owner->d->address),
                                      WebOptions::origin(owner->d->source),
                                      WebOptions::origin(owner->d->options["cloudUrl"].toString())};
                for (const auto *key : {"desktopDashboard", "fullscreenDashboard", "pipDashboard"})
                    origins.insert(WebOptions::origin(owner->d->options[key].toString()));
                for (const auto &origin : origins) {
                    if (origin.isEmpty())
                        continue;
                    for (const auto kind :
                         {COREWEBVIEW2_PERMISSION_KIND_MICROPHONE, COREWEBVIEW2_PERMISSION_KIND_CAMERA,
                          COREWEBVIEW2_PERMISSION_KIND_GEOLOCATION,
                          COREWEBVIEW2_PERMISSION_KIND_NOTIFICATIONS,
                          COREWEBVIEW2_PERMISSION_KIND_CLIPBOARD_READ})
                        if (!seen.contains(origin + '/' + QString::number(int(kind))))
                            rows.append(
                                permissionRow(origin, int(kind), int(COREWEBVIEW2_PERMISSION_STATE_DEFAULT)));
                }
                std::sort(rows.begin(), rows.end(), [](const QVariant &a, const QVariant &b) {
                    const auto left = a.toMap(), right = b.toMap();
                    return left["origin"] == right["origin"]
                               ? left["kind"].toInt() < right["kind"].toInt()
                               : left["origin"].toString() < right["origin"].toString();
                });
                owner->d->permissions = rows;
                owner->d->permissionStatus =
                    "Ask uses the site's normal permission prompt. Changes are saved in this browser profile; reload the dashboard to apply them to an open page.";
                emit owner->permissionsChanged();
                return S_OK;
            })
            .Get());
    if (FAILED(result)) {
        d->permissionBusy = false;
        d->permissionStatus = "Site permissions could not be read.";
        emit permissionsChanged();
    }
}

void HomeAssistantWeb::setPermission(const QString &origin, int kind, int state) {
    if (d->permissionBusy || d->clearing || state < 0 || state > 2)
        return;
    for (const auto &value : d->permissions) {
        const auto row = value.toMap();
        if (row["origin"] == origin && row["kind"].toInt() == kind) {
            d->permissionQueue = {permissionRow(origin, kind, state)};
            applyNextPermission();
            return;
        }
    }
}

void HomeAssistantWeb::resetPermissions() {
    if (d->permissionBusy || d->clearing)
        return;
    for (const auto &value : d->permissions) {
        auto row = value.toMap();
        if (row["state"].toInt() == int(COREWEBVIEW2_PERMISSION_STATE_DEFAULT))
            continue;
        row["state"] = int(COREWEBVIEW2_PERMISSION_STATE_DEFAULT);
        d->permissionQueue.append(row);
    }
    applyNextPermission();
}

void HomeAssistantWeb::applyNextPermission() {
    if (d->permissionQueue.isEmpty()) {
        d->permissionBusy = false;
        refreshPermissions();
        return;
    }
    const auto profile = permissionProfile(d->web.Get());
    if (!profile)
        return;
    d->permissionBusy = true;
    d->permissionStatus = "Saving site permissions…";
    emit permissionsChanged();
    const auto row = d->permissionQueue.takeFirst().toMap();
    const QPointer<HomeAssistantWeb> owner(this);
    const auto generation = d->generation;
    auto complete = [owner, generation](HRESULT error) -> HRESULT {
        if (!owner || owner->d->generation != generation)
            return S_OK;
        if (FAILED(error)) {
            owner->d->permissionBusy = false;
            owner->d->permissionQueue.clear();
            owner->d->permissionStatus = "The permission could not be saved. Refresh and try again.";
            emit owner->permissionsChanged();
        } else {
            QTimer::singleShot(0, owner, [owner, generation] {
                if (owner && owner->d->generation == generation)
                    owner->applyNextPermission();
            });
        }
        return S_OK;
    };
    const auto result = profile->SetPermissionState(
        COREWEBVIEW2_PERMISSION_KIND(row["kind"].toInt()), row["origin"].toString().toStdWString().c_str(),
        COREWEBVIEW2_PERMISSION_STATE(row["state"].toInt()),
        Callback<ICoreWebView2SetPermissionStateCompletedHandler>(complete).Get());
    if (FAILED(result))
        complete(result);
}
