#include "clips.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <ranges>

namespace clips {
namespace {

QString avErr(int code)
{
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

struct FormatInputCloser {
    void operator()(AVFormatContext *c) const { avformat_close_input(&c); }
};
struct CodecCtxCloser {
    void operator()(AVCodecContext *c) const { avcodec_free_context(&c); }
};
struct FrameCloser {
    void operator()(AVFrame *f) const { av_frame_free(&f); }
};
struct PacketCloser {
    void operator()(AVPacket *p) const { av_packet_free(&p); }
};
struct SwsCloser {
    void operator()(SwsContext *s) const { sws_freeContext(s); }
};

using FormatInputPtr = std::unique_ptr<AVFormatContext, FormatInputCloser>;
using CodecCtxPtr = std::unique_ptr<AVCodecContext, CodecCtxCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;
using SwsPtr = std::unique_ptr<SwsContext, SwsCloser>;

struct OpenVideo {
    FormatInputPtr fmt;
    CodecCtxPtr dec;
    AVStream *stream = nullptr;
    int streamIndex = -1;
    double startOffsetSeconds = 0.0; // stream start_time, subtracted everywhere
    double durationSeconds = 0.0;
    double fps = 30.0;
};

std::expected<OpenVideo, QString> openVideo(const QString &path)
{
    AVFormatContext *fmtRaw = nullptr;
    if (const int ret = avformat_open_input(&fmtRaw, path.toUtf8().constData(), nullptr, nullptr);
        ret < 0)
        return std::unexpected(QStringLiteral("Cannot open %1: %2").arg(path, avErr(ret)));
    OpenVideo v;
    v.fmt.reset(fmtRaw);

    if (const int ret = avformat_find_stream_info(v.fmt.get(), nullptr); ret < 0)
        return std::unexpected(QStringLiteral("No stream info: %1").arg(avErr(ret)));

    const AVCodec *decoder = nullptr;
    v.streamIndex = av_find_best_stream(v.fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (v.streamIndex < 0 || !decoder)
        return std::unexpected(QStringLiteral("No decodable video stream in %1").arg(path));
    v.stream = v.fmt->streams[v.streamIndex];

    v.dec.reset(avcodec_alloc_context3(decoder));
    if (!v.dec)
        return std::unexpected(QStringLiteral("avcodec_alloc_context3 failed"));
    if (const int ret = avcodec_parameters_to_context(v.dec.get(), v.stream->codecpar); ret < 0)
        return std::unexpected(avErr(ret));
    if (const int ret = avcodec_open2(v.dec.get(), decoder, nullptr); ret < 0)
        return std::unexpected(QStringLiteral("Cannot open decoder: %1").arg(avErr(ret)));

    if (v.stream->start_time != AV_NOPTS_VALUE)
        v.startOffsetSeconds = v.stream->start_time * av_q2d(v.stream->time_base);

    if (v.fmt->duration != AV_NOPTS_VALUE)
        v.durationSeconds = static_cast<double>(v.fmt->duration) / AV_TIME_BASE;
    else if (v.stream->duration != AV_NOPTS_VALUE)
        v.durationSeconds = v.stream->duration * av_q2d(v.stream->time_base);
    if (v.durationSeconds <= 0)
        return std::unexpected(QStringLiteral("Cannot determine duration of %1").arg(path));

    const AVRational fr = av_guess_frame_rate(v.fmt.get(), v.stream, nullptr);
    if (fr.num > 0 && fr.den > 0) {
        const double fps = av_q2d(fr);
        if (fps > 0.5 && fps <= 240.0)
            v.fps = fps;
    }
    return v;
}

// Scales a decoded frame into a black kWidth x kHeight canvas, preserving
// aspect ratio ("contain"). The SwsContext is cached across calls.
std::expected<QImage, QString> letterboxFrame(const AVFrame *frame, SwsPtr &sws,
                                              int &cachedW, int &cachedH, int &cachedFmt)
{
    QImage image(kWidth, kHeight, QImage::Format_RGB888);
    image.fill(Qt::black);

    const double scale = std::min(double(kWidth) / frame->width, double(kHeight) / frame->height);
    const int outW = std::clamp(int(std::lround(frame->width * scale)) & ~1, 2, kWidth);
    const int outH = std::clamp(int(std::lround(frame->height * scale)) & ~1, 2, kHeight);
    const int xOff = (kWidth - outW) / 2;
    const int yOff = (kHeight - outH) / 2;

    if (!sws || cachedW != frame->width || cachedH != frame->height || cachedFmt != frame->format) {
        sws.reset(sws_getContext(frame->width, frame->height,
                                 static_cast<AVPixelFormat>(frame->format), outW, outH,
                                 AV_PIX_FMT_RGB24, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!sws)
            return std::unexpected(QStringLiteral("sws_getContext failed"));
        cachedW = frame->width;
        cachedH = frame->height;
        cachedFmt = frame->format;
    }

    uint8_t *dstData[4] = {image.bits() + yOff * image.bytesPerLine() + xOff * 3, nullptr, nullptr,
                           nullptr};
    const int dstStride[4] = {int(image.bytesPerLine()), 0, 0, 0};
    sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height, dstData, dstStride);
    return image;
}

} // namespace

std::expected<double, QString> videoDurationSeconds(const QString &path)
{
    return openVideo(path).transform([](const OpenVideo &v) { return v.durationSeconds; });
}

std::expected<std::vector<Clip>, QString>
extractClips(const QString &path, double startSeconds, int count, double intervalSeconds,
             double lengthSeconds)
{
    if (count <= 0 || intervalSeconds <= 0 || lengthSeconds <= 0)
        return std::unexpected(QStringLiteral("Invalid sampling parameters"));

    auto opened = openVideo(path);
    if (!opened)
        return std::unexpected(opened.error());
    OpenVideo &v = *opened;

    const FramePtr frame{av_frame_alloc()};
    const PacketPtr packet{av_packet_alloc()};
    if (!frame || !packet)
        return std::unexpected(QStringLiteral("Out of memory"));

    SwsPtr sws;
    int cachedW = -1, cachedH = -1, cachedFmt = -1;
    // Never keep more frames than the clip length implies (+ slack for ragged timestamps).
    const auto maxFrames = static_cast<size_t>(std::ceil(lengthSeconds * v.fps)) + 2;

    std::vector<Clip> result;
    for (const int i : std::views::iota(0, count)) {
        const double clipStart = startSeconds + i * intervalSeconds;
        if (clipStart >= v.durationSeconds)
            break;

        const auto seekTarget =
            static_cast<int64_t>((clipStart + v.startOffsetSeconds) * AV_TIME_BASE);
        if (const int ret = av_seek_frame(v.fmt.get(), -1, seekTarget, AVSEEK_FLAG_BACKWARD);
            ret < 0)
            return std::unexpected(
                QStringLiteral("Seek to %1 s failed: %2").arg(clipStart).arg(avErr(ret)));
        avcodec_flush_buffers(v.dec.get());

        Clip clip{.startSeconds = clipStart, .fps = v.fps, .frames = {}};
        bool done = false;
        bool eof = false;
        while (!done && !eof) {
            int ret = av_read_frame(v.fmt.get(), packet.get());
            if (ret == AVERROR_EOF) {
                eof = true;
                avcodec_send_packet(v.dec.get(), nullptr); // flush decoder
            } else if (ret < 0) {
                return std::unexpected(QStringLiteral("Read error: %1").arg(avErr(ret)));
            } else {
                if (packet->stream_index != v.streamIndex) {
                    av_packet_unref(packet.get());
                    continue;
                }
                ret = avcodec_send_packet(v.dec.get(), packet.get());
                av_packet_unref(packet.get());
                if (ret < 0 && ret != AVERROR(EAGAIN))
                    return std::unexpected(QStringLiteral("Decode error: %1").arg(avErr(ret)));
            }

            while (true) {
                ret = avcodec_receive_frame(v.dec.get(), frame.get());
                if (ret == AVERROR(EAGAIN))
                    break;
                if (ret == AVERROR_EOF) {
                    done = true;
                    break;
                }
                if (ret < 0)
                    return std::unexpected(QStringLiteral("Decode error: %1").arg(avErr(ret)));

                const int64_t pts = frame->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE) {
                    av_frame_unref(frame.get());
                    continue;
                }
                const double t = pts * av_q2d(v.stream->time_base) - v.startOffsetSeconds;
                if (t >= clipStart + lengthSeconds || clip.frames.size() >= maxFrames) {
                    done = true;
                    av_frame_unref(frame.get());
                    break;
                }
                if (t >= clipStart - 0.5 / v.fps) {
                    auto image = letterboxFrame(frame.get(), sws, cachedW, cachedH, cachedFmt);
                    if (!image)
                        return std::unexpected(image.error());
                    clip.frames.push_back(std::move(*image));
                }
                av_frame_unref(frame.get());
            }
        }
        // An empty clip means the decodable video ended before this position;
        // stop here so batch size stays a reliable measure of progress.
        if (clip.frames.empty())
            break;
        result.push_back(std::move(clip));
    }
    return result;
}

std::expected<void, QString> encodeClip(const Clip &clip, const QString &outPath)
{
    if (clip.frames.empty())
        return std::unexpected(QStringLiteral("Clip has no frames"));

    AVFormatContext *outRaw = nullptr;
    if (const int ret = avformat_alloc_output_context2(&outRaw, nullptr, "mp4",
                                                       outPath.toUtf8().constData());
        ret < 0)
        return std::unexpected(QStringLiteral("Cannot create %1: %2").arg(outPath, avErr(ret)));
    struct OutputCloser {
        void operator()(AVFormatContext *c) const
        {
            if (!(c->oformat->flags & AVFMT_NOFILE))
                avio_closep(&c->pb);
            avformat_free_context(c);
        }
    };
    const std::unique_ptr<AVFormatContext, OutputCloser> out{outRaw};

    const AVCodec *codec = avcodec_find_encoder_by_name("libx264");
    if (!codec)
        codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec)
        return std::unexpected(QStringLiteral("No H.264/MPEG-4 encoder in this FFmpeg build"));

    CodecCtxPtr enc{avcodec_alloc_context3(codec)};
    if (!enc)
        return std::unexpected(QStringLiteral("avcodec_alloc_context3 failed"));

    const AVRational fps = av_d2q(clip.fps, 100000);
    enc->width = kWidth;
    enc->height = kHeight;
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->time_base = av_inv_q(fps);
    enc->framerate = fps;
    enc->gop_size = 12;
    if (std::strcmp(codec->name, "libx264") == 0) {
        av_opt_set(enc->priv_data, "preset", "veryfast", 0);
        av_opt_set(enc->priv_data, "crf", "20", 0);
    } else {
        enc->bit_rate = 1'500'000;
    }
    if (out->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if (const int ret = avcodec_open2(enc.get(), codec, nullptr); ret < 0)
        return std::unexpected(QStringLiteral("Cannot open encoder: %1").arg(avErr(ret)));

    AVStream *stream = avformat_new_stream(out.get(), nullptr);
    if (!stream)
        return std::unexpected(QStringLiteral("avformat_new_stream failed"));
    stream->time_base = enc->time_base;
    if (const int ret = avcodec_parameters_from_context(stream->codecpar, enc.get()); ret < 0)
        return std::unexpected(avErr(ret));

    if (!(out->oformat->flags & AVFMT_NOFILE)) {
        if (const int ret = avio_open(&out->pb, outPath.toUtf8().constData(), AVIO_FLAG_WRITE);
            ret < 0)
            return std::unexpected(QStringLiteral("Cannot write %1: %2").arg(outPath, avErr(ret)));
    }
    if (const int ret = avformat_write_header(out.get(), nullptr); ret < 0)
        return std::unexpected(QStringLiteral("Header write failed: %1").arg(avErr(ret)));

    const FramePtr yuv{av_frame_alloc()};
    const PacketPtr packet{av_packet_alloc()};
    if (!yuv || !packet)
        return std::unexpected(QStringLiteral("Out of memory"));
    yuv->width = kWidth;
    yuv->height = kHeight;
    yuv->format = AV_PIX_FMT_YUV420P;
    if (const int ret = av_frame_get_buffer(yuv.get(), 0); ret < 0)
        return std::unexpected(avErr(ret));

    const SwsPtr sws{sws_getContext(kWidth, kHeight, AV_PIX_FMT_RGB24, kWidth, kHeight,
                                    AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr)};
    if (!sws)
        return std::unexpected(QStringLiteral("sws_getContext failed"));

    const auto drainPackets = [&](AVCodecContext *encoder) -> std::expected<void, QString> {
        while (true) {
            const int ret = avcodec_receive_packet(encoder, packet.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                return {};
            if (ret < 0)
                return std::unexpected(QStringLiteral("Encode error: %1").arg(avErr(ret)));
            av_packet_rescale_ts(packet.get(), enc->time_base, stream->time_base);
            packet->stream_index = stream->index;
            if (const int wret = av_interleaved_write_frame(out.get(), packet.get()); wret < 0)
                return std::unexpected(QStringLiteral("Write failed: %1").arg(avErr(wret)));
        }
    };

    for (size_t index = 0; index < clip.frames.size(); ++index) {
        const QImage &image = clip.frames[index];
        if (const int ret = av_frame_make_writable(yuv.get()); ret < 0)
            return std::unexpected(avErr(ret));
        const uint8_t *srcData[4] = {image.constBits(), nullptr, nullptr, nullptr};
        const int srcStride[4] = {int(image.bytesPerLine()), 0, 0, 0};
        sws_scale(sws.get(), srcData, srcStride, 0, kHeight, yuv->data, yuv->linesize);
        yuv->pts = static_cast<int64_t>(index);
        if (const int ret = avcodec_send_frame(enc.get(), yuv.get()); ret < 0)
            return std::unexpected(QStringLiteral("Encode error: %1").arg(avErr(ret)));
        if (auto drained = drainPackets(enc.get()); !drained)
            return drained;
    }
    if (const int ret = avcodec_send_frame(enc.get(), nullptr); ret < 0)
        return std::unexpected(QStringLiteral("Encode flush error: %1").arg(avErr(ret)));
    if (auto drained = drainPackets(enc.get()); !drained)
        return drained;

    if (const int ret = av_write_trailer(out.get()); ret < 0)
        return std::unexpected(QStringLiteral("Trailer write failed: %1").arg(avErr(ret)));
    return {};
}

} // namespace clips
