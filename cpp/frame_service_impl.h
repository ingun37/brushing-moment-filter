#ifndef FFMPEG_PLAYGROUND_FRAME_SERVICE_IMPL_H
#define FFMPEG_PLAYGROUND_FRAME_SERVICE_IMPL_H

#include "frame_service.grpc.pb.h"

#include <chrono>
#include <cstddef>

struct FrameServerOptions {
    double intervalSeconds = 0.001; // decode (almost) every frame
    double tolerance = 10.0;
    std::size_t batchN = 1;
    std::size_t sampleM = 1;
    std::chrono::seconds timeout{60};
};

// Implements the FrameService contract (see frame_service.proto): receives
// the uploaded video, pipelines it through extractFramesToQueue ->
// removeConsecutiveDuplicatesToQueue -> downsampleToQueue, sends the first
// frame, then one frame per Next request until EndOfFile or Stop. A session
// with no client message for `timeout` is cancelled.
class FrameServiceImpl final : public frameservice::FrameService::Service {
public:
    explicit FrameServiceImpl(FrameServerOptions opts) : opts_(opts) {}

    grpc::Status Session(grpc::ServerContext* context,
                         grpc::ServerReaderWriter<frameservice::ServerMessage,
                                                  frameservice::ClientMessage>* stream)
        override;

private:
    const FrameServerOptions opts_;
};

#endif // FFMPEG_PLAYGROUND_FRAME_SERVICE_IMPL_H
