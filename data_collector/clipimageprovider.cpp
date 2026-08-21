#include "clipimageprovider.h"

QImage ClipImageProvider::requestImage(const QString &id, QSize *size,
                                       const QSize &requestedSize)
{
    Q_UNUSED(requestedSize);
    const auto blank = [size] {
        QImage image(clips::kWidth, clips::kHeight, QImage::Format_RGB888);
        image.fill(Qt::black);
        if (size)
            *size = image.size();
        return image;
    };

    const QStringList parts = id.split(u'/');
    if (parts.size() != 3)
        return blank();
    bool clipOk = false, frameOk = false;
    const int clipIndex = parts[1].toInt(&clipOk);
    const int frameIndex = parts[2].toInt(&frameOk);

    const auto batch = store_->get();
    if (!clipOk || !frameOk || !batch || clipIndex < 0
        || clipIndex >= static_cast<int>(batch->size()))
        return blank();
    const auto &frames = (*batch)[clipIndex].frames;
    if (frames.empty() || frameIndex < 0)
        return blank();

    const QImage &image = frames[frameIndex % frames.size()];
    if (size)
        *size = image.size();
    return image;
}
