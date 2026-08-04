#include "frame_service_impl.h"

#include <grpcpp/grpcpp.h>
#include <gtest/gtest.h>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace {

const std::string kResourceDir = TEST_RESOURCE_DIR;

// Mean absolute pixel difference between two same-sized BGR images.
double meanAbsDiff(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat diff;
    cv::absdiff(a, b, diff);
    return cv::mean(cv::mean(diff))[0];
}

// Runs a FrameServiceImpl on an in-process gRPC server (no TCP port).
class TestServer {
public:
    explicit TestServer(FrameServerOptions opts) : service_(opts) {
        grpc::ServerBuilder builder;
        builder.RegisterService(&service_);
        server_ = builder.BuildAndStart();
        stub_ = frameservice::FrameService::NewStub(
            server_->InProcessChannel(grpc::ChannelArguments{}));
    }
    ~TestServer() { server_->Shutdown(); }

    frameservice::FrameService::Stub& stub() { return *stub_; }

private:
    FrameServiceImpl service_;
    std::unique_ptr<grpc::Server> server_;
    std::unique_ptr<frameservice::FrameService::Stub> stub_;
};

using Stream = grpc::ClientReaderWriter<frameservice::ClientMessage,
                                        frameservice::ServerMessage>;

// Streams the file as VideoChunks, the final one flagged `last`. The first
// chunk carries the per-video pipeline parameters: interval 0.001 decodes
// every frame, tolerance 5 keeps adjacent digits of 12.mp4 distinct (they
// differ by as little as ~7.3).
void upload(Stream& stream, const std::string& path,
            double intervalSeconds = 0.001, double tolerance = 5.0) {
    std::ifstream in(path, std::ios::binary);
    ASSERT_TRUE(in) << path;
    std::vector<char> buf(1 << 16);
    frameservice::ClientMessage message;
    bool first = true;
    while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())),
           in.gcount() > 0) {
        message.mutable_chunk()->set_data(buf.data(),
                                          static_cast<std::size_t>(in.gcount()));
        message.mutable_chunk()->set_last(in.peek() == EOF);
        if (first) {
            message.mutable_chunk()->set_sample_interval_seconds(intervalSeconds);
            message.mutable_chunk()->set_dedup_tolerance(tolerance);
            first = false;
        }
        ASSERT_TRUE(stream.Write(message));
    }
}

void requestNext(Stream& stream, std::uint32_t count = 1) {
    frameservice::ClientMessage message;
    message.mutable_next()->set_count(count);
    ASSERT_TRUE(stream.Write(message));
}

cv::Mat decodePng(const std::string& png) {
    const cv::Mat bytes(1, static_cast<int>(png.size()), CV_8UC1,
                        const_cast<char*>(png.data()));
    return cv::imdecode(bytes, cv::IMREAD_COLOR);
}

} // namespace

TEST(FrameServiceTest, FullSessionYieldsOneFramePerNumberThenEof) {
    TestServer server({});
    grpc::ClientContext ctx;
    auto stream = server.stub().Session(&ctx);
    upload(*stream, kResourceDir + "/12.mp4");

    // Each Next yields one frame; a Next past the last frame yields eof.
    std::vector<cv::Mat> frames;
    frameservice::ServerMessage response;
    requestNext(*stream);
    while (stream->Read(&response) && !response.has_eof()) {
        ASSERT_TRUE(response.has_frames());
        ASSERT_EQ(response.frames().pngs_size(), 1);
        frames.push_back(decodePng(response.frames().pngs(0)));
        ASSERT_FALSE(frames.back().empty());
        requestNext(*stream);
    }
    EXPECT_TRUE(response.has_eof());
    stream->WritesDone();
    EXPECT_TRUE(stream->Finish().ok());

    ASSERT_EQ(frames.size(), 12u);
    for (int i = 0; i < 12; ++i) {
        const cv::Mat number =
            cv::imread(kResourceDir + "/" + std::to_string(i) + ".png");
        ASSERT_FALSE(number.empty());
        EXPECT_LT(meanAbsDiff(frames[i], number), 10.0) << "frame " << i;
    }
}

TEST(FrameServiceTest, NextCountBatchesFramesPerResponse) {
    TestServer server({});
    grpc::ClientContext ctx;
    auto stream = server.stub().Session(&ctx);
    upload(*stream, kResourceDir + "/12.mp4");

    // Ask for 5 frames at a time. 12 unique frames total -> batches of
    // 5, 5, 2, then eof.
    std::vector<int> batchSizes;
    std::vector<cv::Mat> frames;
    frameservice::ServerMessage response;
    requestNext(*stream, 5);
    while (stream->Read(&response) && !response.has_eof()) {
        ASSERT_TRUE(response.has_frames());
        batchSizes.push_back(response.frames().pngs_size());
        for (const auto& png : response.frames().pngs()) {
            frames.push_back(decodePng(png));
            ASSERT_FALSE(frames.back().empty());
        }
        requestNext(*stream, 5);
    }
    EXPECT_TRUE(response.has_eof());
    stream->WritesDone();
    EXPECT_TRUE(stream->Finish().ok());

    EXPECT_EQ(batchSizes, (std::vector<int>{5, 5, 2}));
    ASSERT_EQ(frames.size(), 12u);
    for (int i = 0; i < 12; ++i) {
        const cv::Mat number =
            cv::imread(kResourceDir + "/" + std::to_string(i) + ".png");
        ASSERT_FALSE(number.empty());
        EXPECT_LT(meanAbsDiff(frames[i], number), 10.0) << "frame " << i;
    }
}

TEST(FrameServiceTest, StopTerminatesSessionEarly) {
    TestServer server({});
    grpc::ClientContext ctx;
    auto stream = server.stub().Session(&ctx);
    upload(*stream, kResourceDir + "/12.mp4");
    requestNext(*stream);

    frameservice::ServerMessage response;
    ASSERT_TRUE(stream->Read(&response));
    ASSERT_TRUE(response.has_frames());

    frameservice::ClientMessage stop;
    stop.mutable_stop();
    ASSERT_TRUE(stream->Write(stop));
    stream->WritesDone();

    EXPECT_FALSE(stream->Read(&response)); // no eof, just termination
    EXPECT_TRUE(stream->Finish().ok());
}

TEST(FrameServiceTest, SilentClientIsCancelledAfterIdleTimeout) {
    FrameServerOptions opts;
    opts.timeout = std::chrono::seconds(1);
    TestServer server(opts);
    grpc::ClientContext ctx;
    auto stream = server.stub().Session(&ctx);
    upload(*stream, kResourceDir + "/12.mp4");
    requestNext(*stream);

    frameservice::ServerMessage response;
    ASSERT_TRUE(stream->Read(&response));
    ASSERT_TRUE(response.has_frames());

    // Send no request: the server must cancel the session after ~1 s.
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(stream->Read(&response));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_EQ(stream->Finish().error_code(), grpc::StatusCode::CANCELLED);
    EXPECT_GE(elapsed, std::chrono::milliseconds(900));
    EXPECT_LT(elapsed, std::chrono::seconds(10));
}

TEST(FrameServiceTest, NonChunkMessageDuringUploadIsRejected) {
    TestServer server({});
    grpc::ClientContext ctx;
    auto stream = server.stub().Session(&ctx);

    frameservice::ClientMessage message;
    message.mutable_next(); // upload must start with VideoChunks
    ASSERT_TRUE(stream->Write(message));
    stream->WritesDone();

    frameservice::ServerMessage response;
    EXPECT_FALSE(stream->Read(&response));
    EXPECT_EQ(stream->Finish().error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(FrameServiceTest, UndecodableVideoReportsError) {
    TestServer server({});
    grpc::ClientContext ctx;
    auto stream = server.stub().Session(&ctx);

    frameservice::ClientMessage message;
    message.mutable_chunk()->set_data("this is not an mp4 file");
    message.mutable_chunk()->set_last(true);
    ASSERT_TRUE(stream->Write(message));
    requestNext(*stream); // the decode error is reported in reply to a Next
    stream->WritesDone();

    frameservice::ServerMessage response;
    EXPECT_FALSE(stream->Read(&response)); // no frames, no eof
    const auto status = stream->Finish();
    EXPECT_EQ(status.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(status.error_message().find("decoding failed"), std::string::npos);
}
