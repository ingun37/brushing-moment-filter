#include "clipimageprovider.h"
#include "clips.h"
#include "clipstore.h"
#include "md5.h"
#include "session.h"

#include <QGuiApplication>
#include <QQmlApplicationEngine>

#include <cstdio>
#include <memory>
#include <ranges>

// Headless check of the FFmpeg pipeline (md5 → extract → encode), handy while
// developing: appdata_collector --selftest <video> <outDir>
static int selftest(const QString &videoPath, const QString &outDir)
{
    const auto md5 = computeFileMd5(videoPath);
    if (!md5)
        return std::fprintf(stderr, "md5: %s\n", qPrintable(md5.error())), 1;
    std::printf("md5: %s\n", qPrintable(*md5));

    const auto duration = clips::videoDurationSeconds(videoPath);
    if (!duration)
        return std::fprintf(stderr, "duration: %s\n", qPrintable(duration.error())), 1;
    std::printf("duration: %.3f s\n", *duration);

    const auto batch = clips::extractClips(videoPath, 0.0, 3, 5.0, 1.0);
    if (!batch)
        return std::fprintf(stderr, "extract: %s\n", qPrintable(batch.error())), 1;
    for (size_t i = 0; i < batch->size(); ++i) {
        const clips::Clip &clip = (*batch)[i];
        std::printf("clip %zu: start=%.3f fps=%.3f frames=%zu\n", i, clip.startSeconds, clip.fps,
                    clip.frames.size());
        const QString out = outDir + QStringLiteral("/selftest_%1.mp4").arg(i);
        if (const auto encoded = clips::encodeClip(clip, out); !encoded)
            return std::fprintf(stderr, "encode: %s\n", qPrintable(encoded.error())), 1;
        std::printf("encoded %s\n", qPrintable(out));
    }
    return 0;
}

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("data_collector"));

    const QStringList args = QCoreApplication::arguments();
    if (args.size() == 4 && args[1] == QLatin1String("--selftest"))
        return selftest(args[2], args[3]);

    const auto store = std::make_shared<ClipStore>();
    Session session(store);

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("clips"), new ClipImageProvider(store));
    qmlRegisterSingletonInstance("DataCollector", 1, 0, "Session", &session);
    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);
    engine.loadFromModule("data_collector", "Main");

    return QGuiApplication::exec();
}
