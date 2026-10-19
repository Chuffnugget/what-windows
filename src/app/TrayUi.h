#pragma once
#include <QApplication>
#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

inline void styleWhatTrayMenu(QMenu *menu, const QIcon &icon) {
    menu->setObjectName("whatTrayMenu");
    menu->setFont(QFont("Segoe UI", 10));
    menu->setMinimumWidth(280);
    menu->setStyleSheet("QMenu { background:palette(window); color:palette(text); border:1px solid palette(mid); padding:8px; }"
                        "QMenu::item { padding:10px 18px; margin:2px 0; }"
                        "QMenu::item:selected { background:palette(alternate-base); color:palette(link); }"
                        "QMenu::item:disabled { color:palette(midlight); }"
                        "QMenu::separator { height:1px; background:palette(dark); margin:7px 10px; }"
                        "QLabel { background:transparent; color:palette(text); }");
    auto *header = new QWidget(menu);
    auto *layout = new QHBoxLayout(header);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(12);
    auto *mark = new QLabel(header);
    mark->setPixmap(icon.pixmap(32, 40));
    layout->addWidget(mark);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(2);
    auto *title = new QLabel("<b>WHAT</b> " + QCoreApplication::applicationVersion(), header);
    titles->addWidget(title);
    auto *description = new QLabel("Windows Home Assistant Toolkit", header);
    description->setStyleSheet("color:palette(midlight); font-size:11px;");
    titles->addWidget(description);
    layout->addLayout(titles);
    layout->addStretch();
    auto *action = new QWidgetAction(menu);
    action->setDefaultWidget(header);
    menu->addAction(action);
    menu->addSeparator();
}

inline void popupWhatTrayMenu(QMenu *menu) {
    if (!menu)
        return;
    const QPointer<QMenu> guarded(menu);
    QTimer::singleShot(0, menu, [guarded] {
        if (!guarded)
            return;
        if (auto *modal = QApplication::activeModalWidget()) {
            modal->raise();
            modal->activateWindow();
            return;
        }
        guarded->popup(QCursor::pos());
    });
}
