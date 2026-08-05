using System;
using System.IO;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;
using Frameservice;
using Google.Protobuf;
using Grpc.Core;
using Grpc.Net.Client;

namespace DataGenUI;

// A running review session, created by VideoUploadWindow (which performs the
// upload + VideoInfo/Start handshake) and handed to MainWindow (which drives
// the Next/Frames ping-pong and owns disposal from then on).
public class VideoSession
{
    public required GrpcChannel Channel { get; init; }
    public required AsyncDuplexStreamingCall<ClientMessage, ServerMessage> Call { get; init; }
    public required SessionManifest Manifest { get; init; }
    public required string ManifestPath { get; init; }
    public required string PositiveDir { get; init; }
    public required string NegativeDir { get; init; }
}

// Step two of the startup flow (LaunchWindow -> VideoUploadWindow ->
// MainWindow): the user picks a video and pipeline parameters here, the
// upload and session handshake run, and the live session moves to MainWindow
// for frame review. MainWindow returns here to choose another video.
public partial class VideoUploadWindow : Window
{
    public VideoUploadWindow()
    {
        InitializeComponent();
        UpdateProgress();
    }

    private void UpdateProgress()
    {
        var sessionsDir = Path.Combine(AppContext.BaseDirectory, "DataGenUI_data", "sessions");
        ProgressText.Text = SessionManifest.DescribeProgress(sessionsDir);
    }

    private async void OnOpenVideo(object? sender, RoutedEventArgs e)
    {
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Choose a video",
            FileTypeFilter = [new FilePickerFileType("Video") { Patterns = ["*.mp4"] }],
        });
        if (files.Count == 0)
            return;
        var path = files[0].TryGetLocalPath();
        if (path is null)
        {
            StatusText.Text = "Cannot access the selected file.";
            return;
        }

        // Per-video pipeline parameters, sent to the server on the first chunk.
        if (!double.TryParse(IntervalBox.Text, out var intervalSeconds) || intervalSeconds <= 0)
        {
            StatusText.Text = "Interval must be a positive number of seconds.";
            return;
        }
        if (!double.TryParse(ToleranceBox.Text, out var tolerance) || tolerance < 0)
        {
            StatusText.Text = "Dedup tolerance must be a non-negative number.";
            return;
        }

        OpenButton.IsEnabled = false;
        ClearButton.IsEnabled = false;
        GrpcChannel? channel = null;
        AsyncDuplexStreamingCall<ClientMessage, ServerMessage>? call = null;
        try
        {
            StatusText.Text = "Uploading…";
            // A 12-frame PNG batch easily exceeds the 4 MB default limit.
            channel = GrpcChannel.ForAddress(ServerBox.Text ?? "", new GrpcChannelOptions
            {
                MaxReceiveMessageSize = null, // unlimited
            });
            call = new FrameService.FrameServiceClient(channel).Session();

            await using (var file = File.OpenRead(path))
            {
                var buffer = new byte[1 << 16];
                int read;
                var firstChunk = true;
                while ((read = await file.ReadAsync(buffer)) > 0)
                {
                    var chunk = new VideoChunk
                    {
                        Data = ByteString.CopyFrom(buffer, 0, read),
                        Last = file.Position == file.Length,
                    };
                    if (firstChunk)
                    {
                        chunk.SampleIntervalSeconds = intervalSeconds;
                        chunk.DedupTolerance = tolerance;
                        firstChunk = false;
                    }
                    await call.RequestStream.WriteAsync(new ClientMessage { Chunk = chunk });
                }
            }

            // The server identifies the uploaded video by its stream MD5;
            // all session state (manifest, saved frames) is keyed by it, so
            // renamed or moved copies of a video share one session.
            var videoMd5 = await ReadVideoInfoAsync(call);
            var dataDir = Path.Combine(AppContext.BaseDirectory, "DataGenUI_data");
            var manifestPath = Path.Combine(dataDir, "sessions", videoMd5 + ".json");
            var manifest = SessionManifest.LoadOrCreate(manifestPath, path);
            manifest.VideoPath = path; // keep the latest known location

            await call.RequestStream.WriteAsync(new ClientMessage
            {
                Start = new Start { StartSeconds = manifest.ResumeSeconds },
            });

            var mainWindow = new MainWindow(new VideoSession
            {
                Channel = channel,
                Call = call,
                Manifest = manifest,
                ManifestPath = manifestPath,
                PositiveDir = Path.Combine(dataDir, "positive", videoMd5),
                NegativeDir = Path.Combine(dataDir, "negative", videoMd5),
            });
            if (Avalonia.Application.Current?.ApplicationLifetime
                is IClassicDesktopStyleApplicationLifetime desktop)
            {
                desktop.MainWindow = mainWindow;
            }
            mainWindow.Show();
            Close();
        }
        catch (Exception ex)
        {
            StatusText.Text = $"Session failed: {ex.Message}";
            call?.Dispose();
            channel?.Dispose();
            OpenButton.IsEnabled = true;
            ClearButton.IsEnabled = true;
        }
    }

    // Reads the VideoInfo the server sends once the upload completes and
    // returns the video's stream MD5.
    private static async Task<string> ReadVideoInfoAsync(
        AsyncDuplexStreamingCall<ClientMessage, ServerMessage> call)
    {
        if (!await call.ResponseStream.MoveNext(default))
            throw new RpcException(new Status(StatusCode.Unavailable, "stream ended"));
        var message = call.ResponseStream.Current;
        if (message.MsgCase != ServerMessage.MsgOneofCase.Info)
            throw new RpcException(new Status(StatusCode.Internal, "expected VideoInfo"));
        return message.Info.VideoMd5;
    }

    // Deletes everything under DataGenUI_data: saved positive/negative frames
    // and all session manifests. No session is active on this screen.
    private void OnClear(object? sender, RoutedEventArgs e)
    {
        var dataDir = Path.Combine(AppContext.BaseDirectory, "DataGenUI_data");
        try
        {
            if (Directory.Exists(dataDir))
                Directory.Delete(dataDir, recursive: true);
        }
        catch (IOException ex)
        {
            StatusText.Text = $"Clear failed: {ex.Message}";
            return;
        }
        StatusText.Text = "Cleared all saved frames and session state.";
        UpdateProgress();
    }
}
