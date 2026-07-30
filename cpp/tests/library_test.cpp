#include "library.h"

#include <gtest/gtest.h>
#include <opencv2/imgcodecs.hpp>

#include <string>

TEST(LibraryTest, HelloPrintsGreeting) {
    testing::internal::CaptureStdout();
    hello();
    EXPECT_EQ(testing::internal::GetCapturedStdout(), "Hello, World!\n");
}

namespace {

const std::string kResourceDir = TEST_RESOURCE_DIR;

// Mean absolute pixel difference between two same-sized BGR images.
double meanAbsDiff(const cv::Mat& a, const cv::Mat& b) {
    cv::Mat diff;
    cv::absdiff(a, b, diff);
    return cv::mean(cv::mean(diff))[0];
}

} // namespace

TEST(ExtractFramesTest, SamplesAbcVideoEveryPointNineSeconds) {
    // 3 s video, one letter per second. Sampling at t = 0, 0.9, 1.8, 2.7
    // should yield A, A, B, C.
    const auto result = extractFrames(kResourceDir + "/ABC.mp4", 0.9);
    ASSERT_TRUE(result.has_value()) << result.error();
    const auto& frames = *result;
    ASSERT_EQ(frames.size(), 4u);

    const cv::Mat a = cv::imread(kResourceDir + "/A.png");
    const cv::Mat b = cv::imread(kResourceDir + "/B.png");
    const cv::Mat c = cv::imread(kResourceDir + "/C.png");
    ASSERT_FALSE(a.empty() || b.empty() || c.empty());

    const cv::Mat* expected[] = {&a, &a, &b, &c};
    const cv::Mat* letters[] = {&a, &b, &c};
    const char* names = "ABC";

    for (size_t i = 0; i < frames.size(); ++i) {
        ASSERT_EQ(frames[i].size(), expected[i]->size());
        ASSERT_EQ(frames[i].type(), CV_8UC3);
        // The frame must be closest to the expected letter, with a
        // tolerance for lossy h264/yuv420p encoding.
        EXPECT_LT(meanAbsDiff(frames[i], *expected[i]), 10.0) << "frame " << i;
        for (int l = 0; l < 3; ++l) {
            if (letters[l] != expected[i]) {
                EXPECT_GT(meanAbsDiff(frames[i], *letters[l]), 10.0)
                    << "frame " << i << " too similar to " << names[l];
            }
        }
    }
}
