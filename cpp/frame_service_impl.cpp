#include "frame_service_impl.h"

#include "library.h"

#include <opencv2/imgcodecs.hpp>

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

namespace {

// Cancels the session when the client stays silent for too long. The sync
// gRPC API has no timed Read, so a watchdog thread calls TryCancel() to
// unblock it: arm() before each Read, disarm() after.
class IdleWatchdog {
public:
    IdleWatchdog(grpc::ServerContext& context, std::chrono::seconds timeout)
        : context_(context), timeout_(timeout)
    {
        thread_ = std::jthread([this](std::stop_token stop) {
            std::unique_lock lock(mutex_);
            while (!stop.stop_requested()) {
                if (!armed_) {
                    cv_.wait(lock, [&] { return armed_ || stop.stop_requested(); });
                    continue;
                }
                if (cv_.wait_until(lock, deadline_,
                                   [&] { return !armed_ || stop.stop_requested(); }))
                    continue;
                std::cerr << "session timed out\n";
                context_.TryCancel();
                return;
            }
        });
    }
    ~IdleWatchdog()
    {
        thread_.request_stop();
        cv_.notify_all();
    }

    void arm()
    {
        std::lock_guard lock(mutex_);
        deadline_ = std::chrono::steady_clock::now() + timeout_;
        armed_ = true;
        cv_.notify_all();
    }
    void disarm()
    {
        std::lock_guard lock(mutex_);
        armed_ = false;
        cv_.notify_all();
    }

private:
    grpc::ServerContext& context_;
    const std::chrono::seconds timeout_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::chrono::steady_clock::time_point deadline_;
    bool armed_ = false;
    std::jthread thread_;
};

using Stream = grpc::ServerReaderWriter<frameservice::ServerMessage,
                                        frameservice::ClientMessage>;

// Reads one client message, enforcing the idle timeout.
bool timedRead(Stream& stream, IdleWatchdog& watchdog,
               frameservice::ClientMessage& message)
{
    watchdog.arm();
    const bool ok = stream.Read(&message);
    watchdog.disarm();
    return ok;
}

struct ReceivedVideo {
    std::filesystem::path path;
    double startSeconds = 0.0; // from the first chunk; where sampling resumes
};

std::expected<ReceivedVideo, std::string>
receiveVideo(Stream& stream, IdleWatchdog& watchdog)
{
    const auto path =
        std::filesystem::temp_directory_path() /
        ("frame_server_" + std::to_string(::getpid()) + "_" +
         std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
         ".mp4");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return std::unexpected("failed to open temp file " + path.string());

    frameservice::ClientMessage message;
    double startSeconds = 0.0;
    bool first = true;
    while (true) {
        if (!timedRead(stream, watchdog, message))
            return std::unexpected("client went away mid-upload");
        if (!message.has_chunk())
            return std::unexpected("expected a VideoChunk during upload");
        const auto& chunk = message.chunk();
        if (first) {
            if (chunk.start_seconds() < 0)
                return std::unexpected("start_seconds must not be negative");
            startSeconds = chunk.start_seconds();
            first = false;
        }
        out.write(chunk.data().data(),
                  static_cast<std::streamsize>(chunk.data().size()));
        if (chunk.last())
            break;
    }
    if (!out.flush())
        return std::unexpected("failed to write temp file " + path.string());
    return ReceivedVideo{path, startSeconds};
}

// Pops up to `count` frames and sends them as one Frames batch. Returns the
// number of frames sent, or unexpected on a write/encode failure.
std::expected<std::size_t, std::string>
sendFrames(Stream& stream, FrameQueue& queue, std::size_t count)
{
    frameservice::ServerMessage message;
    auto& frames = *message.mutable_frames();
    for (std::size_t i = 0; i < count; ++i) {
        auto frame = queue.pop();
        if (!frame) // pipeline drained mid-batch: send what we have
            break;
        std::vector<uchar> png;
        if (!cv::imencode(".png", frame->image, png))
            return std::unexpected("png encoding failed");
        frames.add_pngs(png.data(), png.size());
        frames.add_timestamps_seconds(frame->timestampSeconds);
    }
    if (frames.pngs_size() > 0 && !stream.Write(message))
        return std::unexpected("client went away");
    return static_cast<std::size_t>(frames.pngs_size());
}

} // namespace

grpc::Status FrameServiceImpl::Session(grpc::ServerContext* context, Stream* stream)
{
    IdleWatchdog watchdog(*context, opts_.timeout);

    const auto video = receiveVideo(*stream, watchdog);
    if (!video) {
        std::cerr << video.error() << "\n";
        return {grpc::StatusCode::INVALID_ARGUMENT, video.error()};
    }

    FrameQueue decoded(4);
    FrameQueue unique(4);

    std::expected<void, std::string> extracted;
    std::jthread extractThread([&] {
        extracted = extractFramesToQueue(video->path.string(), opts_.intervalSeconds,
                                         decoded, video->startSeconds);
    });
    std::jthread dedupThread([&] {
        removeConsecutiveDuplicatesToQueue(decoded, unique, opts_.tolerance);
    });

    grpc::Status status = grpc::Status::OK;

    // Send up to Next.count frames per Next, including the first one.
    while (true) {
        frameservice::ClientMessage request;
        if (!timedRead(*stream, watchdog, request) || request.has_stop())
            break; // stop, timeout, or disconnect
        if (!request.has_next()) {
            status = {grpc::StatusCode::INVALID_ARGUMENT, "expected Next or Stop"};
            break;
        }
        const auto count = std::max<std::size_t>(request.next().count(), 1);

        const auto sent = sendFrames(*stream, unique, count);
        if (!sent) {
            std::cerr << sent.error() << "\n";
            break;
        }
        if (*sent == 0) { // pipeline drained: report end of file and terminate
            if (!extracted) {
                status = {grpc::StatusCode::INVALID_ARGUMENT,
                          "decoding failed: " + extracted.error()};
                break;
            }
            frameservice::ServerMessage message;
            message.mutable_eof();
            stream->Write(message);
            break;
        }
    }

    // Unblock any still-running stage; close() propagates upstream.
    unique.close();
    decoded.close();
    extractThread.join();
    if (!extracted)
        std::cerr << "decoding failed: " << extracted.error() << "\n";

    std::error_code ec;
    std::filesystem::remove(video->path, ec);
    return status;
}
