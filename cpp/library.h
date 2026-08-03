#ifndef FFMPEG_PLAYGROUND_LIBRARY_H
#define FFMPEG_PLAYGROUND_LIBRARY_H

#include <opencv2/core/mat.hpp>

#include <concepts>
#include <condition_variable>
#include <deque>
#include <expected>
#include <mutex>
#include <optional>
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

// Thread-safe bounded FIFO of frames. push() blocks while the queue is at
// capacity; pop() blocks while it is empty. close() unblocks everyone:
// subsequent pushes are rejected and pop() drains what is left, then
// returns nullopt.
class FrameQueue {
public:
    explicit FrameQueue(std::size_t capacity) : capacity_(capacity) {}

    // Blocks until there is room (or the queue is closed).
    // Returns false if the queue was closed, in which case the frame is dropped.
    bool push(cv::Mat frame)
    {
        std::unique_lock lock(mutex_);
        notFull_.wait(lock, [&] { return frames_.size() < capacity_ || closed_; });
        if (closed_)
            return false;
        frames_.push_back(std::move(frame));
        notEmpty_.notify_one();
        return true;
    }

    // Blocks until a frame is available; returns nullopt once the queue is
    // closed and drained.
    std::optional<cv::Mat> pop()
    {
        std::unique_lock lock(mutex_);
        notEmpty_.wait(lock, [&] { return !frames_.empty() || closed_; });
        if (frames_.empty())
            return std::nullopt;
        cv::Mat frame = std::move(frames_.front());
        frames_.pop_front();
        notFull_.notify_one();
        return frame;
    }

    void close()
    {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        notFull_.notify_all();
        notEmpty_.notify_all();
    }

    std::size_t size() const
    {
        std::lock_guard lock(mutex_);
        return frames_.size();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
    std::deque<cv::Mat> frames_;
    bool closed_ = false;
};

// Like extractFrames, but meant to run on a producer thread: each sampled
// frame is pushed into `queue`, blocking whenever the queue is full.
// The queue is always closed before returning (success, error, or when the
// consumer closes the queue early, which stops decoding).
std::expected<void, std::string>
extractFramesToQueue(const std::string& path, double intervalSeconds, FrameQueue& queue);

// Like removeConsecutiveDuplicates, but meant to run as a pipeline stage on
// its own thread: frames are popped from `input` and each frame that is not
// similar (per framesSimilar with `tolerance`) to the previously forwarded
// frame is pushed into `output`, blocking whenever `output` is full.
// Runs until `input` is closed and drained; `output` is always closed before
// returning. If the downstream consumer closes `output` early, `input` is
// closed too so the upstream producer stops.
void removeConsecutiveDuplicatesToQueue(FrameQueue& input, FrameQueue& output,
                                        double tolerance);

// Pipeline stage that downsamples in batches: buffers `batchSize` (N) frames
// from `input`, forwards the `sampleCount` (M <= N) frames at regular
// intervals within the batch (indices i*N/M for i = 0..M-1), discards the
// rest, and repeats. A final partial batch uses the same index pattern,
// truncated to the frames that exist. Runs until `input` is closed and
// drained; `output` is always closed before returning. If the downstream
// consumer closes `output` early, `input` is closed too.
void downsampleToQueue(FrameQueue& input, FrameQueue& output,
                       std::size_t batchSize, std::size_t sampleCount);

#endif // FFMPEG_PLAYGROUND_LIBRARY_H
