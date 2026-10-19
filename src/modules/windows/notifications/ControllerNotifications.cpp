#include "app/Controller.h"

void Controller::initAnnouncements() {
    richNotifications = new RichNotifications(testMode, this);
    richNotifications->setPermitted(controlPermissions()["notifications"].toBool());
    connect(richNotifications, &RichNotifications::changed, this, &Controller::publish);
    connect(richNotifications, &RichNotifications::completed, this,
            [this](const QString &request, bool ok, const QString &message) {
        const auto split = request.indexOf('\n');
        if (split >= 0)
            server.replyCommand(request.left(split), request.mid(split + 1), ok, message);
        log(ok ? "Rich notification submitted to Windows" : "Rich notification rejected or failed");
    });
    announcements = new Announcements(testMode, this);
    announcements->setPermitted(controlPermissions()["announcements"].toBool());
    connect(announcements, &Announcements::changed, this, [this] { publish(); emit changed(); });
    connect(announcements, &Announcements::completed, this,
            [this](const QString &request, bool ok, const QString &message) {
        const auto split = request.indexOf('\n');
        if (split >= 0)
            server.replyCommand(request.left(split), request.mid(split + 1), ok, message);
        log(ok ? "Announcement playback requested" : "Announcement rejected or failed");
    });
}

void Controller::handleAnnouncementCommand(const QString &session, const QJsonObject &command) {
    const auto lease = server.commandLease(session);
    if (lease && lease->load())
        announcements->execute(command["action"].toString(), command["args"].toObject(),
                               session + '\n' + command["id"].toString(), lease);
}

QString Controller::sendDesktopNotification(const QJsonObject &args) {
    for (auto it = args.begin(); it != args.end(); ++it)
        if (!QStringList{"title", "message", "level"}.contains(it.key()) || !it.value().isString())
            return "Invalid notification arguments";
    const auto title = args["title"].toString(), message = args["message"].toString(),
               level = args["level"].toString("information");
    // Shell notification buffers include their terminating NUL: reject rather
    // than silently truncate. Plain text only; no URLs, shell or XML evaluation.
    if (title.trimmed().isEmpty() || title.size() > 63 || title.contains(QChar::Null) ||
        title.contains('\n') || title.contains('\r'))
        return "Notification title must contain 1–63 characters on one line";
    if (message.trimmed().isEmpty() || message.size() > 255 || message.contains(QChar::Null))
        return "Notification message must contain 1–255 characters";
    if (!QStringList{"information", "warning", "error"}.contains(level))
        return "Notification level must be information, warning or error";
    if (!controlPermissions()["notifications"].toBool())
        return "Enable notifications from Home Assistant in Modules → Windows → Notifications from Home Assistant";
    if (notificationClock.isValid() && notificationClock.elapsed() < 1000)
        return "Notifications are limited to one per second";
    if (!testMode) {
        if (!notificationSender)
            return "Windows notification sender is unavailable";
        const auto error = notificationSender(title, message, level);
        if (!error.isEmpty())
            return error;
    }
    notificationClock.restart();
    log(QString("Home Assistant notification submitted (%1; title %2 characters, message %3 characters)%4")
            .arg(level)
            .arg(title.size())
            .arg(message.size())
            .arg(testMode ? " [test mode; not displayed]" : ""));
    return {};
}

void Controller::handleNotificationCommand(const QString &session, const QJsonObject &command) {
    const auto lease = server.commandLease(session);
    if (!lease || !lease->load())
        return;
    const auto args = command["args"].toObject();
    bool rich = args["title"].toString().size() > 63 || args["message"].toString().size() > 255;
    for (auto it = args.begin(); it != args.end(); ++it)
        rich |= !QStringList{"title", "message", "level"}.contains(it.key());
    if (rich) {
        if (notificationClock.isValid() && notificationClock.elapsed() < 1000) {
            server.replyCommand(session, command["id"].toString(), false, "Notifications are limited to one per second");
            return;
        }
        notificationClock.restart();
        richNotifications->send(args, session + '\n' + command["id"].toString(), lease);
        return;
    }
    const auto error = sendDesktopNotification(command["args"].toObject());
    if (!error.isEmpty())
        log("Home Assistant notification rejected: " + error);
    server.replyCommand(
        session, command["id"].toString(), error.isEmpty(),
        error.isEmpty()
            ? "Submitted to Windows. Windows notification settings and Do not disturb determine whether it is shown."
            : error);
}
