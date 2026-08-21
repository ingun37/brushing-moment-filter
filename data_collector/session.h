#pragma once

#include "clipstore.h"

#include <QObject>
#include <QUrl>
#include <QVariantList>
#include <memory>

// Application state machine driving both QML scenes. All heavy work (MD5,
// decode, encode) runs on QtConcurrent worker threads; results are marshalled
// back to the GUI thread.
class Session : public QObject
{
    Q_OBJECT
    // Loaded video
    Q_PROPERTY(QString videoPath READ videoPath NOTIFY videoInfoChanged)
    Q_PROPERTY(QString md5 READ md5 NOTIFY videoInfoChanged)
    Q_PROPERTY(double durationSeconds READ durationSeconds NOTIFY videoInfoChanged)
    Q_PROPERTY(bool videoLoaded READ videoLoaded NOTIFY videoInfoChanged)
    Q_PROPERTY(bool hasPreviousProgress READ hasPreviousProgress NOTIFY videoInfoChanged)
    Q_PROPERTY(QString outputDir READ outputDir NOTIFY videoInfoChanged)
    // Sampling states (SPEC "Collect Scene")
    Q_PROPERTY(double samplingInterval READ samplingInterval WRITE setSamplingInterval NOTIFY paramsChanged)
    Q_PROPERTY(double sampleLength READ sampleLength WRITE setSampleLength NOTIFY paramsChanged)
    Q_PROPERTY(int sampleSize READ sampleSize WRITE setSampleSize NOTIFY paramsChanged)
    Q_PROPERTY(double cursor READ cursor WRITE setCursor NOTIFY cursorChanged)
    // Current batch / activity
    Q_PROPERTY(QVariantList clipModel READ clipModel NOTIFY batchChanged)
    Q_PROPERTY(int epoch READ epoch NOTIFY batchChanged)
    Q_PROPERTY(bool atEnd READ atEnd NOTIFY batchChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyMessage READ busyMessage NOTIFY busyChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)

public:
    explicit Session(std::shared_ptr<ClipStore> store, QObject *parent = nullptr);

    QString videoPath() const { return videoPath_; }
    QString md5() const { return md5_; }
    double durationSeconds() const { return duration_; }
    bool videoLoaded() const { return videoLoaded_; }
    bool hasPreviousProgress() const { return hasPreviousProgress_; }
    QString outputDir() const;

    double samplingInterval() const { return samplingInterval_; }
    void setSamplingInterval(double value);
    double sampleLength() const { return sampleLength_; }
    void setSampleLength(double value);
    int sampleSize() const { return sampleSize_; }
    void setSampleSize(int value);
    double cursor() const { return cursor_; }
    void setCursor(double value);

    QVariantList clipModel() const { return clipModel_; }
    int epoch() const { return epoch_; }
    bool atEnd() const { return atEnd_; }
    bool busy() const { return busy_; }
    QString busyMessage() const { return busyMessage_; }
    QString lastError() const { return lastError_; }

    Q_INVOKABLE void loadVideo(const QUrl &url);
    Q_INVOKABLE void startCollecting();
    // Re-extracts at the current cursor/parameters without saving anything.
    Q_INVOKABLE void resample();
    // Saves the batch (selected = positive, rest = negative), logs it,
    // advances the cursor and extracts the next batch.
    Q_INVOKABLE void submitSelection(const QVariantList &selectedIndices);
    Q_INVOKABLE void reset();

signals:
    void videoInfoChanged();
    void paramsChanged();
    void cursorChanged();
    void batchChanged();
    void busyChanged();
    void lastErrorChanged();

private:
    void extractBatchAsync();
    void setBusy(bool busy, const QString &message = {});
    void fail(const QString &error);
    void rebuildClipModel();

    std::shared_ptr<ClipStore> store_;

    QString videoPath_;
    QString md5_;
    double duration_ = 0.0;
    bool videoLoaded_ = false;
    bool hasPreviousProgress_ = false;

    double samplingInterval_ = 5.0;
    double sampleLength_ = 1.0;
    int sampleSize_ = 12;
    double cursor_ = 0.0;

    ClipStore::Batch batch_;
    double batchStart_ = 0.0;
    double batchInterval_ = 5.0;
    QVariantList clipModel_;
    int epoch_ = 0;
    bool atEnd_ = false;

    bool busy_ = false;
    QString busyMessage_;
    QString lastError_;
};
