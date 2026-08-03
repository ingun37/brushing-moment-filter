// Frame server executable. The service logic lives in frame_service_impl.cpp;
// the wire contract in frame_service.proto.
//
// Usage: frame_server <port> [intervalSeconds] [tolerance] [batchN] [sampleM]
//                     [timeoutSeconds]

#include "frame_service_impl.h"

#include <grpcpp/grpcpp.h>

#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <port> [intervalSeconds] [tolerance] [batchN] [sampleM]"
                     " [timeoutSeconds]\n";
        return 1;
    }
    FrameServerOptions opts;
    const int port = std::atoi(argv[1]);
    if (argc > 2) opts.intervalSeconds = std::atof(argv[2]);
    if (argc > 3) opts.tolerance = std::atof(argv[3]);
    if (argc > 4) opts.batchN = static_cast<std::size_t>(std::atoll(argv[4]));
    if (argc > 5) opts.sampleM = static_cast<std::size_t>(std::atoll(argv[5]));
    if (argc > 6) opts.timeout = std::chrono::seconds(std::atoi(argv[6]));

    FrameServiceImpl service(opts);
    grpc::ServerBuilder builder;
    builder.AddListeningPort("0.0.0.0:" + std::to_string(port),
                             grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    const auto server = builder.BuildAndStart();
    if (!server) {
        std::cerr << "failed to start server on port " << port << "\n";
        return 1;
    }
    std::cout << "listening on port " << port << std::endl;
    server->Wait();
    return 0;
}
