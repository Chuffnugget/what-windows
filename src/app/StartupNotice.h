#pragma once
#include <QApplication>
#include "AnimatedBrandLogo.h"
#include <QCursor>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include <QWidget>
#include <QFrame>
#include <QIcon>
#include <functional>

// No hidden main-window parent: Windows must be able to show this independently.
inline QWidget *createStartupNotice(std::function<void()> open) {
    auto *notice = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    notice->setAttribute(Qt::WA_ShowWithoutActivating);
    notice->setWindowTitle("WHAT is running");
    notice->setAttribute(Qt::WA_TranslucentBackground);
    notice->setFixedWidth(420);
    notice->setStyleSheet(
        "QWidget { color:palette(text); font:13px 'Segoe UI'; }"
        "QFrame#noticeCard { background:palette(tooltip-base); border:1px solid palette(mid); }"
        "QLabel { background:transparent; border:0; }"
        "QPushButton { background:palette(button); border:1px solid palette(mid); padding:8px 12px; }"
        "QPushButton:hover { background:palette(alternate-base); }"
        "QPushButton:pressed { background:palette(shadow); }"
        "QPushButton#openApp { background:palette(highlight); color:palette(highlighted-text); border:0; font-weight:600; }"
        "QPushButton#openApp:hover { background:palette(link-visited); }");
    auto *outer = new QVBoxLayout(notice);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *card = new QFrame;
    card->setObjectName("noticeCard");
    outer->addWidget(card);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);
    auto *header = new QHBoxLayout;
    header->setSpacing(12);
    auto *logo = new AnimatedBrandLogo;
    header->addWidget(logo);
    auto *titles = new QVBoxLayout;
    titles->setSpacing(4);
    auto *title = new QLabel("WHAT");
    title->setStyleSheet("font-size:19px; font-weight:600;");
    titles->addWidget(title);
    auto *state = new QLabel("RUNNING IN YOUR SYSTEM TRAY");
    state->setStyleSheet("color:palette(link);font-size:10px;font-weight:600;");
    titles->addWidget(state);
    header->addLayout(titles, 1);
    layout->addLayout(header);
    auto *description = new QLabel(
        "The app is open in the background. Use the blue house in your system tray whenever you need it.");
    description->setWordWrap(true);
    description->setStyleSheet("color:palette(text);");
    layout->addWidget(description);
    auto *hint = new QLabel("Can't see it? Check the hidden-icons arrow beside the clock.");
    hint->setWordWrap(true);
    hint->setStyleSheet("color:palette(midlight);font-size:11px;");
    layout->addWidget(hint);
    auto *buttons = new QHBoxLayout;
    buttons->setSpacing(12);
    auto *show = new QPushButton("Open WHAT");
    show->setObjectName("openApp");
    auto *dismiss = new QPushButton("Got it");
    buttons->addWidget(show, 1);
    buttons->addWidget(dismiss);
    layout->addLayout(buttons);
    QObject::connect(show, &QPushButton::clicked, notice, [notice, open] {
        notice->hide();
        open();
    });
    QObject::connect(dismiss, &QPushButton::clicked, notice, &QWidget::hide);
    notice->adjustSize();
    auto *screen = QApplication::screenAt(QCursor::pos());
    if (!screen)
        screen = QApplication::primaryScreen();
    const auto area = screen->availableGeometry();
    notice->move(area.right() - notice->width() - 16, area.bottom() - notice->height() - 16);
    return notice;
}
