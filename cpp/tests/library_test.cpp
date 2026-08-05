#include "library.h"
#include "pipeline.h"

#include <gtest/gtest.h>
#include <opencv2/imgcodecs.hpp>

#include <chrono>
#include <filesystem>
#include <future>
#include <list>
#include <string>
#include <thread>

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

TEST(VideoStreamMd5Test, IsDeterministicAndDistinguishesVideos) {
    const auto abc = videoStreamMd5(kResourceDir + "/ABC.mp4");
    ASSERT_TRUE(abc.has_value()) << abc.error();

    // 32 lowercase hex digits.
    EXPECT_EQ(abc->size(), 32u);
    EXPECT_EQ(abc->find_first_not_of("0123456789abcdef"), std::string::npos);

    // Same file, same hash; different video, different hash.
    EXPECT_EQ(*abc, *videoStreamMd5(kResourceDir + "/ABC.mp4"));
    const auto twelve = videoStreamMd5(kResourceDir + "/12.mp4");
    ASSERT_TRUE(twelve.has_value()) << twelve.error();
    EXPECT_NE(*abc, *twelve);
}

TEST(VideoStreamMd5Test, ByteIdenticalCopyHashesEqual) {
    const auto copyPath =
        std::filesystem::temp_directory_path() / "video_stream_md5_copy.mp4";
    std::filesystem::copy_file(kResourceDir + "/ABC.mp4", copyPath,
                               std::filesystem::copy_options::overwrite_existing);

    const auto original = videoStreamMd5(kResourceDir + "/ABC.mp4");
    const auto copy = videoStreamMd5(copyPath.string());
    std::filesystem::remove(copyPath);

    ASSERT_TRUE(original.has_value()) << original.error();
    ASSERT_TRUE(copy.has_value()) << copy.error();
    EXPECT_EQ(*original, *copy);
}

TEST(VideoStreamMd5Test, FailsForMissingFile) {
    const auto result = videoStreamMd5(kResourceDir + "/does_not_exist.mp4");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("does_not_exist"), std::string::npos);
}

TEST(VideoDurationSecondsTest, ReportsKnownDurations) {
    // gen-test-resources.sh: ABC.mp4 is 3 x 1 s stills, 12.mp4 is 12 x 1 s.
    const auto abc = videoDurationSeconds(kResourceDir + "/ABC.mp4");
    ASSERT_TRUE(abc) << abc.error();
    EXPECT_NEAR(*abc, 3.0, 0.2);
    const auto twelve = videoDurationSeconds(kResourceDir + "/12.mp4");
    ASSERT_TRUE(twelve) << twelve.error();
    EXPECT_NEAR(*twelve, 12.0, 0.2);
}

TEST(VideoDurationSecondsTest, FailsForMissingFile) {
    const auto result = videoDurationSeconds(kResourceDir + "/does_not_exist.mp4");
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("does_not_exist"), std::string::npos);
}

TEST(ExtractFramesToQueueTest, ProducerPausesOnFullQueueAndDeliversAllFrames) {
    // Reference: the same sampling done synchronously yields 4 frames
    // (A, A, B, C at t = 0, 0.9, 1.8, 2.7), so a capacity-2 queue forces
    // the producer to block at least once.
    const auto expected = extractFrames(kResourceDir + "/ABC.mp4", 0.9);
    ASSERT_TRUE(expected.has_value()) << expected.error();
    ASSERT_EQ(expected->size(), 4u);

    FrameQueue queue(2);
    auto producer = std::async(std::launch::async, [&] {
        return extractFramesToQueue(kResourceDir + "/ABC.mp4", 0.9, queue);
    });

    // With the consumer idle, the producer must stop at capacity and
    // not finish: the queue holds exactly 2 frames.
    while (queue.size() < 2)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(producer.wait_for(std::chrono::milliseconds(200)),
              std::future_status::timeout);
    EXPECT_EQ(queue.size(), 2u);

    // Draining the queue unblocks the producer; all frames arrive in order,
    // each stamped with its presentation time.
    std::vector<TimedFrame> received;
    while (auto frame = queue.pop())
        received.push_back(std::move(*frame));

    const auto result = producer.get();
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(received.size(), expected->size());
    for (size_t i = 0; i < received.size(); ++i) {
        EXPECT_LT(meanAbsDiff(received[i].image, (*expected)[i]), 1e-9) << "frame " << i;
        // Sampled at t = 0, 0.9, 1.8, 2.7: each timestamp is the first frame
        // at or after the sampling point, so it lies within one frame period.
        EXPECT_GE(received[i].timestampSeconds, i * 0.9) << "frame " << i;
        EXPECT_LT(received[i].timestampSeconds, i * 0.9 + 0.2) << "frame " << i;
    }
}

TEST(ExtractFramesToQueueTest, ClosesQueueOnError) {
    FrameQueue queue(2);
    auto producer = std::async(std::launch::async, [&] {
        return extractFramesToQueue(kResourceDir + "/does_not_exist.mp4", 0.9, queue);
    });

    // The queue is closed even on failure, so pop() must not block forever.
    EXPECT_EQ(queue.pop(), std::nullopt);
    EXPECT_FALSE(producer.get().has_value());
}

TEST(ExtractFramesToQueueTest, ConsumerClosingQueueStopsProducer) {
    FrameQueue queue(1);
    auto producer = std::async(std::launch::async, [&] {
        return extractFramesToQueue(kResourceDir + "/ABC.mp4", 0.1, queue);
    });

    ASSERT_TRUE(queue.pop().has_value());
    queue.close();

    // The producer notices the closed queue and returns without decoding
    // the rest of the video; a rejected push is not an error.
    ASSERT_EQ(producer.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(producer.get().has_value());
}

TEST(RemoveConsecutiveDuplicatesToQueueTest, DropsNearDuplicatesPreservingOrder) {
    const cv::Mat black(8, 8, CV_8UC3, cv::Scalar::all(0));
    const cv::Mat almostBlack(8, 8, CV_8UC3, cv::Scalar::all(3));
    const cv::Mat white(8, 8, CV_8UC3, cv::Scalar::all(255));

    // Same scenario as the synchronous test:
    // black, ~black, white, white, black -> black, white, black
    FrameQueue input(8);
    FrameQueue output(1); // capacity 1 forces the stage to block on push
    for (const cv::Mat& frame : {black, almostBlack, white, white, black})
        ASSERT_TRUE(input.push({frame.clone(), 0.0}));
    input.close();

    auto stage = std::async(std::launch::async, [&] {
        removeConsecutiveDuplicatesToQueue(input, output, 5.0);
    });

    std::vector<cv::Mat> result;
    while (auto frame = output.pop())
        result.push_back(std::move(frame->image));
    stage.get();

    ASSERT_EQ(result.size(), 3u);
    EXPECT_LT(meanAbsDiff(result[0], black), 1e-9);
    EXPECT_LT(meanAbsDiff(result[1], white), 1e-9);
    EXPECT_LT(meanAbsDiff(result[2], black), 1e-9);
}

TEST(RemoveConsecutiveDuplicatesToQueueTest, PipelinesFromExtractFramesToQueue) {
    // Full producer -> dedup -> consumer pipeline on the ABC video, each
    // stage on its own thread with small queues. Must match the synchronous
    // extractFrames + removeConsecutiveDuplicates result: exactly A, B, C.
    FrameQueue decoded(2);
    FrameQueue unique(2);

    auto producer = std::async(std::launch::async, [&] {
        return extractFramesToQueue(kResourceDir + "/ABC.mp4", 0.4, decoded);
    });
    auto dedup = std::async(std::launch::async, [&] {
        removeConsecutiveDuplicatesToQueue(decoded, unique, 10.0);
    });

    std::vector<cv::Mat> result;
    while (auto frame = unique.pop())
        result.push_back(std::move(frame->image));

    dedup.get();
    const auto produced = producer.get();
    ASSERT_TRUE(produced.has_value()) << produced.error();

    ASSERT_EQ(result.size(), 3u);
    const char* names[] = {"A.png", "B.png", "C.png"};
    for (int i = 0; i < 3; ++i) {
        const cv::Mat letter = cv::imread(kResourceDir + "/" + names[i]);
        ASSERT_FALSE(letter.empty());
        EXPECT_LT(meanAbsDiff(result[i], letter), 10.0) << names[i];
    }
}

TEST(RemoveConsecutiveDuplicatesToQueueTest, ClosingOutputStopsWholePipeline) {
    FrameQueue decoded(1);
    FrameQueue unique(1);

    auto producer = std::async(std::launch::async, [&] {
        return extractFramesToQueue(kResourceDir + "/ABC.mp4", 0.1, decoded);
    });
    auto dedup = std::async(std::launch::async, [&] {
        removeConsecutiveDuplicatesToQueue(decoded, unique, 10.0);
    });

    // Take one frame, then cancel from the consumer end. The dedup stage
    // must close its input, which in turn stops the producer.
    ASSERT_TRUE(unique.pop().has_value());
    unique.close();

    ASSERT_EQ(dedup.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    ASSERT_EQ(producer.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(producer.get().has_value());
}

TEST(PipelineTest, ExtractUniqueFramesMatchesSynchronousResult) {
    // stdexec pipeline (decode -> dedup -> collect on a thread pool) must
    // produce the same A, B, C frames as the synchronous path.
    const auto result = pipeline::extractUniqueFrames(
        kResourceDir + "/ABC.mp4", 0.4, 10.0, /*queueCapacity=*/2);
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->size(), 3u);

    const char* names[] = {"A.png", "B.png", "C.png"};
    for (int i = 0; i < 3; ++i) {
        const cv::Mat letter = cv::imread(kResourceDir + "/" + names[i]);
        ASSERT_FALSE(letter.empty());
        EXPECT_LT(meanAbsDiff((*result)[i], letter), 10.0) << names[i];
    }
}

TEST(PipelineTest, DecodeErrorPropagatesThroughPipeline) {
    const auto result =
        pipeline::extractUniqueFrames(kResourceDir + "/does_not_exist.mp4", 0.4, 10.0);
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().find("does_not_exist"), std::string::npos);
}

TEST(PipelineTest, CustomFilterStageComposesIntoPipeline) {
    // Four-stage pipeline with a user-defined filter spliced in between
    // dedup and collect (the slot a hand-filtering UI stage would occupy):
    // it rejects frames that look like B, so only A and C come out.
    const cv::Mat b = cv::imread(kResourceDir + "/B.png");
    ASSERT_FALSE(b.empty());

    exec::static_thread_pool pool(4);
    auto scheduler = pool.get_scheduler();
    FrameQueue decoded(2);
    FrameQueue unique(2);
    FrameQueue accepted(2);

    auto [extracted, frames] = stdexec::sync_wait(
        stdexec::when_all(
            pipeline::extractStage(scheduler, kResourceDir + "/ABC.mp4", 0.4, decoded),
            pipeline::dedupStage(scheduler, decoded, unique, 10.0),
            pipeline::filterStage(scheduler, unique, accepted,
                                  [&](const cv::Mat& f) { return meanAbsDiff(f, b) > 10.0; }),
            pipeline::collectStage(scheduler, accepted))).value();

    ASSERT_TRUE(extracted.has_value()) << extracted.error();
    ASSERT_EQ(frames.size(), 2u);
    const cv::Mat a = cv::imread(kResourceDir + "/A.png");
    const cv::Mat c = cv::imread(kResourceDir + "/C.png");
    ASSERT_FALSE(a.empty() || c.empty());
    EXPECT_LT(meanAbsDiff(frames[0], a), 10.0);
    EXPECT_LT(meanAbsDiff(frames[1], c), 10.0);
}

TEST(ExtractFramesTest, StartSecondsSkipsTheBeginning) {
    // Sampling at t = 1.0, 1.9, 2.8 skips the A second entirely: B, B, C.
    const auto result = extractFrames(kResourceDir + "/ABC.mp4", 0.9, 1.0);
    ASSERT_TRUE(result.has_value()) << result.error();
    ASSERT_EQ(result->size(), 3u);

    const cv::Mat b = cv::imread(kResourceDir + "/B.png");
    const cv::Mat c = cv::imread(kResourceDir + "/C.png");
    ASSERT_FALSE(b.empty() || c.empty());
    EXPECT_LT(meanAbsDiff((*result)[0], b), 10.0);
    EXPECT_LT(meanAbsDiff((*result)[1], b), 10.0);
    EXPECT_LT(meanAbsDiff((*result)[2], c), 10.0);
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
