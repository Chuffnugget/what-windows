#pragma once
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QVariantAnimation>
#include <QTimer>
#include <QVariantMap>
#include <QEnterEvent>
#include <cmath>

// Paint into the parent's backing store: no separate translucent Quick surface
// can punch a transparent rectangle through the prompt's background.
class AnimatedBrandLogo final : public QWidget {
    QVariantAnimation motion;
    QTimer pause;
    bool enabled = true, hovered = false;
    qreal progress = 0;

public:
    explicit AnimatedBrandLogo(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("animatedPromptLogo");
        setFixedSize(48, 58);
        setAutoFillBackground(false);
        motion.setStartValue(0.0);
        motion.setEndValue(1.0);
        motion.setDuration(1500);
        pause.setSingleShot(true);
        pause.setInterval(180);
        connect(&motion, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            progress = v.toReal();
            update();
        });
        connect(&motion, &QVariantAnimation::finished, this, [this] {
            progress = 0;
            update();
            if (enabled && hovered && isVisible())
                pause.start();
        });
        connect(&pause, &QTimer::timeout, this, [this] {
            if (enabled && hovered && isVisible())
                motion.start();
        });
    }

    void configure(bool animate, const QVariantMap &timings) {
        enabled = animate;
        motion.setDuration(timings.value("logo", 1500).toInt());
        pause.setInterval(timings.value("logoPause", 180).toInt());
        if (!enabled) {
            motion.stop();
            pause.stop();
            progress = 0;
            update();
        } else if (hovered && isVisible() && motion.state() != QAbstractAnimation::Running &&
                   !pause.isActive())
            motion.start();
    }

protected:
    void enterEvent(QEnterEvent *event) override {
        hovered = true;
        if (enabled && motion.state() != QAbstractAnimation::Running && !pause.isActive())
            motion.start();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override {
        hovered = false;
        QWidget::leaveEvent(event);
    }

    void hideEvent(QHideEvent *event) override {
        hovered = false;
        motion.stop();
        pause.stop();
        progress = 0;
        QWidget::hideEvent(event);
    }

    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal unit = qMin(width() / 668.0, height() / 848.0);
        p.translate((width() - 668 * unit) / 2 - 178 * unit, (height() - 848 * unit) / 2 - 66 * unit);
        p.scale(unit, unit);
        auto ease = [](qreal v) {
            v = qBound(0.0, v, 1.0);
            return v * v * (3 - 2 * v);
        };
        const qreal travel = 50 * ease(progress / .25) * (1 - ease((progress - .38) / .42));
        const qreal dx = travel * 35 / 37, dy = travel * 12 / 37, d = travel / std::sqrt(2.0);
        const QPointF origins[] = {{230 - dx, 390 - dy},
                                   {527 + dx, 390 - dy},
                                   {527 + d, 646 + d},
                                   {230 - d, 646 + d},
                                   {230, 120 - travel}};
        QVector<QPolygonF> shapes;
        QPainterPath fill;
        for (int i = 0; i < 5; ++i) {
            const auto a = origins[i];
            QPolygonF polygon;
            if (i == 4)
                polygon << a + QPointF(0, 240) << a + QPointF(282, 0) << a + QPointF(564, 240)
                        << a + QPointF(0, 240);
            else
                polygon << a << a + QPointF(267, 0) << a + QPointF(267, 226) << a + QPointF(0, 226) << a;
            shapes << polygon;
            fill.addPolygon(polygon);
        }
        p.fillPath(fill, QColor("#1EB9E8"));

        struct Segment {
            QPointF a, b;
            qreal start, length;
            bool draw;
        };

        QVector<Segment> route;
        qreal total = 0;
        QPointF last;
        bool first = true;
        auto segment = [&](QPointF a, QPointF b, bool draw) {
            qreal length = QLineF(a, b).length();
            route.append({a, b, total, length, draw});
            total += length;
        };
        for (int i : {4, 0, 1, 2, 3}) {
            const auto &polygon = shapes[i];
            if (!first)
                segment(last, polygon[0], false);
            first = false;
            for (int j = 1; j < polygon.size(); ++j)
                segment(polygon[j - 1], polygon[j], true);
            last = polygon.last();
        }
        if (progress <= 0 || progress >= 1)
            return;
        const qreal start = total * qBound(0.0, (progress - .38) / .42, 1.0),
                    end = total * qMin(1.0, progress / .25);
        QPainterPath trace;
        for (const auto &part : route) {
            qreal from = qMax(start, part.start), to = qMin(end, part.start + part.length);
            if (!part.draw || to <= from)
                continue;
            trace.moveTo(part.a + (part.b - part.a) * ((from - part.start) / part.length));
            trace.lineTo(part.a + (part.b - part.a) * ((to - part.start) / part.length));
        }
        p.setPen(QPen(QColor(209, 249, 255, 217), 6, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        p.drawPath(trace);
        const qreal tip = start > 0 ? start : end;
        for (const auto &part : route)
            if (part.draw && tip > part.start && tip < part.start + part.length) {
                p.setPen(Qt::NoPen);
                p.setBrush(Qt::white);
                p.drawEllipse(part.a + (part.b - part.a) * ((tip - part.start) / part.length), 5, 5);
                break;
            }
    }
};
