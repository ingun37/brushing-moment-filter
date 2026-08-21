#include "session.h"

#include "clips.h"
#include "md5.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QtConcurrent>

#include <algorithm>
#include <ranges>

using namespace Qt::StringLiterals;

namespace {

QString appDataRoot()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString logPathFor(const QString &md5)
{
    return appDataRoot() + "/sessions/" + md5 + ".json";
}

QString clipsDirFor(const QString &md5)
{
    return appDataRoot() + "/clips/" + md5;
}

QJsonObject readLogFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

std::expected<void, QString> writeLogFile(const QString &path, const QJsonObject &log)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return std::unexpected(QStringLiteral("Cannot write %1: %2").arg(path, file.errorString()));
    file.write(QJsonDocument(log).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return std::unexpected(QStringLiteral("Cannot write %1: %2").arg(path, file.errorString()));
    return {};
}

QString clipFileName(double startSeconds)
{
    // Zero-padded milliseconds keep the directory sorted by time.
    return QStringLiteral("%1.mp4").arg(qRound64(startSeconds * 1000.0), 10, 10, QChar(u'0'));
}

} // namespace

Session::Session(std::shared_ptr<ClipStore> store, QObject *parent)
    : QObject(parent), store_(std::move(store))
{
}

QString Session::outputDir() const
{
    return md5_.isEmpty() ? QString() : clipsDirFor(md5_);
}

void Session::setSamplingInterval(double value)
{
    if (value > 0 && value != samplingInterval_) {
        samplingInterval_ = value;
        emit paramsChanged();
    }
}

void Session::setSampleLength(double value)
{
    if (value > 0 && value != sampleLength_) {
        sampleLength_ = value;
        emit paramsChanged();
    }
}

void Session::setSampleSize(int value)
{
    if (value > 0 && value != sampleSize_) {
        sampleSize_ = value;
        emit paramsChanged();
    }
}

void Session::setCursor(double value)
{
    value = std::clamp(value, 0.0, std::max(0.0, duration_));
    if (value != cursor_) {
        cursor_ = value;
        emit cursorChanged();
    }
}

void Session::loadVideo(const QUrl &url)
{
    if (busy_)
        return;
    const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
    setBusy(true, tr("Hashing video (MD5)…"));

    (void)QtConcurrent::run([this, path] {
        const auto md5 = computeFileMd5(path);
        const auto duration =
            md5 ? clips::videoDurationSeconds(path) : std::expected<double, QString>(0.0);
        QMetaObject::invokeMethod(
            this,
            [this, path, md5, duration] {
                if (!md5)
                    return fail(md5.error());
                if (!duration)
                    return fail(duration.error());

                videoPath_ = path;
                md5_ = *md5;
                duration_ = *duration;
                videoLoaded_ = true;
                atEnd_ = false;
                cursor_ = 0.0;
                hasPreviousProgress_ = false;

                // SPEC: check the log in the app directory for previous progress.
                if (const QJsonObject log = readLogFile(logPathFor(md5_)); !log.isEmpty()) {
                    hasPreviousProgress_ = true;
                    cursor_ = std::clamp(log[u"cursor"].toDouble(), 0.0, duration_);
                    if (const double v = log[u"samplingInterval"].toDouble(); v > 0)
                        samplingInterval_ = v;
                    if (const double v = log[u"sampleLength"].toDouble(); v > 0)
                        sampleLength_ = v;
                    if (const int v = log[u"sampleSize"].toInt(); v > 0)
                        sampleSize_ = v;
                }
                setBusy(false);
                emit videoInfoChanged();
                emit paramsChanged();
                emit cursorChanged();
            },
            Qt::QueuedConnection);
    });
}

void Session::startCollecting()
{
    if (busy_ || !videoLoaded_)
        return;
    extractBatchAsync();
}

void Session::resample()
{
    if (busy_ || !videoLoaded_)
        return;
    extractBatchAsync();
}

void Session::extractBatchAsync()
{
    setBusy(true, tr("Extracting clips…"));
    const QString path = videoPath_;
    const double start = cursor_;
    const double interval = samplingInterval_;
    const double length = sampleLength_;
    const int count = sampleSize_;

    (void)QtConcurrent::run([this, path, start, interval, length, count] {
        auto extracted = clips::extractClips(path, start, count, interval, length);
        ClipStore::Batch batch;
        QString error;
        if (extracted)
            batch = std::make_shared<const std::vector<clips::Clip>>(std::move(*extracted));
        else
            error = extracted.error();

        QMetaObject::invokeMethod(
            this,
            [this, batch, error, start, interval] {
                if (!error.isEmpty())
                    return fail(error);

                batch_ = batch;
                batchStart_ = start;
                batchInterval_ = interval;
                store_->set(batch);
                ++epoch_;
                atEnd_ = batch->empty();
                if (atEnd_) {
                    // SPEC "Termination": everything is already saved; just record it.
                    QJsonObject log = readLogFile(logPathFor(md5_));
                    log[u"finished"] = true;
                    if (auto written = writeLogFile(logPathFor(md5_), log); !written)
                        return fail(written.error());
                }
                rebuildClipModel();
                setBusy(false);
                emit batchChanged();
            },
            Qt::QueuedConnection);
    });
}

void Session::submitSelection(const QVariantList &selectedIndices)
{
    if (busy_ || !batch_ || batch_->empty())
        return;

    QSet<int> selected;
    for (const QVariant &index : selectedIndices)
        selected.insert(index.toInt());

    setBusy(true, tr("Saving clips…"));
    const auto batch = batch_;
    const QString md5 = md5_;
    const QString videoPath = videoPath_;
    const double newCursor =
        std::min(batchStart_ + batch->size() * batchInterval_, duration_ + batchInterval_);
    const double interval = samplingInterval_;
    const double length = sampleLength_;
    const int size = sampleSize_;

    (void)QtConcurrent::run([this, batch, selected, md5, videoPath, newCursor, interval, length,
                             size] {
        QString error;
        QJsonArray labeled;
        for (size_t index = 0; index < batch->size(); ++index) {
            const clips::Clip &clip = (*batch)[index];
            const bool positive = selected.contains(static_cast<int>(index));
            const QString dir =
                clipsDirFor(md5) + (positive ? u"/positive"_s : u"/negative"_s);
            if (!QDir().mkpath(dir)) {
                error = tr("Cannot create %1").arg(dir);
                break;
            }
            const QString file = dir + u'/' + clipFileName(clip.startSeconds);
            if (auto encoded = clips::encodeClip(clip, file); !encoded) {
                error = encoded.error();
                break;
            }
            labeled.append(QJsonObject{{u"start"_s, clip.startSeconds},
                                       {u"label"_s, positive ? u"positive"_s : u"negative"_s},
                                       {u"file"_s, file}});
        }

        if (error.isEmpty()) {
            // SPEC: log labeling and progress status every iteration.
            QJsonObject log = readLogFile(logPathFor(md5));
            log[u"md5"] = md5;
            log[u"videoPath"] = videoPath;
            log[u"cursor"] = newCursor;
            log[u"samplingInterval"] = interval;
            log[u"sampleLength"] = length;
            log[u"sampleSize"] = size;
            log[u"finished"] = false;
            QJsonArray iterations = log[u"iterations"].toArray();
            iterations.append(QJsonObject{
                {u"labeledAt"_s, QDateTime::currentDateTime().toString(Qt::ISODate)},
                {u"clips"_s, labeled}});
            log[u"iterations"] = iterations;
            if (auto written = writeLogFile(logPathFor(md5), log); !written)
                error = written.error();
        }

        QMetaObject::invokeMethod(
            this,
            [this, error, newCursor] {
                if (!error.isEmpty())
                    return fail(error);
                cursor_ = newCursor;
                emit cursorChanged();
                extractBatchAsync(); // next iteration (or termination if past the end)
            },
            Qt::QueuedConnection);
    });
}

void Session::reset()
{
    if (busy_)
        return;
    videoPath_.clear();
    md5_.clear();
    duration_ = 0.0;
    videoLoaded_ = false;
    hasPreviousProgress_ = false;
    cursor_ = 0.0;
    batch_.reset();
    store_->set(nullptr);
    clipModel_.clear();
    ++epoch_;
    atEnd_ = false;
    lastError_.clear();
    emit videoInfoChanged();
    emit cursorChanged();
    emit batchChanged();
    emit lastErrorChanged();
}

void Session::rebuildClipModel()
{
    clipModel_.clear();
    if (batch_) {
        for (const clips::Clip &clip : *batch_) {
            const double fps = clip.fps > 0 ? clip.fps : 30.0;
            clipModel_.append(QVariantMap{{u"startSeconds"_s, clip.startSeconds},
                                          {u"frameCount"_s, int(clip.frames.size())},
                                          {u"frameIntervalMs"_s, int(1000.0 / fps)}});
        }
    }
}

void Session::setBusy(bool busy, const QString &message)
{
    busy_ = busy;
    busyMessage_ = message;
    emit busyChanged();
    if (busy && !lastError_.isEmpty()) {
        lastError_.clear();
        emit lastErrorChanged();
    }
}

void Session::fail(const QString &error)
{
    lastError_ = error;
    setBusy(false);
    emit lastErrorChanged();
}
