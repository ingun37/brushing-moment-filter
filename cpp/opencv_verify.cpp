#include <cstdio>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

int main() {
    std::printf("OpenCV version: %s\n", CV_VERSION);

    cv::Mat img(64, 64, CV_8UC3, cv::Scalar(0, 128, 255));
    cv::Mat gray;
    cv::cvtColor(img, gray, cv::COLOR_BGR2GRAY);
    std::printf("cvtColor BGR->GRAY: %dx%d, channels=%d\n",
                gray.cols, gray.rows, gray.channels());

    return gray.channels() == 1 ? 0 : 1;
}
