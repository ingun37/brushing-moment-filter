#ifndef FFMPEG_PLAYGROUND_LIBRARY_H
#define FFMPEG_PLAYGROUND_LIBRARY_H

#include <opencv2/core/mat.hpp>

#include <concepts>
#include <expected>
#include <ranges>
#include <string>
#include <vector>

void hello();

// Returns true if `a` and `b` are the same size/type and their mean absolute
// per-pixel difference does not exceed `tolerance`.
bool framesSimilar(const cv::Mat& a, const cv::Mat& b, double tolerance);

// Copies `frames` into a vector, dropping each frame that is similar
// (per framesSimilar with `tolerance`) to the previous kept frame.
// Order is preserved.
template <std::ranges::input_range R>
    requires std::convertible_to<std::ranges::range_reference_t<R>, const cv::Mat&>
std::vector<cv::Mat> removeConsecutiveDuplicates(R&& frames, double tolerance)
{
    std::vector<cv::Mat> result;
    for (const cv::Mat& frame : frames) {
        if (result.empty() || !framesSimilar(result.back(), frame, tolerance))
            result.push_back(frame);
    }
    return result;
}

// Decodes the video at `path` and returns one BGR frame for each sampling
// point t = 0, interval, 2*interval, ... within the video's duration.
// On I/O or decoding failure, returns an error message instead.
std::expected<std::vector<cv::Mat>, std::string>
extractFrames(const std::string& path, double intervalSeconds);

#endif // FFMPEG_PLAYGROUND_LIBRARY_H
