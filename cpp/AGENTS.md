Use modern standard libraries (ranges, concepts, expected, etc.) whenever possible and appropriate.

## Layout

- `library.{h,cpp}` — core: `extractFrames`, `framesSimilar`, `removeConsecutiveDuplicates`,
  plus the threaded pipeline: `FrameQueue` (bounded, blocking, `close()` for shutdown) and the
  queue stages `extractFramesToQueue`, `removeConsecutiveDuplicatesToQueue`.
- `pipeline.h` — stdexec (P2300 senders) wrappers around the queue stages, `pipeline::` namespace.
  stdexec provides launch/join/error plumbing only; the FrameQueues are the actual stream +
  backpressure (senders model one async value, not streams).
- `frame_service.proto` — gRPC contract, shared with the Avalonia client in `dotnet/DataGenUI`
  (its build generates C# stubs from this same file). Change it in lockstep with both sides.
- `frame_service_impl.{h,cpp}` — gRPC service (`frame_service` static lib); `server.cpp` is just
  `main()` for the `frame_server` executable. The only flag is the mandatory `--port`
  (`frame_server --port 15071`). Pipeline parameters are per-video: the first `VideoChunk`
  of each session may carry `sample_interval_seconds` and `dedup_tolerance` (server defaults
  1.0 s / 10.0 when unset). After the upload the server replies with `VideoInfo` (the
  `videoStreamMd5` of the upload — the video's identity) and waits for `Start`, which carries
  `start_seconds`, before launching the pipeline. The session idle timeout is not a flag; it defaults to 300 s in
  `FrameServerOptions` (tests override it programmatically).
- `tests/library_test.cpp` + `tests/server_test.cpp` (the latter runs the service on an
  in-process gRPC channel, no ports). Test videos come from `tests/gen-test-resources.sh`.

## Pipeline stage contract

Every queue stage must: always `close()` its output before returning (guard with RAII), stop when
its output's `push` returns false (downstream closed), and then `close()` its input so
cancellation propagates upstream to the producer. Follow this when adding stages.

## Build & test

```
cmake --build build -j 8 && (cd build && ctest)
```

- Configured build tree lives in `cpp/build` (`cmake ..` from there after CMakeLists changes).
- Deps: FFmpeg/OpenCV/gRPC/protobuf via Homebrew; googletest + stdexec via FetchContent.
- gtest's post-build test discovery occasionally hits its 5 s timeout and deletes the binary —
  just rebuild.

## Gotchas

- Dedup tolerance: adjacent digit images in `12.mp4` differ by as little as ~7.3 mean abs diff
  ("5" vs "6"), so `framesSimilar` tolerance 10 merges them; tests use 5 for that video.
- `extractFrames` with a tiny interval (e.g. 0.001) decodes every frame.
- The server buffers the full upload to a temp file before decoding (mp4 moov atom is usually at
  the end; sequential decode of arbitrary mp4s isn't possible). Known limitation, not a bug.
- `cv::Mat` copies are refcounted shallow copies — keeping a reference then moving the Mat
  elsewhere is safe (used by the dedup stage's `lastKept`).
