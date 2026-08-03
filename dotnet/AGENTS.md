# dotnet/DataGenUI

Avalonia GUI client for the C++ `frame_server` (gRPC). The user reviews extracted video frames
in tiled batches of 12: Keep Selected saves the selected PNGs to `<video>_kept/` next to the
video and fetches the next batch, Skip All just fetches the next batch, Stop ends the session.

- The wire contract is `../../cpp/frame_service.proto`, referenced directly by
  `DataGenUI.csproj` (`Grpc.Tools` generates the C# stubs at build time, namespace
  `Frameservice`). Never copy the proto — edit the one file and rebuild both sides.
- `dotnet` is at `/usr/local/share/dotnet/dotnet` (not on PATH in non-login shells).
- Build/run: `dotnet build` / `dotnet run` in `DataGenUI/`. Start the server first, e.g.
  `cpp/build/frame_server --port 15071 --dedup-tolerance 5` (or just `python3 run.py` from
  the repo root, which starts both). The session idle timeout is fixed at 300 s server-side
  and ticks between user clicks.
- No MVVM: the template is plain code-behind (`MainWindow.axaml.cs` holds the gRPC session
  state). Fine at this size; introduce a view model only if the UI grows.
- The gRPC session is strict ping-pong after upload: every batch, including the first, is
  sent in reply to a `Next` (up to `Next.count` frames); this client always asks for 12.
  Buttons are disabled while a batch is in flight.
