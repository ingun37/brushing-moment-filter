#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

int main() {
    std::printf("avcodec version:  %s\n", AV_STRINGIFY(LIBAVCODEC_VERSION));
    std::printf("avformat version: %s\n", AV_STRINGIFY(LIBAVFORMAT_VERSION));
    std::printf("avutil version:   %s\n", AV_STRINGIFY(LIBAVUTIL_VERSION));

    const AVCodec* h264 = avcodec_find_decoder(AV_CODEC_ID_H264);
    std::printf("H.264 decoder: %s\n", h264 ? h264->long_name : "not found");
    return h264 ? 0 : 1;
}
