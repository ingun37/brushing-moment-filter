#ifndef FFMPEG_PLAYGROUND_PIPELINE_H
#define FFMPEG_PLAYGROUND_PIPELINE_H

// Sender/receiver (P2300 / C++26 std::execution, via NVIDIA stdexec) wrappers
// around the FrameQueue pipeline stages. Each stage is a composable sender
// scheduled on a caller-provided scheduler; stages communicate through
// bounded FrameQueues, which provide the backpressure that senders alone do
// not model. Launch all stages of a pipeline with stdexec::when_all so they
// run concurrently; each stage blocks its pool thread on queue operations,
// so the pool must have at least one thread per stage.

#include "library.h"

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include <utility>

namespace pipeline {

// Decodes `path`, pushing sampled frames into `out` (see extractFramesToQueue).
// Value channel: std::expected<void, std::string> — the decode result.
inline stdexec::sender auto
extractStage(stdexec::scheduler auto scheduler, std::string path,
             double intervalSeconds, FrameQueue& out)
{
    return stdexec::schedule(scheduler)
         | stdexec::then([path = std::move(path), intervalSeconds, &out] {
               return extractFramesToQueue(path, intervalSeconds, out);
           });
}

// Forwards frames from `in` to `out`, dropping consecutive near-duplicates
// (see removeConsecutiveDuplicatesToQueue).
inline stdexec::sender auto
dedupStage(stdexec::scheduler auto scheduler, FrameQueue& in, FrameQueue& out,
           double tolerance)
{
    return stdexec::schedule(scheduler)
         | stdexec::then([&in, &out, tolerance] {
               removeConsecutiveDuplicatesToQueue(in, out, tolerance);
           });
}

// Buffers `batchSize` frames from `in`, forwards `sampleCount` of them at
// regular intervals, discards the rest, and repeats
// (see downsampleToQueue).
inline stdexec::sender auto
downsampleStage(stdexec::scheduler auto scheduler, FrameQueue& in, FrameQueue& out,
                std::size_t batchSize, std::size_t sampleCount)
{
    return stdexec::schedule(scheduler)
         | stdexec::then([&in, &out, batchSize, sampleCount] {
               downsampleToQueue(in, out, batchSize, sampleCount);
           });
}

// Generic stage for custom per-frame filters (e.g. a future hand-filtering UI
// thread): pops each frame from `in` and pushes it to `out` if
// `keep(frame)` returns true. Closes `out` when done; propagates an early
// close of `out` back to `in`, like the built-in stages.
inline stdexec::sender auto
filterStage(stdexec::scheduler auto scheduler, FrameQueue& in, FrameQueue& out,
            std::predicate<const cv::Mat&> auto keep)
{
    return stdexec::schedule(scheduler)
         | stdexec::then([&in, &out, keep = std::move(keep)] {
               struct CloseGuard {
                   FrameQueue& q;
                   ~CloseGuard() { q.close(); }
               } closeGuard{out};
               while (auto frame = in.pop()) {
                   if (!keep(*frame))
                       continue;
                   if (!out.push(std::move(*frame))) {
                       in.close();
                       return;
                   }
               }
           });
}

// Terminal stage: drains `in` into a vector.
// Value channel: std::vector<cv::Mat>.
inline stdexec::sender auto
collectStage(stdexec::scheduler auto scheduler, FrameQueue& in)
{
    return stdexec::schedule(scheduler)
         | stdexec::then([&in] {
               std::vector<cv::Mat> frames;
               while (auto frame = in.pop())
                   frames.push_back(std::move(*frame));
               return frames;
           });
}

// Convenience: runs decode -> dedup -> collect concurrently on an internal
// thread pool and returns the unique frames.
inline std::expected<std::vector<cv::Mat>, std::string>
extractUniqueFrames(const std::string& path, double intervalSeconds,
                    double tolerance, std::size_t queueCapacity = 2)
{
    exec::static_thread_pool pool(3); // one thread per stage
    auto scheduler = pool.get_scheduler();
    FrameQueue decoded(queueCapacity);
    FrameQueue unique(queueCapacity);

    auto [extracted, frames] = stdexec::sync_wait(
        stdexec::when_all(
            extractStage(scheduler, path, intervalSeconds, decoded),
            dedupStage(scheduler, decoded, unique, tolerance),
            collectStage(scheduler, unique))).value();

    if (!extracted)
        return std::unexpected(extracted.error());
    return std::move(frames);
}

} // namespace pipeline

#endif // FFMPEG_PLAYGROUND_PIPELINE_H
