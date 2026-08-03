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

find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(PROTO_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/proto_gen)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
add_custom_command(
        OUTPUT ${PROTO_GEN_DIR}/frame_service.pb.cc ${PROTO_GEN_DIR}/frame_service.pb.h
               ${PROTO_GEN_DIR}/frame_service.grpc.pb.cc ${PROTO_GEN_DIR}/frame_service.grpc.pb.h
        COMMAND protobuf::protoc
        ARGS --cpp_out=${PROTO_GEN_DIR} --grpc_out=${PROTO_GEN_DIR}
             --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
             -I ${CMAKE_CURRENT_SOURCE_DIR}
             ${CMAKE_CURRENT_SOURCE_DIR}/frame_service.proto
        DEPENDS frame_service.proto
        COMMENT "Generating gRPC sources from frame_service.proto")

add_library(frame_service STATIC frame_service_impl.cpp
        ${PROTO_GEN_DIR}/frame_service.pb.cc
        ${PROTO_GEN_DIR}/frame_service.grpc.pb.cc)
target_include_directories(frame_service PUBLIC ${PROTO_GEN_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(frame_service PUBLIC brushing_moment_filter
        gRPC::grpc++ protobuf::libprotobuf)

add_executable(frame_server server.cpp)
target_link_libraries(frame_server PRIVATE frame_service)

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

set(STDEXEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(STDEXEC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        stdexec
        URL https://github.com/NVIDIA/stdexec/archive/refs/tags/nvhpc-26.05.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(stdexec)
target_link_libraries(brushing_moment_filter PUBLIC STDEXEC::stdexec)

add_executable(ffmpeg_playground_tests tests/library_test.cpp)
target_link_libraries(ffmpeg_playground_tests PRIVATE brushing_moment_filter GTest::gtest_main)
target_include_directories(ffmpeg_playground_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(ffmpeg_playground_tests PRIVATE
        TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/tests/resources")

add_executable(frame_server_tests tests/server_test.cpp)
target_link_libraries(frame_server_tests PRIVATE frame_service GTest::gtest_main)
target_compile_definitions(frame_server_tests PRIVATE
        TEST_RESOURCE_DIR="${CMAKE_SOURCE_DIR}/tests/resources")

include(GoogleTest)
gtest_discover_tests(ffmpeg_playground_tests)
gtest_discover_tests(frame_server_tests)
