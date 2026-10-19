#include "RichNotifications.h"
#include <QDateTime>
#include <QDebug>
#include <QBuffer>
#include <QFile>
#include <QImage>
#include <QSet>
#include <QCryptographicHash>
#include <QEventLoop>
#include <QImageReader>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTemporaryDir>
#include <QUrl>
#include <QUuid>
#include <windows.h>
#include <appmodel.h>
#include <notificationactivationcallback.h>
#include <wrl.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>

namespace {
// Matches the COM server and toast activation declarations in AppxManifest.xml.
const CLSID activatorId = {0xd34bbe21, 0x98a6, 0x4a15, {0xaa, 0x82, 0x84, 0xe6, 0x44, 0xb7, 0xc0, 0x2a}};

bool text(const QJsonValue &value, int length) {
    return value.isString() && !value.toString().trimmed().isEmpty() && value.toString().size() <= length &&
           !value.toString().contains(QChar::Null);
}

bool webUrl(const QJsonValue &value) {
    const QUrl url(value.toString(), QUrl::StrictMode);
    return text(value, 4096) && url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
           QStringList{"http", "https"}.contains(url.scheme());
}

winrt::hstring nativeText(const QString &value) {
    return winrt::hstring(reinterpret_cast<const wchar_t *>(value.utf16()), value.size());
}

QString xml(const QString &value) {
    return value.toHtmlEscaped().replace("'", "&apos;");
}
} // namespace

struct RichNotifications::Mailbox {
    QMutex mutex;
    bool permitted = false;
    RichNotifications *owner = nullptr;
    quint64 generation = 0;
    QString appId;
    QHash<QString, QJsonObject> pending;
    QJsonArray events;

    // Called with the mailbox locked. Destruction clears owner under the same
    // lock; Qt discards an already-queued invocation if the receiver is destroyed.
    void requestOpen(bool expired) {
        if (owner)
            QMetaObject::invokeMethod(owner, [target = owner, expired] { emit target->openRequested(expired); },
                                      Qt::QueuedConnection);
    }

    void activate(const QString &app, const QString &argument, const QString &reply) {
        QMutexLocker lock(&mutex);
        const auto split = argument.indexOf(':');
        const auto nonce = argument.left(split), action = argument.mid(split + 1);
        const auto entry = pending.value(nonce);
        const auto now = QDateTime::currentSecsSinceEpoch();
        if (app != appId || split != 32)
            return;
        if (!permitted || entry.isEmpty() || entry["expires"].toDouble() < now ||
            !entry["actions"].toArray().contains(action)) {
            // Pending action nonces belong to this process. After a restart,
            // acknowledge Windows' activation but never replay an old action.
            requestOpen(true);
            return;
        }
        if (action == "OPEN")
            requestOpen(false);
        pending.remove(nonce); // Each displayed notification can produce at most one action.
        events.append(QJsonObject{{"id", QUuid::createUuid().toString(QUuid::Id128)},
                                  {"notification_id", entry["id"]},
                                  {"action", action},
                                  {"reply", entry["reply"].toBool() ? reply.left(256) : QString()},
                                  {"created", double(now)}});
        while (events.size() > 32)
            events.removeFirst();
    }
};

struct RichNotifications::Native {
    using ToastNotifier = winrt::Windows::UI::Notifications::ToastNotifier;
    ToastNotifier notifier{nullptr};
    DWORD registration = 0;
    bool initialized = false;
    std::unique_ptr<QTemporaryDir> images;
    QStringList files;
    std::shared_ptr<Mailbox> mailbox;

    struct Activator
        : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                       INotificationActivationCallback> {
        std::shared_ptr<Mailbox> mailbox;

        explicit Activator(std::shared_ptr<Mailbox> value) : mailbox(std::move(value)) {}

        HRESULT STDMETHODCALLTYPE Activate(LPCWSTR app, LPCWSTR arguments,
                                           const NOTIFICATION_USER_INPUT_DATA *input, ULONG count) override {
            try {
                QString reply;
                for (ULONG index = 0; input && index < qMin<ULONG>(count, 16); ++index)
                    if (input[index].Key && wcscmp(input[index].Key, L"reply") == 0 && input[index].Value)
                        reply = QString::fromWCharArray(input[index].Value,
                                                        int(wcsnlen(input[index].Value, 256)));
                if (app && arguments)
                    mailbox->activate(QString::fromWCharArray(app, int(wcsnlen(app, 256))),
                                      QString::fromWCharArray(arguments, int(wcsnlen(arguments, 160))),
                                      reply);
                return S_OK;
            } catch (...) {
                return E_FAIL;
            }
        }
    };

    struct Factory
        : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                       IClassFactory> {
        std::shared_ptr<Mailbox> mailbox;

        explicit Factory(std::shared_ptr<Mailbox> value) : mailbox(std::move(value)) {}

        HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown *outer, REFIID iid, void **result) override {
            if (outer)
                return CLASS_E_NOAGGREGATION;
            return Microsoft::WRL::Make<Activator>(mailbox)->QueryInterface(iid, result);
        }

        HRESULT STDMETHODCALLTYPE LockServer(BOOL) override {
            return S_OK;
        }
    };

    explicit Native(std::shared_ptr<Mailbox> value) : mailbox(std::move(value)) {}

    ~Native() {
        if (registration)
            CoRevokeClassObject(registration);
        notifier = nullptr;
        if (initialized)
            winrt::uninit_apartment();
    }

    QString initialize() {
        if (notifier)
            return {};
        UINT32 length = 0;
        if (GetCurrentApplicationUserModelId(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER)
            return "Install WHAT to use images, replies and action buttons in Windows notifications";
        std::wstring app(length, L'\0');
        if (GetCurrentApplicationUserModelId(&length, app.data()) != ERROR_SUCCESS)
            return "Windows could not identify the installed WHAT app";
        if (!initialized) {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            initialized = true;
        }
        const auto id = QString::fromWCharArray(app.c_str());
        {
            QMutexLocker lock(&mailbox->mutex);
            mailbox->appId = id;
        }
        const auto factory = Microsoft::WRL::Make<Factory>(mailbox);
        if (!registration)
            winrt::check_hresult(CoRegisterClassObject(activatorId, factory.Get(), CLSCTX_LOCAL_SERVER,
                                                       REGCLS_MULTIPLEUSE, &registration));
        notifier =
            winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(nativeText(id));
        images = std::make_unique<QTemporaryDir>();
        return {};
    }

    QString image(const QString &url, const std::function<bool()> &cancelled) {
        QNetworkAccessManager network;
        QNetworkRequest request{QUrl(url)};
        request.setTransferTimeout(10000);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        auto *reply = network.get(request);
        reply->setReadBufferSize(2 * 1024 * 1024 + 1);
        QByteArray bytes;
        QEventLoop loop;
        QTimer deadline, cancellation;
        deadline.setSingleShot(true);
        QObject::connect(&deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
        QObject::connect(&cancellation, &QTimer::timeout, reply, [&] {
            if (cancelled())
                reply->abort();
        });
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
            bytes += reply->readAll();
            if (bytes.size() > 2 * 1024 * 1024)
                reply->abort();
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        deadline.start(10000);
        cancellation.start(100);
        loop.exec();
        bytes += reply->readAll();
        if (reply->error() != QNetworkReply::NoError || bytes.size() > 2 * 1024 * 1024 || cancelled())
            return {};
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        if (!QList<QByteArray>{"png", "jpeg", "jpg", "webp", "gif", "bmp"}.contains(reader.format()))
            return {};
        const auto size = reader.size();
        if (!size.isValid() || size.width() > 4096 || size.height() > 4096)
            return {};
        const auto picture = reader.read();
        if (picture.isNull() || !images->isValid())
            return {};
        const auto path = images->filePath(QUuid::createUuid().toString(QUuid::Id128) + ".png");
        if (!picture.scaled(1280, 720, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(path))
            return {};
        files.append(path);
        while (files.size() > 16)
            QFile::remove(files.takeFirst());
        return QUrl::fromLocalFile(path).toString();
    }

    QString send(const QJsonObject &args, const std::function<bool()> &cancelled) {
        const auto error = initialize();
        if (!error.isEmpty())
            return error;
        QString picture;
        if (args.contains("image")) {
            picture = image(args["image"].toString(), cancelled);
            if (picture.isEmpty())
                return "Notification image could not be loaded (maximum 2 MiB and 4096 pixels per side)";
        }
        if (cancelled())
            return "Notification cancelled";
        const auto nonce = QUuid::createUuid().toString(QUuid::Id128);
        const auto identifier = args["notification_id"].toString(nonce);
        const auto expires = QDateTime::currentSecsSinceEpoch() + args["ttl"].toInt(600);
        QString payload =
            "<toast launch='" + nonce + ":OPEN'><visual><binding template='ToastGeneric'><text>" +
            xml(args["title"].toString()) + "</text><text>" + xml(args["message"].toString()) + "</text>";
        if (!picture.isEmpty())
            payload += "<image placement='hero' src='" + xml(picture) + "'/>";
        payload += "</binding></visual><actions>";
        if (args["reply"].toBool())
            payload += "<input id='reply' type='text' placeHolderContent='Reply'/>";
        QJsonArray actionIds{"OPEN"};
        for (auto value : args["actions"].toArray()) {
            const auto action = value.toObject();
            const auto id = action["id"].toString();
            const bool link = action.contains("url");
            if (!link)
                actionIds.append(id);
            payload += "<action content='" + xml(action["title"].toString()) + "' activationType='" +
                       (link ? "protocol" : "foreground") + "' arguments='" +
                       xml(link ? action["url"].toString() : nonce + ':' + id) + "'" +
                       (!link && args["reply"].toBool() ? " hint-inputId='reply'" : "") + "/>";
        }
        payload += "</actions>";
        if (args["silent"].toBool())
            payload += "<audio silent='true'/>";
        payload += "</toast>";
        winrt::Windows::Data::Xml::Dom::XmlDocument document;
        document.LoadXml(nativeText(payload));
        winrt::Windows::UI::Notifications::ToastNotification toast(document);
        toast.ExpirationTime(winrt::clock::from_time_t(expires));
        const auto tag = QString::fromLatin1(
            QCryptographicHash::hash(identifier.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
        toast.Tag(nativeText(tag));
        toast.Group(L"WHAT");
        {
            QMutexLocker lock(&mailbox->mutex);
            if (!mailbox->permitted)
                return "Notifications permission was revoked";
            for (const auto &key : mailbox->pending.keys()) {
                const auto value = mailbox->pending.value(key);
                if (value["id"] == identifier ||
                    value["expires"].toDouble() < QDateTime::currentSecsSinceEpoch())
                    mailbox->pending.remove(key);
            }
            if (mailbox->pending.size() >= 32)
                return "Too many pending interactive notifications";
            mailbox->pending.insert(nonce, {{"id", identifier},
                                            {"expires", double(expires)},
                                            {"actions", actionIds},
                                            {"reply", args["reply"].toBool()}});
        }
        try {
            notifier.Show(toast);
        } catch (...) {
            QMutexLocker lock(&mailbox->mutex);
            mailbox->pending.remove(nonce);
            throw;
        }
        return {};
    }
};

QString RichNotifications::validate(const QJsonObject &args) {
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!QStringList{"title", "message", "level", "notification_id", "image", "actions", "reply",
                         "silent", "ttl"}
                 .contains(it.key()))
            return "Unknown notification option";
    if (!text(args["title"], 128) || args["title"].toString().contains('\n') ||
        args["title"].toString().contains('\r') || !text(args["message"], 2000))
        return "Notification needs a title of 1-128 characters and message of 1-2000 characters";
    if (args.contains("level") &&
        !QStringList{"information", "warning", "error"}.contains(args["level"].toString()))
        return "Invalid notification level";
    if ((args.contains("notification_id") && !text(args["notification_id"], 64)) ||
        (args.contains("image") && !webUrl(args["image"])) ||
        (args.contains("reply") && !args["reply"].isBool()) ||
        (args.contains("silent") && !args["silent"].isBool()) ||
        (args.contains("ttl") && (!args["ttl"].isDouble() || args["ttl"].toDouble() != args["ttl"].toInt() ||
                                  args["ttl"].toInt() < 1 || args["ttl"].toInt() > 3600)))
        return "Invalid notification ID, image, reply, sound or expiry option";
    if (args.contains("actions") && (!args["actions"].isArray() || args["actions"].toArray().size() > 4))
        return "Use at most four notification actions";
    QSet<QString> ids{"OPEN"};
    for (auto value : args["actions"].toArray()) {
        const auto action = value.toObject();
        if (!value.isObject() || !text(action["id"], 64) || ids.contains(action["id"].toString()) ||
            action["id"].toString().contains(':') || !text(action["title"], 40) ||
            (action.contains("url") && !webUrl(action["url"])) ||
            action.size() != (action.contains("url") ? 3 : 2))
            return "Each action needs a unique ID, a short title and an optional HTTP or HTTPS link";
        ids.insert(action["id"].toString());
    }
    if (args["reply"].toBool()) {
        bool canReply = false;
        for (auto value : args["actions"].toArray())
            canReply |= !value.toObject().contains("url");
        if (!canReply)
            return "Add an action without a URL to submit a reply";
    }
    return {};
}

RichNotifications::RichNotifications(bool testMode, QObject *parent)
    : QObject(parent), mailbox(std::make_shared<Mailbox>()), native(std::make_shared<Native>(mailbox)),
      testing(testMode) {
    mailbox->owner = this;
    timer.setInterval(250);
    connect(&timer, &QTimer::timeout, this, [this] {
        QJsonArray current;
        {
            QMutexLocker lock(&mailbox->mutex);
            while (!mailbox->events.isEmpty() && mailbox->events.first().toObject()["created"].toDouble() <
                                                     QDateTime::currentSecsSinceEpoch() - 60)
                mailbox->events.removeFirst();
            current = mailbox->events;
        }
        if (events != current) {
            events = current;
            emit changed();
        }
    });
}

RichNotifications::~RichNotifications() {
    {
        QMutexLocker lock(&mailbox->mutex);
        mailbox->owner = nullptr;
    }
    setPermitted(false);
    worker.post([state = std::move(native)] {});
}

void RichNotifications::setPermitted(bool enabled) {
    QMutexLocker lock(&mailbox->mutex);
    if (mailbox->permitted == enabled)
        return;
    mailbox->permitted = enabled;
    ++mailbox->generation;
    if (enabled)
        timer.start();
    else
        timer.stop();
    if (!enabled) {
        mailbox->pending.clear();
        mailbox->events = {};
        events = {};
    }
    if (enabled && !testing)
        worker.post([state = native] { state->initialize(); });
}

void RichNotifications::prepareActivation() {
    if (testing || activationPrepared)
        return;
    activationPrepared = true;
    worker.post([state = native] {
        const auto error = state->initialize();
        if (!error.isEmpty())
            qWarning().noquote() << "Notification activation:" << error;
    });
}

QJsonObject RichNotifications::state() const {
    return {{"version", 1}, {"events", events}};
}

void RichNotifications::send(QJsonObject args, QString request, std::shared_ptr<std::atomic_bool> lease) {
    auto error = validate(args);
    quint64 generation;
    {
        QMutexLocker lock(&mailbox->mutex);
        generation = mailbox->generation;
        if (!mailbox->permitted)
            error = "Enable Notifications from Home Assistant in Windows module settings";
    }
    if (error.isEmpty() && worker.isRunning())
        error = "Another notification is being prepared";
    if (!error.isEmpty()) {
        emit completed(request, false, error);
        return;
    }
    worker.onStatus = [this, request](const QString &error) {
        if (!error.isEmpty())
            emit completed(request, false, "Windows could not display the notification");
    };
    worker.submit(
        [state = native, args, lease, generation, testMode = testing] {
            const auto cancelled = [=] {
                QMutexLocker lock(&state->mailbox->mutex);
                return !state->mailbox->permitted || state->mailbox->generation != generation || !lease ||
                       !lease->load();
            };
            if (cancelled())
                return QString("Notification cancelled");
            return testMode ? QString() : state->send(args, cancelled);
        },
        [this, request](const QString &error) {
            emit completed(request, error.isEmpty(), error.isEmpty() ? "Submitted to Windows" : error);
        });
}
