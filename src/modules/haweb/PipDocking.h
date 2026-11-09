#pragma once
#include <QColor>
#include <QRect>
#include <array>
#include <memory>
#include <optional>

class QWidget;

// Bounds and distance use the same coordinate space. Native dragging supplies
// physical pixels, avoiding Qt's gaps between monitors with different DPI.
struct PipDockTarget {
    QRect bounds;
    Qt::Edges edges;
};
// The caller chooses the monitor under the pointer, or the window's monitor
// for keyboard moves. Previous edges add release tolerance on that monitor.
PipDockTarget pipDockTarget(QRect bounds, const QRect &workArea, int distance,
                            std::optional<QPoint> pointer = {}, Qt::Edges previousEdges = {});

// Four narrow raster surfaces, created on demand. No browser captures, full
// monitor textures or polling timers are needed while dragging the dashboard.
class PipDockPreview {
public:
    PipDockPreview();
    ~PipDockPreview();
    void show(const QRect &nativeTarget, Qt::Edges edges, int thickness, const QColor &color);
    void hide();

private:
    std::array<std::unique_ptr<QWidget>, 4> strips;
};
