#include "ColourPalette.h"
#include <QApplication>
#include <QColorDialog>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
QString colourName(const QColor &colour) {
    return colour.name(colour.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb);
}

QIcon swatchIcon(const QColor &colour) {
    QPixmap image(24, 24);
    QPainter painter(&image);
    for (int y = 0; y < 24; y += 6)
        for (int x = 0; x < 24; x += 6)
            painter.fillRect(x, y, 6, 6, ((x + y) / 6) % 2 ? Qt::gray : Qt::white);
    painter.fillRect(image.rect(), colour);
    return QIcon(image);
}
} // namespace

ColourPalette::ColourPalette(QSettings &settings, QObject *parent)
    : QObject(parent), settings(settings) {}

QStringList ColourPalette::colours(const QString &key, int limit) const {
    QStringList result;
    for (const auto &value : settings.value(key).toStringList()) {
        const QColor colour(value);
        if (!colour.isValid())
            continue;
        const auto name = colourName(colour);
        if (!result.contains(name))
            result.append(name);
        if (result.size() == limit)
            break;
    }
    return result;
}

QStringList ColourPalette::recent() const {
    return colours("colours/recent", 16);
}

QStringList ColourPalette::favourites() const {
    return colours("colours/favourites", 32);
}

void ColourPalette::remember(const QColor &colour) {
    if (!colour.isValid())
        return;
    auto values = recent();
    const auto name = colourName(colour);
    if (!values.isEmpty() && values.first() == name)
        return;
    values.removeAll(name);
    values.prepend(name);
    values = values.mid(0, 16);
    settings.setValue("colours/recent", values);
    emit changed();
}

void ColourPalette::setFavourite(const QColor &colour, bool favourite) {
    if (!colour.isValid())
        return;
    auto values = favourites();
    const auto name = colourName(colour);
    if (values.contains(name) == favourite || (favourite && values.size() >= 32))
        return;
    if (favourite)
        values.append(name);
    else
        values.removeAll(name);
    settings.setValue("colours/favourites", values);
    emit changed();
}

QColor ColourPalette::choose(const QColor &initial, const QString &title, bool alpha) {
    QColorDialog dialog(initial, QApplication::activeWindow());
    dialog.setObjectName("savedColourDialog");
    dialog.setWindowTitle(title);
    dialog.setOption(QColorDialog::DontUseNativeDialog);
    dialog.setOption(QColorDialog::ShowAlphaChannel, alpha);
    auto *saved = new QWidget(&dialog);
    auto *layout = new QVBoxLayout(saved);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *favourite = new QPushButton(saved);
    favourite->setObjectName("favouriteColour");
    layout->addWidget(favourite);
    auto addSection = [&](const QString &title) {
        layout->addWidget(new QLabel(title, saved));
        auto *scroll = new QScrollArea(saved);
        scroll->setWidgetResizable(true);
        scroll->setFixedHeight(78);
        auto *content = new QWidget(scroll);
        auto *grid = new QGridLayout(content);
        grid->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        scroll->setWidget(content);
        layout->addWidget(scroll);
        return grid;
    };
    auto *recentGrid = addSection("Previously Used Colours");
    auto *favouriteGrid = addSection("Favourites");
    auto populate = [&](QGridLayout *grid, const QStringList &values, const QString &empty) {
        while (auto *item = grid->takeAt(0)) {
            item->widget()->hide();
            item->widget()->deleteLater();
            delete item;
        }
        if (values.isEmpty())
            grid->addWidget(new QLabel(empty, saved), 0, 0);
        for (int i = 0; i < values.size(); ++i) {
            const QColor colour(values[i]);
            auto *button = new QToolButton(saved);
            button->setIcon(swatchIcon(colour));
            button->setIconSize(QSize(24, 24));
            button->setFixedSize(32, 32);
            button->setToolTip("Choose " + values[i].toUpper());
            button->setAccessibleName(button->toolTip());
            if (grid == favouriteGrid) {
                button->setToolTip(button->toolTip() + ". Right-click to remove this favourite.");
                button->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(button, &QToolButton::customContextMenuRequested, &dialog,
                        [&, button, colour](const QPoint &point) {
                            QMenu menu(&dialog);
                            auto *remove = menu.addAction("Remove Favourite");
                            if (menu.exec(button->mapToGlobal(point)) == remove)
                                setFavourite(colour, false);
                        });
            }
            connect(button, &QToolButton::clicked, &dialog, [&, colour] {
                auto selected = colour;
                if (!alpha)
                    selected.setAlpha(255);
                dialog.setCurrentColor(selected);
            });
            grid->addWidget(button, i / 8, i % 8);
        }
    };
    auto updateFavourite = [&] {
        const bool selected = favourites().contains(colourName(dialog.currentColor()));
        favourite->setText(selected ? "Remove from Favourites" : "Add to Favourites");
        favourite->setEnabled(selected || favourites().size() < 32);
        favourite->setToolTip(selected ? "Remove this colour from your saved favourites."
                                      : "Save this colour across WHAT. Up to 32 favourites can be kept.");
    };
    auto refresh = [&] {
        populate(recentGrid, recent(), "Colours appear here after you use them.");
        populate(favouriteGrid, favourites(), "Choose a colour, then add it to Favourites.");
        updateFavourite();
    };
    connect(this, &ColourPalette::changed, &dialog, refresh);
    connect(&dialog, &QColorDialog::currentColorChanged, &dialog, updateFavourite);
    connect(favourite, &QPushButton::clicked, &dialog, [&] {
        const auto colour = dialog.currentColor();
        setFavourite(colour, !favourites().contains(colourName(colour)));
    });
    // Qt's colour dialog uses a vertical layout ending in its OK/Cancel buttons.
    auto *dialogLayout = qobject_cast<QVBoxLayout *>(dialog.layout());
    dialogLayout->insertWidget(dialogLayout->count() - 1, saved);
    refresh();
    dialog.ensurePolished();
    dialog.adjustSize();
    if (dialog.exec() != QDialog::Accepted)
        return {};
    const auto colour = dialog.selectedColor();
    remember(colour);
    return colour;
}
