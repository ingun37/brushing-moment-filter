#include "library.h"

#include <opencv2/core.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <functional>
#include <iostream>
#include <memory>

void hello()
{
    std::cout << "Hello, World!" << std::endl;
}

bool framesSimilar(const cv::Mat& a, const cv::Mat& b, double tolerance)
{
    if (a.size() != b.size() || a.type() != b.type())
        return false;
    const double meanAbsDiff = cv::norm(a, b, cv::NORM_L1) / (a.total() * a.channels());
    return meanAbsDiff <= tolerance;
}

namespace {

std::unexpected<std::string> fail(const std::string& what, int err = 0)
{
    if (err < 0) {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(err, buf, sizeof(buf));
        return std::unexpected(what + ": " + buf);
    }
    return std::unexpected(what);
}

std::expected<cv::Mat, std::string> toBgrMat(const AVFrame* frame, SwsContext*& sws)
{
    sws = sws_getCachedContext(sws,
                               frame->width, frame->height,
                               static_cast<AVPixelFormat>(frame->format),
                               frame->width, frame->height, AV_PIX_FMT_BGR24,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws)
        return fail("sws_getCachedContext failed");

    cv::Mat mat(frame->height, frame->width, CV_8UC3);
    uint8_t* dstData[4] = {mat.data};
    int dstLinesize[4] = {static_cast<int>(mat.step)};
    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, dstData, dstLinesize);
    return mat;
}

struct SwsGuard {
    SwsContext* ctx = nullptr;
    ~SwsGuard() { sws_freeContext(ctx); }
};

// Decodes the video at `path` and passes each sampled BGR frame and its
// presentation time (seconds) to `sink`. Stops early if `sink` returns false.
std::expected<void, std::string>
decodeFrames(const std::string& path, double intervalSeconds,
             const std::function<bool(cv::Mat, double)>& sink)
{
    if (intervalSeconds <= 0)
        return fail("intervalSeconds must be positive");

    AVFormatContext* fmtRaw = nullptr;
    int err = avformat_open_input(&fmtRaw, path.c_str(), nullptr, nullptr);
    if (err < 0)
        return fail("avformat_open_input failed for " + path, err);
    std::unique_ptr<AVFormatContext, decltype([](AVFormatContext* p) { avformat_close_input(&p); })> fmt(fmtRaw);

    if ((err = avformat_find_stream_info(fmt.get(), nullptr)) < 0)
        return fail("avformat_find_stream_info failed", err);

    const AVCodec* codec = nullptr;
    int streamIndex = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (streamIndex < 0)
        return fail("no video stream found in " + path, streamIndex);
    const AVStream* stream = fmt->streams[streamIndex];

    std::unique_ptr<AVCodecContext, decltype([](AVCodecContext* p) { avcodec_free_context(&p); })>
        codecCtx(avcodec_alloc_context3(codec));
    if (!codecCtx)
        return fail("avcodec_alloc_context3 failed");
    if ((err = avcodec_parameters_to_context(codecCtx.get(), stream->codecpar)) < 0)
        return fail("avcodec_parameters_to_context failed", err);
    if ((err = avcodec_open2(codecCtx.get(), codec, nullptr)) < 0)
        return fail("avcodec_open2 failed", err);

    std::unique_ptr<AVPacket, decltype([](AVPacket* p) { av_packet_free(&p); })> packet(av_packet_alloc());
    std::unique_ptr<AVFrame, decltype([](AVFrame* p) { av_frame_free(&p); })> frame(av_frame_alloc());
    if (!packet || !frame)
        return fail("allocation failed");

    SwsGuard sws;
    bool stopped = false;
    double nextSampleTime = 0.0;
    const double timeBase = av_q2d(stream->time_base);

    auto handleFrame = [&](const AVFrame* f) -> std::expected<void, std::string> {
        int64_t pts = f->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE)
            pts = f->pts;
        const double t = (pts == AV_NOPTS_VALUE) ? nextSampleTime : pts * timeBase;
        if (t >= nextSampleTime) {
            auto mat = toBgrMat(f, sws.ctx);
            if (!mat)
                return std::unexpected(mat.error());
            if (!sink(std::move(*mat), t)) {
                stopped = true;
                return {};
            }
            nextSampleTime += intervalSeconds;
        }
        return {};
    };

    auto drainDecoder = [&]() -> std::expected<void, std::string> {
        while (!stopped && (err = avcodec_receive_frame(codecCtx.get(), frame.get())) >= 0) {
            if (auto ok = handleFrame(frame.get()); !ok)
                return ok;
        }
        if (!stopped && err != AVERROR(EAGAIN) && err != AVERROR_EOF)
            return fail("avcodec_receive_frame failed", err);
        return {};
    };

    while (!stopped && (err = av_read_frame(fmt.get(), packet.get())) >= 0) {
        if (packet->stream_index == streamIndex) {
            if ((err = avcodec_send_packet(codecCtx.get(), packet.get())) < 0)
                return fail("avcodec_send_packet failed", err);
            if (auto ok = drainDecoder(); !ok)
                return std::unexpected(ok.error());
        }
        av_packet_unref(packet.get());
    }
    if (stopped)
        return {};
    if (err != AVERROR_EOF)
        return fail("av_read_frame failed", err);

    avcodec_send_packet(codecCtx.get(), nullptr); // flush
    return drainDecoder();
}

} // namespace

std::expected<std::vector<cv::Mat>, std::string>
extractFrames(const std::string& path, double intervalSeconds)
{
    std::vector<cv::Mat> result;
    auto ok = decodeFrames(path, intervalSeconds, [&](cv::Mat frame, double) {
        result.push_back(std::move(frame));
        return true;
    });
    if (!ok)
        return std::unexpected(ok.error());
    return result;
}

std::expected<void, std::string>
extractFramesToQueue(const std::string& path, double intervalSeconds, FrameQueue& queue)
{
    struct CloseGuard {
        FrameQueue& q;
        ~CloseGuard() { q.close(); }
    } closeGuard{queue};

    return decodeFrames(path, intervalSeconds, [&](cv::Mat frame, double t) {
        return queue.push({std::move(frame), t});
    });
}

void removeConsecutiveDuplicatesToQueue(FrameQueue& input, FrameQueue& output,
                                        double tolerance)
{
    struct CloseGuard {
        FrameQueue& q;
        ~CloseGuard() { q.close(); }
    } closeGuard{output};

    cv::Mat lastKept;
    while (auto frame = input.pop()) {
        if (!lastKept.empty() && framesSimilar(lastKept, frame->image, tolerance))
            continue;
        lastKept = frame->image;
        if (!output.push(std::move(*frame))) {
            input.close(); // downstream gave up; stop the upstream producer too
            return;
        }
    }
}
