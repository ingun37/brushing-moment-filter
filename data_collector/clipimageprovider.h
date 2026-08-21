#pragma once

#include "clipstore.h"

#include <QQuickImageProvider>
#include <memory>

// Serves clip frames as "image://clips/<epoch>/<clipIndex>/<frameIndex>".
// The epoch component only busts Image's URL comparison; lookups use the
// current batch in the ClipStore.
class ClipImageProvider : public QQuickImageProvider
{
public:
    explicit ClipImageProvider(std::shared_ptr<const ClipStore> store)
        : QQuickImageProvider(QQuickImageProvider::Image), store_(std::move(store))
    {
    }

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

private:
    std::shared_ptr<const ClipStore> store_;
};
