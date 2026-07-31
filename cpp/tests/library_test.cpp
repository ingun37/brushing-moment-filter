#include "library.h"

#include <gtest/gtest.h>
#include <opencv2/imgcodecs.hpp>

#include <list>
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

TEST(RemoveConsecutiveDuplicatesTest, DropsNearDuplicatesPreservingOrder) {
    const cv::Mat black(8, 8, CV_8UC3, cv::Scalar::all(0));
    const cv::Mat almostBlack(8, 8, CV_8UC3, cv::Scalar::all(3));
    const cv::Mat white(8, 8, CV_8UC3, cv::Scalar::all(255));

    // black, ~black, white, white, black -> black, white, black
    const std::list<cv::Mat> frames = {black, almostBlack, white, white, black};
    const auto result = removeConsecutiveDuplicates(frames, 5.0);

    ASSERT_EQ(result.size(), 3u);
    EXPECT_LT(meanAbsDiff(result[0], black), 1e-9);
    EXPECT_LT(meanAbsDiff(result[1], white), 1e-9);
    EXPECT_LT(meanAbsDiff(result[2], black), 1e-9);

    // With zero tolerance the near-duplicate survives.
    EXPECT_EQ(removeConsecutiveDuplicates(frames, 0.0).size(), 4u);

    EXPECT_TRUE(removeConsecutiveDuplicates(std::vector<cv::Mat>{}, 5.0).empty());
}

TEST(RemoveConsecutiveDuplicatesTest, DifferentSizesAreNeverDuplicates) {
    const cv::Mat small(4, 4, CV_8UC3, cv::Scalar::all(0));
    const cv::Mat big(8, 8, CV_8UC3, cv::Scalar::all(0));
    const std::vector<cv::Mat> frames = {small, big};
    EXPECT_EQ(removeConsecutiveDuplicates(frames, 255.0).size(), 2u);
}

TEST(RemoveConsecutiveDuplicatesTest, DeduplicatesExtractedAbcFrames) {
    // 3 s video, one letter per second, sampled every 0.4 s: many consecutive
    // frames show the same letter. After dedup only A, B, C remain.
    const auto extracted = extractFrames(kResourceDir + "/ABC.mp4", 0.4);
    ASSERT_TRUE(extracted.has_value()) << extracted.error();
    ASSERT_GT(extracted->size(), 3u);

    const auto unique = removeConsecutiveDuplicates(*extracted, 10.0);
    ASSERT_EQ(unique.size(), 3u);

    const char* names[] = {"A.png", "B.png", "C.png"};
    for (int i = 0; i < 3; ++i) {
        const cv::Mat letter = cv::imread(kResourceDir + "/" + names[i]);
        ASSERT_FALSE(letter.empty());
        EXPECT_LT(meanAbsDiff(unique[i], letter), 10.0) << names[i];
    }
}

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
