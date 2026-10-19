#include "MediaSessions.h"
#include <QJsonDocument>
#include <QScopeGuard>
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Media.h>
#include <algorithm>
#include <chrono>

namespace {
using namespace winrt;
using namespace Windows::Data::Json;
using namespace Windows::Media::Control;

template <class Operation> auto awaitMedia(Operation operation) {
    if (operation.wait_for(std::chrono::seconds(2)) != Windows::Foundation::AsyncStatus::Completed) {
        operation.Cancel();
        throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    return operation.GetResults();
}

JsonValue text(hstring value) {
    auto s = std::wstring(value);
    if (s.size() > 255)
        s.resize(255);
    return JsonValue::CreateStringValue(s);
}

void add(JsonArray &rows, const wchar_t *id, const wchar_t *group, const wchar_t *name, IJsonValue value,
         const wchar_t *kind = L"text") {
    JsonObject row;
    row.SetNamedValue(L"id", text(id));
    row.SetNamedValue(L"source", text(L"windows"));
    row.SetNamedValue(L"group", text(group));
    row.SetNamedValue(L"name", text(name));
    row.SetNamedValue(L"value", value);
    row.SetNamedValue(L"kind", text(kind));
    row.SetNamedValue(L"type", JsonValue::CreateNumberValue(0));
    row.SetNamedValue(L"unit", text(L""));
    row.SetNamedValue(L"available", JsonValue::CreateBooleanValue(value.ValueType() != JsonValueType::Null));
    row.SetNamedValue(L"state_class", text(kind == std::wstring(L"number") ? L"measurement" : L""));
    rows.Append(row);
}
} // namespace

QJsonArray readMediaSessions(bool includeTitles) {
    // All WinRT objects are released before this worker's COM apartment closes.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto cleanup = qScopeGuard([] { winrt::uninit_apartment(); });
    JsonArray rows;
    {
        try {
            auto manager = awaitMedia(GlobalSystemMediaTransportControlsSessionManager::RequestAsync());
            auto session = manager.GetCurrentSession();
            add(rows, L"win_media_status", L"Media", L"Collection status",
                text(session ? L"Ready" : L"No media session"));
            add(rows, L"win_media_app", L"Media", L"Current media application",
                session ? text(session.SourceAppUserModelId()) : JsonValue::CreateNullValue());
            hstring state = L"No session";
            if (session)
                switch (session.GetPlaybackInfo().PlaybackStatus()) {
                case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing:
                    state = L"Playing";
                    break;
                case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Paused:
                    state = L"Paused";
                    break;
                case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Stopped:
                    state = L"Stopped";
                    break;
                case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Closed:
                    state = L"Closed";
                    break;
                case GlobalSystemMediaTransportControlsSessionPlaybackStatus::Changing:
                    state = L"Changing";
                    break;
                default:
                    state = L"Opened";
                }
            add(rows, L"win_media_state", L"Media", L"Playback state", text(state));
            if (session) {
                auto playback = session.GetPlaybackInfo();
                auto controls = playback.Controls();
                auto timeline = session.GetTimelineProperties();
                add(rows, L"win_media_can_play", L"Media", L"Play supported",
                    JsonValue::CreateBooleanValue(controls.IsPlayEnabled()), L"binary");
                add(rows, L"win_media_can_pause", L"Media", L"Pause supported",
                    JsonValue::CreateBooleanValue(controls.IsPauseEnabled()), L"binary");
                add(rows, L"win_media_can_stop", L"Media", L"Stop supported",
                    JsonValue::CreateBooleanValue(controls.IsStopEnabled()), L"binary");
                add(rows, L"win_media_can_next", L"Media", L"Next supported",
                    JsonValue::CreateBooleanValue(controls.IsNextEnabled()), L"binary");
                add(rows, L"win_media_can_previous", L"Media", L"Previous supported",
                    JsonValue::CreateBooleanValue(controls.IsPreviousEnabled()), L"binary");
                auto position = timeline.Position().count();
                if (playback.PlaybackStatus() ==
                    GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
                    position +=
                        std::max<int64_t>(0, (winrt::clock::now() - timeline.LastUpdatedTime()).count());
                if (timeline.EndTime().count() > 0)
                    position = std::min(position, timeline.EndTime().count());
                add(rows, L"win_media_position", L"Media", L"Playback position",
                    JsonValue::CreateNumberValue(double(std::max<int64_t>(0, position)) / 10000000),
                    L"number");
                add(rows, L"win_media_duration", L"Media", L"Duration",
                    JsonValue::CreateNumberValue(double(std::max<int64_t>(0, timeline.EndTime().count())) /
                                                 10000000),
                    L"number");
                add(rows, L"win_media_can_seek", L"Media", L"Seek supported",
                    JsonValue::CreateBooleanValue(controls.IsPlaybackPositionEnabled()), L"binary");
                add(rows, L"win_media_can_shuffle", L"Media", L"Shuffle supported",
                    JsonValue::CreateBooleanValue(controls.IsShuffleEnabled()), L"binary");
                add(rows, L"win_media_can_repeat", L"Media", L"Repeat supported",
                    JsonValue::CreateBooleanValue(controls.IsRepeatEnabled()), L"binary");
                auto shuffle = playback.IsShuffleActive();
                add(rows, L"win_media_shuffle", L"Media", L"Shuffle",
                    shuffle ? JsonValue::CreateBooleanValue(shuffle.Value()) : JsonValue::CreateNullValue(),
                    L"binary");
                auto repeat = playback.AutoRepeatMode();
                using Windows::Media::MediaPlaybackAutoRepeatMode;
                add(rows, L"win_media_repeat", L"Media", L"Repeat",
                    text(!repeat || repeat.Value() == MediaPlaybackAutoRepeatMode::None ? L"off"
                         : repeat.Value() == MediaPlaybackAutoRepeatMode::Track         ? L"one"
                                                                                        : L"all"));
            }
            if (includeTitles) {
                if (session) {
                    auto properties = awaitMedia(session.TryGetMediaPropertiesAsync());
                    add(rows, L"win_media_title", L"Media", L"Track title", text(properties.Title()));
                    add(rows, L"win_media_artist", L"Media", L"Artist", text(properties.Artist()));
                } else {
                    add(rows, L"win_media_title", L"Media", L"Track title", JsonValue::CreateNullValue());
                    add(rows, L"win_media_artist", L"Media", L"Artist", JsonValue::CreateNullValue());
                }
            }
        } catch (hresult_error const &) {
            rows = JsonArray{};
            add(rows, L"win_media_status", L"Media", L"Collection status",
                text(L"Media API unavailable or access denied"));
        }
    }
    const auto json = winrt::to_string(rows.Stringify());
    return QJsonDocument::fromJson(QByteArray::fromStdString(json)).array();
}
