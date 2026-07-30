cmake_minimum_required(VERSION 4.3)
project(brushing_moment_filter)

set(CMAKE_CXX_STANDARD 26)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
        libavcodec libavformat libavutil libswscale libswresample)

find_package(OpenCV REQUIRED)

add_library(brushing_moment_filter STATIC library.cpp)
target_link_libraries(brushing_moment_filter PUBLIC PkgConfig::FFMPEG ${OpenCV_LIBS})
target_include_directories(brushing_moment_filter PUBLIC ${OpenCV_INCLUDE_DIRS})

add_executable(ffmpeg_verify ffmpeg_verify.cpp)
target_link_libraries(ffmpeg_verify PRIVATE brushing_moment_filter)

add_executable(opencv_verify opencv_verify.cpp)
target_link_libraries(opencv_verify PRIVATE ${OpenCV_LIBS})
target_include_directories(opencv_verify PRIVATE ${OpenCV_INCLUDE_DIRS})

enable_testing()

include(FetchContent)
FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(ffmpeg_playground_tests tests/library_test.cpp)
target_link_libraries(ffmpeg_playground_tests PRIVATE brushing_moment_filter GTest::gtest_main)
target_include_directories(ffmpeg_playground_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(ffmpeg_playground_tests PRIVATE
        TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/tests/resources")

include(GoogleTest)
gtest_discover_tests(ffmpeg_playground_tests)
