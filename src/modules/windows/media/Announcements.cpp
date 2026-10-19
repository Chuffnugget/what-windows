#include "Announcements.h"
#include <QUrl>
#include <QVariantList>
#include <windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Core.h>
#include <winrt/Windows.Media.Playback.h>
#include <winrt/Windows.Media.SpeechSynthesis.h>
#include <winrt/Windows.Storage.Streams.h>
#include <cmath>
#include <chrono>

namespace {
using namespace winrt;
using namespace Windows::Media::Playback;
using namespace Windows::Media::SpeechSynthesis;
using namespace Windows::Media::Core;

bool text(const QJsonValue &value, int length) {
    return value.isString() && !value.toString().trimmed().isEmpty() && value.toString().size() <= length &&
           !value.toString().contains(QChar::Null);
}

bool number(const QJsonValue &value, double low, double high) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= low &&
           value.toDouble() <= high;
}

hstring nativeText(const QString &value) {
    return hstring(reinterpret_cast<const wchar_t *>(value.utf16()), value.size());
}

QString qtText(const hstring &value) {
    return QString::fromWCharArray(value.data(), int(value.size()));
}
} // namespace

struct Announcements::Native {
    MediaPlayer player{nullptr};
    SpeechSynthesizer speech{nullptr};
    bool initialized = false;
    bool stopped = true;
    bool ready = false;
    QString initializing;
    QJsonArray voices;

    struct Events {
        std::atomic_bool ended{false};
        std::atomic_int failure{0};
    };

    std::shared_ptr<Events> events = std::make_shared<Events>();

    void initialize() {
        if (ready)
            return;
        if (!initialized) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized = true;
        }
        initializing = "Windows audio player";
        player = MediaPlayer();
        player.AutoPlay(false);
        player.CommandManager().IsEnabled(false);
        player.Volume(0.7);
        initializing = "Windows speech provider";
        voices = {};
        try {
            for (const auto &voice : SpeechSynthesizer::AllVoices())
                voices.append(QJsonObject{{"id", qtText(voice.Id())},
                                          {"name", qtText(voice.DisplayName())},
                                          {"language", qtText(voice.Language())}});
        } catch (const winrt::hresult_error &) {
            // Audio URLs still work on PCs without a working speech provider.
            // A speech request reports the provider error when it is attempted.
        }
        player.MediaEnded([events = events](const auto &, const auto &) { events->ended.store(true); });
        player.MediaFailed([events = events](const auto &, const MediaPlayerFailedEventArgs &event) {
            events->failure.store(event.ExtendedErrorCode().value);
        });
        ready = true;
        initializing.clear();
    }

    ~Native() {
        try {
            if (player)
                player.Close();
            if (speech)
                speech.Close();
        } catch (...) {
            // OS audio teardown must not turn a normal shutdown into a crash.
        }
        player = nullptr;
        speech = nullptr;
        if (initialized)
            winrt::uninit_apartment();
    }

    QJsonObject snapshot() try {
        initialize();
        QString state = "idle";
        if (!stopped && !events->ended.load()) {
            switch (player.PlaybackSession().PlaybackState()) {
            case MediaPlaybackState::Playing:
                state = "playing";
                break;
            case MediaPlaybackState::Paused:
                state = "paused";
                break;
            case MediaPlaybackState::Opening:
            case MediaPlaybackState::Buffering:
                state = "buffering";
                break;
            default:
                break;
            }
        }
        const auto failure = events->failure.load();
        return {{"state", failure ? "idle" : state},
                {"volume", player.Volume()},
                {"muted", player.IsMuted()},
                {"voices", voices},
                {"error", failure ? QString("Windows could not play this audio (0x%1)")
                                        .arg(quint32(failure), 8, 16, QChar('0'))
                                  : QString()}};
    } catch (const winrt::hresult_error &error) {

        return {{"state", "idle"},
                {"volume", 0.7},
                {"muted", false},
                {"voices", QJsonArray{}},
                {"error", initializing +
                              QString(" failed (0x%1)").arg(quint32(error.code().value), 8, 16, QChar('0'))}};
    }

    QString run(const QString &action, const QJsonObject &args, const std::function<bool()> &cancelled) try {
        if (cancelled())
            return "Announcement cancelled before playback";
        initialize();
        if (action == "speak" || action == "play_audio") {
            events->ended.store(false);
            events->failure.store(0);
        }
        if (action == "speak") {
            if (!speech)
                speech = SpeechSynthesizer();
            const auto selected = args["voice"].toString();
            auto voice = SpeechSynthesizer::DefaultVoice();
            bool found = selected.isEmpty();
            for (auto candidate : SpeechSynthesizer::AllVoices()) {
                if (qtText(candidate.Id()) == selected) {
                    voice = candidate;
                    found = true;
                    break;
                }
            }
            if (!found)
                return "The requested Windows voice is not installed";
            speech.Voice(voice);
            speech.Options().SpeakingRate(args["rate"].toDouble(1.0));
            auto operation = speech.SynthesizeTextToStreamAsync(nativeText(args["message"].toString()));
            // Speech synthesis can stall in an OS voice provider. Keep cancellation
            // and shutdown bounded, while the GUI continues rendering normally.
            for (int attempt = 0; attempt < 100; ++attempt) {
                if (cancelled()) {
                    operation.Cancel();
                    return "Speech synthesis cancelled";
                }
                if (operation.wait_for(std::chrono::milliseconds(100)) !=
                    Windows::Foundation::AsyncStatus::Started)
                    break;
            }
            if (operation.Status() == Windows::Foundation::AsyncStatus::Started) {
                operation.Cancel();
                return "Windows speech synthesis timed out";
            }
            const auto stream = operation.GetResults();
            if (cancelled())
                return "Speech synthesis cancelled";
            player.Source(MediaSource::CreateFromStream(stream, stream.ContentType()));
            player.Volume(args["volume"].toDouble(player.Volume()));
            stopped = false;
            player.Play();
        } else if (action == "play_audio") {
            player.Source(
                MediaSource::CreateFromUri(Windows::Foundation::Uri(nativeText(args["url"].toString()))));
            player.Volume(args["volume"].toDouble(player.Volume()));
            stopped = false;
            player.Play();
        } else {
            const auto operation = args["operation"].toString();
            if (operation == "stop") {
                player.Pause();
                player.Source(nullptr);
                stopped = true;
            } else if (operation == "pause")
                player.Pause();
            else if (operation == "play") {
                stopped = false;
                player.Play();
            } else if (operation == "volume")
                player.Volume(args["value"].toDouble());
            else if (operation == "mute")
                player.IsMuted(args["value"].toBool());
        }
        return {};
    } catch (const winrt::hresult_error &error) {

        // Keep provider details useful without including message text or audio URLs.
        return QString("Windows could not prepare speech or audio (0x%1)")
            .arg(quint32(error.code().value), 8, 16, QChar('0'));
    }
};

QString Announcements::validate(const QString &action, const QJsonObject &args) {
    QStringList keys;
    if (action == "speak") {
        keys = {"message", "voice", "rate", "volume"};
        if (!text(args["message"], 4000) || (args.contains("voice") && !text(args["voice"], 1024)) ||
            (args.contains("rate") && !number(args["rate"], 0.5, 2)))
            return "Enter 1-4000 characters, an installed voice and a speech rate from 0.5 to 2";
    } else if (action == "play_audio") {
        keys = {"url", "volume"};
        const QUrl url(args["url"].toString(), QUrl::StrictMode);
        if (!text(args["url"], 4096) || !url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
            !QStringList{"http", "https"}.contains(url.scheme()))
            return "Audio needs an HTTP or HTTPS URL reachable from this PC, without embedded credentials";
    } else if (action == "audio_control") {
        keys = {"operation", "value"};
        const auto operation = args["operation"].toString();
        if (!QStringList{"play", "pause", "stop", "volume", "mute"}.contains(operation) ||
            (operation == "volume" && !number(args["value"], 0, 1)) ||
            (operation == "mute" && !args["value"].isBool()) ||
            (!QStringList{"volume", "mute"}.contains(operation) && args.contains("value")))
            return "Invalid audio control";
    } else
        return "Unknown announcement command";
    if (args.contains("volume") && !number(args["volume"], 0, 1))
        return "Volume must be between 0 and 1";
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!keys.contains(it.key()))
            return "Unknown announcement option";
    return {};
}

Announcements::Announcements(bool testMode, QObject *parent)
    : QObject(parent), native(std::make_shared<Native>()), testing(testMode) {
    timer.setInterval(1000);
    connect(&timer, &QTimer::timeout, this, [this] {
        if (!permitted || testing || worker.isRunning())
            return;
        worker.onStatus = [this](const QString &error) {
            if (!error.isEmpty() && playback["error"] != "Windows audio is unavailable") {
                playback["error"] = "Windows audio is unavailable";
                emit changed();
            }
        };
        worker.submit([state = native] { return state->snapshot(); },
                      [this](QJsonObject result) {
                          const auto voices = result.take("voices").toArray();
                          if (playback != result || voiceList != voices) {
                              playback = result;
                              voiceList = voices;
                              emit changed();
                          }
                      });
    });
}

Announcements::~Announcements() {
    ++*epoch;
    // WinRT objects and their apartment must close on the worker that created them.
    worker.post([state = std::move(native)] {});
}

void Announcements::setPermitted(bool enabled) {
    if (permitted == enabled)
        return;
    permitted = enabled;
    if (enabled && !testing)
        timer.start();
    else
        timer.stop();
    ++*epoch;
    if (!enabled && !testing)
        worker.post([state = native] {
            if (state->ready)
                state->run("audio_control", {{"operation", "stop"}}, [] { return false; });
        });
    emit changed();
}

QJsonObject Announcements::state() const {
    QJsonObject result{{"state", "idle"}, {"volume", 0.7}, {"muted", false}, {"error", ""}};
    for (auto it = playback.begin(); it != playback.end(); ++it)
        result[it.key()] = it.value();
    result["enabled"] = permitted;
    return result;
}

void Announcements::execute(QString action, QJsonObject args, QString request,
                            std::shared_ptr<std::atomic_bool> connection) {
    auto error = validate(action, args);
    if (error.isEmpty() && !permitted)
        error = "Enable Speech and Audio from Home Assistant in Windows module settings";
    if (error.isEmpty() && worker.isRunning())
        error = "An announcement is being prepared; try again after it completes";
    if (!error.isEmpty()) {
        emit completed(request, false, error);
        return;
    }
    const auto generation = epoch->load();
    worker.onStatus = [this, request](const QString &error) {
        if (!error.isEmpty())
            emit completed(request, false, "Windows could not prepare speech or audio");
    };
    worker.submit(
        [state = native, action, args, connection, token = epoch, generation, testMode = testing] {
            const auto cancelled = [=] {
                return token->load() != generation || (connection && !connection->load());
            };
            if (cancelled())
                return QString("Announcement cancelled");
            return testMode ? QString() : state->run(action, args, cancelled);
        },
        [this, request](const QString &error) {
            emit completed(request, error.isEmpty(), error.isEmpty() ? "Playback requested" : error);
        });
}
