# dotnet/DataGenUI

Avalonia GUI client for the C++ `frame_server` (gRPC). The user reviews extracted video frames
in tiled batches of 12: Keep Selected saves the selected PNGs to `<video>_kept/` next to the
video and fetches the next batch, Skip All just fetches the next batch, Stop ends the session.

- The wire contract is `../../cpp/frame_service.proto`, referenced directly by
  `DataGenUI.csproj` (`Grpc.Tools` generates the C# stubs at build time, namespace
  `Frameservice`). Never copy the proto — edit the one file and rebuild both sides.
- `dotnet` is at `/usr/local/share/dotnet/dotnet` (not on PATH in non-login shells).
- Build/run: `dotnet build` / `dotnet run` in `DataGenUI/`. The app starts `frame_server`
  itself (see below), so a running server is not a prerequisite — but the executable must be
  built (`cmake --build cpp/build --target frame_server`). The session idle timeout is fixed
  at 300 s server-side and ticks between user clicks.
- Startup flow (`LaunchWindow.axaml.cs`): the app opens `LaunchWindow` first, which locates
  `frame_server`, spawns it as a subprocess, then swaps to `MainWindow`. Path resolution
  order: (1) `--server-path <path>` CLI argument (e.g. `dotnet run -- --server-path
  ../../cpp/build/frame_server`), (2) a `frame_server` file next to the app executable
  (`AppContext.BaseDirectory`), (3) a file picker. The resolved path and the server arguments
  (default `--port 15071 --sample-interval-seconds 1.0 --dedup-tolerance 5`, matching
  `run.py`) are shown in editable fields; Next validates the path, starts the process, waits
  1 s to catch an immediate exit (bad flags, port in use — shown in the status line, retry
  allowed), then opens the main view. The `Process` handle lives in `App.ServerProcess`;
  `App.axaml.cs` kills it on `desktop.Exit`.
- No MVVM: the template is plain code-behind (`MainWindow.axaml.cs` holds the gRPC session
  state). Fine at this size; introduce a view model only if the UI grows.
- The gRPC session is strict ping-pong after upload: every batch, including the first, is
  sent in reply to a `Next` (up to `Next.count` frames); this client always asks for 12.
  Buttons are disabled while a batch is in flight.
