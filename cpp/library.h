#ifndef FFMPEG_PLAYGROUND_LIBRARY_H
#define FFMPEG_PLAYGROUND_LIBRARY_H

#include <opencv2/core/mat.hpp>

#include <expected>
#include <string>
#include <vector>

void hello();

// Decodes the video at `path` and returns one BGR frame for each sampling
// point t = 0, interval, 2*interval, ... within the video's duration.
// On I/O or decoding failure, returns an error message instead.
std::expected<std::vector<cv::Mat>, std::string>
extractFrames(const std::string& path, double intervalSeconds);

#endif // FFMPEG_PLAYGROUND_LIBRARY_H
