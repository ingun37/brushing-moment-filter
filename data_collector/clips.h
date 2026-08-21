#pragma once

#include <QImage>
#include <QString>
#include <expected>
#include <vector>

namespace clips {

// Target size of every stored/previewed clip frame ("contain" letterbox).
inline constexpr int kWidth = 320;
inline constexpr int kHeight = 180;

struct Clip {
    double startSeconds = 0.0;
    double fps = 30.0;                // playback rate for preview and encoding
    std::vector<QImage> frames;       // kWidth x kHeight RGB888, letterboxed
};

std::expected<double, QString> videoDurationSeconds(const QString &path);

// Decodes `count` clips of `lengthSeconds`, the i-th starting at
// startSeconds + i * intervalSeconds. Stops early at end of video, so the
// result may hold fewer than `count` clips (possibly zero).
std::expected<std::vector<Clip>, QString>
extractClips(const QString &path, double startSeconds, int count,
             double intervalSeconds, double lengthSeconds);

// Encodes one clip to an .mp4 (libx264 if available, else mpeg4).
std::expected<void, QString> encodeClip(const Clip &clip, const QString &outPath);

} // namespace clips
