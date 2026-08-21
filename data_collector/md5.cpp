#include "md5.h"

#include <QByteArray>
#include <QFile>

extern "C" {
#include <libavutil/md5.h>
#include <libavutil/mem.h>
}

std::expected<QString, QString> computeFileMd5(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::unexpected(QStringLiteral("Cannot open %1: %2").arg(path, file.errorString()));

    struct Md5Free {
        void operator()(AVMD5 *p) const { av_free(p); }
    };
    const std::unique_ptr<AVMD5, Md5Free> ctx{av_md5_alloc()};
    if (!ctx)
        return std::unexpected(QStringLiteral("av_md5_alloc failed"));
    av_md5_init(ctx.get());

    QByteArray buffer(1 << 20, Qt::Uninitialized);
    while (true) {
        const qint64 n = file.read(buffer.data(), buffer.size());
        if (n < 0)
            return std::unexpected(QStringLiteral("Read error: %1").arg(file.errorString()));
        if (n == 0)
            break;
        av_md5_update(ctx.get(), reinterpret_cast<const uint8_t *>(buffer.constData()),
                      static_cast<size_t>(n));
    }

    uint8_t digest[16];
    av_md5_final(ctx.get(), digest);
    return QString::fromLatin1(
        QByteArray(reinterpret_cast<const char *>(digest), sizeof digest).toHex());
}
