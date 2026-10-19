#pragma once
#include "core/BackgroundWorker.h"
#include <QImage>
#include <QVariantMap>
#include <QBuffer>
#include <QTimer>
#include <QSet>

// Rasterization and PNG encoding use QImage on a worker. QPixmap and native
// Shell calls remain with their GUI owners. At most one batch is in flight;
// queued changes replace older requests, including removal/style changes.
class IconRenderer : public QObject {
public:
    struct Request {
        QString id, text;
        QVariantMap style;
        QList<int> sizes;
        bool preview = false;

        bool operator==(const Request &other) const {
            return id == other.id && text == other.text && style == other.style && sizes == other.sizes &&
                   preview == other.preview;
        }
    };

    struct Artwork {
        Request request;
        QList<QImage> images;
        QString preview;
    };

    using Paint = std::function<QImage(const Request &, int)>;

    explicit IconRenderer(Paint paint) : paint(std::move(paint)) {}

    std::function<void()> ready;

    std::optional<Artwork> get(Request request) {
        wanted[request.id] = request;
        auto found = cache.constFind(request.id);
        if (found != cache.cend() && found->request == request)
            return *found;
        if (!scheduled) {
            scheduled = true;
            QTimer::singleShot(0, this, [this] {
                scheduled = false;
                dispatch();
            });
        }
        return {};
    }

    void retain(const QSet<QString> &ids) {
        for (const auto &id : wanted.keys())
            if (!ids.contains(id)) {
                wanted.remove(id);
                cache.remove(id);
            }
    }

private:
    void dispatch() {
        if (job.isRunning())
            return;
        QList<Request> requests;
        for (const auto &request : wanted)
            if (!cache.contains(request.id) || !(cache[request.id].request == request))
                requests << request;
        if (requests.isEmpty())
            return;
        job.submit(
            [requests, paint = paint] {
                QList<Artwork> results;
                for (const auto &request : requests) {
                    Artwork result{request, {}, {}};
                    for (int size : request.sizes)
                        result.images << paint(request, size);
                    if (request.preview) {
                        // A 48 logical-pixel preview stays sharp through 400% display scaling.
                        const auto index = request.sizes.indexOf(192);
                        const auto image = index >= 0 ? result.images[index] : paint(request, 192);
                        QByteArray bytes;
                        QBuffer buffer(&bytes);
                        buffer.open(QIODevice::WriteOnly);
                        image.save(&buffer, "PNG");
                        result.preview = "data:image/png;base64," + QString::fromLatin1(bytes.toBase64());
                    }
                    results << result;
                }
                return results;
            },
            [this](const QList<Artwork> &results) {
                for (const auto &result : results)
                    if (wanted.contains(result.request.id) && wanted[result.request.id] == result.request)
                        cache[result.request.id] = result;
                if (ready)
                    ready();
                dispatch();
            });
    }

    Paint paint;
    QHash<QString, Request> wanted;
    QHash<QString, Artwork> cache;
    bool scheduled = false;
    BackgroundWorker job{"Sensor icon renderer"};
};
