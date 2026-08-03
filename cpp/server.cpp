// Frame server executable. The service logic lives in frame_service_impl.cpp;
// the wire contract in frame_service.proto.
//
// Usage: frame_server --port <port>
//                     [--sample-interval-seconds <double>]
//                     [--dedup-tolerance <double>]

#include "frame_service_impl.h"

#include <grpcpp/grpcpp.h>

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

void printUsage(const char* program)
{
    std::cerr << "usage: " << program
              << " --port <port>\n"
                 "          [--sample-interval-seconds <double>]  time between"
                 " sampled frames (default 1.0)\n"
                 "          [--dedup-tolerance <double>]          max mean abs"
                 " pixel diff treated as duplicate (default 10)\n";
}

template <typename T>
std::optional<T> parseNumber(std::string_view text)
{
    T value{};
    const auto [rest, err] = std::from_chars(text.begin(), text.end(), value);
    if (err != std::errc{} || rest != text.end())
        return std::nullopt;
    return value;
}

} // namespace

int main(int argc, char** argv)
{
    FrameServerOptions opts;
    std::optional<int> port;

    for (int i = 1; i < argc; ++i) {
        const std::string_view name = argv[i];
        if (name == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        if (i + 1 >= argc) {
            std::cerr << "missing value for " << name << "\n";
            printUsage(argv[0]);
            return 1;
        }
        const std::string_view value = argv[++i];

        bool ok = true;
        if (name == "--port") {
            ok = (port = parseNumber<int>(value)).has_value();
        } else if (name == "--sample-interval-seconds") {
            ok = false;
            if (const auto v = parseNumber<double>(value)) {
                opts.intervalSeconds = *v;
                ok = true;
            }
        } else if (name == "--dedup-tolerance") {
            ok = false;
            if (const auto v = parseNumber<double>(value)) {
                opts.tolerance = *v;
                ok = true;
            }
        } else {
            std::cerr << "unknown argument " << name << "\n";
            printUsage(argv[0]);
            return 1;
        }
        if (!ok) {
            std::cerr << "invalid value '" << value << "' for " << name << "\n";
            printUsage(argv[0]);
            return 1;
        }
    }

    if (!port) {
        std::cerr << "--port is required\n";
        printUsage(argv[0]);
        return 1;
    }

    FrameServiceImpl service(opts);
    grpc::ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:" + std::to_string(*port),
                             grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    const auto server = builder.BuildAndStart();
    if (!server) {
        std::cerr << "failed to start server on port " << *port << "\n";
        return 1;
    }
    std::cout << "listening on port " << *port << std::endl;
    server->Wait();
    return 0;
}
