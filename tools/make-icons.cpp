// Format/size conversion only; the user's original artwork is retained verbatim.
#include <QGuiApplication>
#include <QImage>
#include <QBuffer>
#include <QDataStream>
#include <QFile>
#include <QDir>
#include <QPainter>
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (argc != 3)
        return 1;
    const QImage original(QString::fromLocal8Bit(argv[1]));
    if (original.isNull())
        return 2;
    QDir().mkpath(QString::fromLocal8Bit(argv[2]));
    const QDir output(QString::fromLocal8Bit(argv[2]));
    auto square = [&](int size) {
        QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        auto scaled = original.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPainter painter(&image);
        painter.drawImage((size - scaled.width()) / 2, (size - scaled.height()) / 2, scaled);
        return image;
    };
    QList<QByteArray> frames;
    const QList<int> sizes{16, 24, 32, 48, 64, 128, 256};
    for (int size : sizes) {
        // Shell taskbar sizes use a 32-bit DIB with explicit alpha and AND mask.
        // Keep a PNG only for the 256px Explorer frame.
        const auto image = square(size).convertToFormat(QImage::Format_ARGB32);
        QByteArray frame;
        QBuffer buffer(&frame);
        buffer.open(QIODevice::WriteOnly);
        if (size == 256)
            image.save(&buffer, "PNG");
        else {
            QDataStream dib(&buffer);
            dib.setByteOrder(QDataStream::LittleEndian);
            const int maskStride = ((size + 31) / 32) * 4;
            dib << quint32(40) << qint32(size) << qint32(size * 2) << quint16(1) << quint16(32) << quint32(0)
                << quint32(size * size * 4 + maskStride * size) << qint32(0) << qint32(0) << quint32(0)
                << quint32(0);
            for (int y = size - 1; y >= 0; --y)
                for (int x = 0; x < size; ++x) {
                    const auto pixel = image.pixel(x, y);
                    dib << quint8(qBlue(pixel)) << quint8(qGreen(pixel)) << quint8(qRed(pixel))
                        << quint8(qAlpha(pixel));
                }
            for (int y = size - 1; y >= 0; --y) {
                QByteArray mask(maskStride, char(0));
                for (int x = 0; x < size; ++x)
                    if (qAlpha(image.pixel(x, y)) == 0)
                        mask[x / 8] = char(quint8(mask[x / 8]) | (0x80 >> (x % 8)));
                buffer.write(mask);
            }
        }
        frames.append(frame);
    }
    QFile ico(output.filePath("app-icon.ico"));
    if (!ico.open(QIODevice::WriteOnly))
        return 3;
    QDataStream stream(&ico);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(0) << quint16(1) << quint16(sizes.size());
    quint32 offset = 6 + 16 * sizes.size();
    for (int i = 0; i < sizes.size(); ++i) {
        stream << quint8(sizes[i] == 256 ? 0 : sizes[i]) << quint8(sizes[i] == 256 ? 0 : sizes[i])
               << quint8(0) << quint8(0) << quint16(1) << quint16(32) << quint32(frames[i].size()) << offset;
        offset += frames[i].size();
    }
    for (const auto &frame : frames)
        ico.write(frame);
    for (const auto &entry :
         {qMakePair("Logo150.png", 150), qMakePair("Logo44.png", 44), qMakePair("StoreLogo.png", 50)})
        if (!square(entry.second).save(output.filePath(entry.first)))
            return 4;
    // Windows Shell needs explicit unplated variants for transparent taskbar icons.
    for (int size : {16, 20, 24, 30, 32, 36, 40, 44, 48, 60, 64, 72, 80, 96, 256}) {
        for (const auto &theme : {"unplated", "lightunplated"}) {
            const auto name = QString("Logo44.targetsize-%1_altform-%2.png").arg(size).arg(theme);
            if (!square(size).save(output.filePath(name)))
                return 4;
        }
    }
    return 0;
}
