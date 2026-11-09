#include "PipDocking.h"
#include <QPainter>
#include <QWidget>
#include <windows.h>

PipDockTarget pipDockTarget(QRect bounds, const QRect &workArea, int distance,
                            std::optional<QPoint> pointer, Qt::Edges previousEdges) {
    PipDockTarget target{bounds, {}};
    if (!bounds.isValid() || !workArea.isValid() || distance < 0)
        return target;
    // Signed distances keep a target active after crossing its boundary. With
    // absolute distances the preview vanished when most of PiP went offscreen.
    const auto axis = [&](int low, int high, int begin, int end, std::optional<int> cursor,
                          Qt::Edge lowEdge, Qt::Edge highEdge) -> Qt::Edges {
        const int lowLimit = distance + (previousEdges.testFlag(lowEdge) ? distance / 2 : 0);
        const int highLimit = distance + (previousEdges.testFlag(highEdge) ? distance / 2 : 0);
        if (cursor) {
            if (*cursor - begin <= lowLimit && *cursor - begin <= end - *cursor)
                return lowEdge;
            if (end - *cursor <= highLimit)
                return highEdge;
        }
        if (low - begin <= lowLimit && low - begin <= end - high)
            return lowEdge;
        if (end - high <= highLimit)
            return highEdge;
        return {};
    };
    if (bounds.width() <= workArea.width()) {
        target.edges |= axis(bounds.left(), bounds.right(), workArea.left(), workArea.right(),
                             pointer ? std::optional<int>(pointer->x()) : std::nullopt,
                             Qt::LeftEdge, Qt::RightEdge);
        if (target.edges.testFlag(Qt::LeftEdge))
            target.bounds.moveLeft(workArea.left());
        else if (target.edges.testFlag(Qt::RightEdge))
            target.bounds.moveRight(workArea.right());
    }
    if (bounds.height() <= workArea.height()) {
        target.edges |= axis(bounds.top(), bounds.bottom(), workArea.top(), workArea.bottom(),
                             pointer ? std::optional<int>(pointer->y()) : std::nullopt,
                             Qt::TopEdge, Qt::BottomEdge);
        if (target.edges.testFlag(Qt::TopEdge))
            target.bounds.moveTop(workArea.top());
        else if (target.edges.testFlag(Qt::BottomEdge))
            target.bounds.moveBottom(workArea.bottom());
    }
    // Preserve the user's size and position along a side. Only keep the other
    // axis on screen when actually docking, including the rolled-up title bar.
    if (target.edges) {
        if (bounds.width() <= workArea.width())
            target.bounds.moveLeft(qBound(workArea.left(), target.bounds.left(),
                                          workArea.right() - bounds.width() + 1));
        if (bounds.height() <= workArea.height())
            target.bounds.moveTop(qBound(workArea.top(), target.bounds.top(),
                                         workArea.bottom() - bounds.height() + 1));
    }
    return target;
}

namespace {
class OutlineStrip final : public QWidget {
public:
    OutlineStrip() : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint |
            Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus) {
        setObjectName("hawebDockPreview");
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_QuitOnClose, false);
    }
    QColor color;
    QRect nativeBounds;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), color);
    }
};
}

PipDockPreview::PipDockPreview() = default;
PipDockPreview::~PipDockPreview() = default;

void PipDockPreview::show(const QRect &landing, Qt::Edges edges, int thickness, const QColor &color) {
    if (!edges || !landing.isValid()) {
        hide();
        return;
    }
    thickness = qBound(1, thickness, qMin(landing.width(), landing.height()));
    const Qt::Edge sides[]{Qt::LeftEdge, Qt::TopEdge, Qt::RightEdge, Qt::BottomEdge};
    for (size_t i = 0; i < strips.size(); ++i) {
        if (!strips[i])
            strips[i] = std::make_unique<OutlineStrip>();
        auto *strip = static_cast<OutlineStrip *>(strips[i].get());
        auto shade = color;
        shade.setAlpha(edges.testFlag(sides[i]) ? 235 : 115);
        auto bounds = landing;
        if (sides[i] == Qt::LeftEdge || sides[i] == Qt::RightEdge) {
            bounds.setWidth(thickness);
            if (sides[i] == Qt::RightEdge)
                bounds.moveRight(landing.right());
        } else {
            bounds.setHeight(thickness);
            if (sides[i] == Qt::BottomEdge)
                bounds.moveBottom(landing.bottom());
        }
        if (strip->isVisible() && strip->nativeBounds == bounds && strip->color == shade)
            continue;
        strip->nativeBounds = bounds;
        strip->color = shade;
        // Native screen coordinates also handle negative monitor origins and
        // taskbars on any edge without converting absolute positions by DPI.
        const auto hwnd = reinterpret_cast<HWND>(strip->winId());
        SetWindowPos(hwnd, HWND_TOPMOST, bounds.x(), bounds.y(), bounds.width(), bounds.height(),
                     SWP_NOACTIVATE);
        strip->show();
        SetWindowPos(hwnd, HWND_TOPMOST, bounds.x(), bounds.y(), bounds.width(), bounds.height(),
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        strip->update();
    }
}

void PipDockPreview::hide() {
    for (const auto &strip : strips)
        if (strip)
            strip->hide();
}
