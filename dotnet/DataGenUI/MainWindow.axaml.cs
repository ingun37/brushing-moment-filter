using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Interactivity;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Frameservice;
using Google.Protobuf;
using Grpc.Core;
using Grpc.Net.Client;

namespace DataGenUI;

public partial class MainWindow : Window
{
    private const int BatchSize = 12;

    private GrpcChannel? _channel;
    private AsyncDuplexStreamingCall<ClientMessage, ServerMessage>? _call;
    private readonly List<byte[]> _batchPngs = [];
    private readonly List<double> _batchTimestamps = []; // parallel to _batchPngs
    private bool _eofReceived;
    private string _positiveDir = "";
    private string _negativeDir = "";
    private string _manifestPath = "";
    private SessionManifest? _manifest;
    private int _frameIndex; // index of the first frame of the current batch
    private int _positiveCount;

    public MainWindow()
    {
        InitializeComponent();
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

        await EndSessionAsync();
        var videoName = Path.GetFileNameWithoutExtension(path);
        var dataDir = Path.Combine(AppContext.BaseDirectory, "DataGenUI_data");
        _positiveDir = Path.Combine(dataDir, "positive", videoName);
        _negativeDir = Path.Combine(dataDir, "negative", videoName);
        _manifestPath = Path.Combine(dataDir, "sessions", videoName + ".json");
        _manifest = SessionManifest.LoadOrCreate(_manifestPath, path);
        var resuming = _manifest.ResumeSeconds > 0;
        _frameIndex = 0;
        _positiveCount = _manifest.Frames.Count(f => f.Positive);
        _eofReceived = false;
        _batchPngs.Clear();
        _batchTimestamps.Clear();
        FrameList.Items.Clear();

        try
        {
            OpenButton.IsEnabled = false;
            StatusText.Text = resuming
                ? $"Uploading… (resuming from {_manifest.ResumeSeconds:F1} s)"
                : "Uploading…";
            // A 12-frame PNG batch easily exceeds the 4 MB default limit.
            _channel = GrpcChannel.ForAddress(ServerBox.Text ?? "", new GrpcChannelOptions
            {
                MaxReceiveMessageSize = null, // unlimited
            });
            _call = new FrameService.FrameServiceClient(_channel).Session();

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
                        chunk.StartSeconds = _manifest.ResumeSeconds;
                        firstChunk = false;
                    }
                    await _call.RequestStream.WriteAsync(new ClientMessage { Chunk = chunk });
                }
            }

            StatusText.Text = "Waiting for frames…";
            await _call.RequestStream.WriteAsync(new ClientMessage
            {
                Next = new Next { Count = BatchSize },
            });
            await ReadResponseAsync();
            await ShowBatchAsync();
        }
        catch (Exception ex)
        {
            StatusText.Text = $"Session failed: {ex.Message}";
            await EndSessionAsync();
        }
        finally
        {
            OpenButton.IsEnabled = true;
        }
    }

    private async void OnKeep(object? sender, RoutedEventArgs e)
    {
        var selected = FrameList.SelectedItems!.Cast<ListBoxItem>()
            .Select(item => (int)item.Tag!)
            .ToHashSet();
        await SaveBatchAsync(selected);
        await RequestNextBatchAsync();
    }

    private async void OnSkip(object? sender, RoutedEventArgs e)
    {
        await SaveBatchAsync([]);
        await RequestNextBatchAsync();
    }

    // Writes every frame of the current batch: selected ones to the positive
    // directory, the rest to the negative directory. Records each frame in
    // the session manifest and advances its resume position.
    private async Task SaveBatchAsync(HashSet<int> selected)
    {
        for (var i = 0; i < _batchPngs.Count; i++)
        {
            var positive = selected.Contains(i);
            var timestamp = _batchTimestamps[i];
            var dir = positive ? _positiveDir : _negativeDir;
            Directory.CreateDirectory(dir);
            // Timestamp-based names stay unique and meaningful across resumed
            // sessions, unlike a per-session frame counter.
            var name = Path.Combine(dir, FormattableString.Invariant($"frame_t{timestamp:000000.000}s.png"));
            await File.WriteAllBytesAsync(name, _batchPngs[i]);
            if (positive)
                _positiveCount++;

            _manifest!.Frames.Add(new FrameRecord
            {
                File = Path.GetRelativePath(AppContext.BaseDirectory, name),
                TimestampSeconds = timestamp,
                Positive = positive,
            });
        }
        if (_batchTimestamps.Count > 0)
        {
            // Nudge past the last frame so resuming does not re-serve it.
            _manifest!.ResumeSeconds = _batchTimestamps[^1] + 0.001;
            _manifest.Save(_manifestPath);
        }
    }

    // Ends any active session and deletes everything under DataGenUI_data:
    // saved positive/negative frames and all session manifests.
    private async void OnClear(object? sender, RoutedEventArgs e)
    {
        await EndSessionAsync();
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
        _manifest = null;
        _manifestPath = "";
        _positiveDir = "";
        _negativeDir = "";
        _frameIndex = 0;
        _positiveCount = 0;
        FrameList.Items.Clear();
        StatusText.Text = "Cleared all saved frames and session state. Open a video to start.";
    }

    private async void OnStop(object? sender, RoutedEventArgs e)
    {
        if (_call is not null)
        {
            try
            {
                await _call.RequestStream.WriteAsync(new ClientMessage { Stop = new Stop() });
            }
            catch (Exception)
            {
                // The session may already be gone; ending it below is enough.
            }
        }
        await EndSessionAsync();
        StatusText.Text = $"Stopped. {_positiveCount} positive frame(s) in {_positiveDir}";
    }

    private async Task RequestNextBatchAsync()
    {
        if (_call is null)
            return;
        SetReviewEnabled(false);
        _frameIndex += _batchPngs.Count;
        _batchPngs.Clear();
        _batchTimestamps.Clear();
        FrameList.Items.Clear();

        if (_eofReceived)
        {
            await EndSessionAsync();
            StatusText.Text = $"End of video. {_positiveCount} positive frame(s) in {_positiveDir}";
            return;
        }

        try
        {
            await _call.RequestStream.WriteAsync(new ClientMessage
            {
                Next = new Next { Count = BatchSize },
            });
            await ReadResponseAsync();
            await ShowBatchAsync();
        }
        catch (Exception ex)
        {
            StatusText.Text = $"Session failed: {ex.Message}";
            await EndSessionAsync();
        }
    }

    // Reads one server message, appending its frames to the current batch or
    // recording end-of-file (acted on once the displayed batch is reviewed).
    private async Task ReadResponseAsync()
    {
        if (!await _call!.ResponseStream.MoveNext(default))
            throw new RpcException(new Status(StatusCode.Unavailable, "stream ended"));

        var message = _call.ResponseStream.Current;
        if (message.MsgCase == ServerMessage.MsgOneofCase.Eof)
        {
            _eofReceived = true;
            return;
        }
        foreach (var png in message.Frames.Pngs)
            _batchPngs.Add(png.ToByteArray());
        _batchTimestamps.AddRange(message.Frames.TimestampsSeconds);
    }

    private async Task ShowBatchAsync()
    {
        if (_batchPngs.Count == 0) // eof with nothing left to review
        {
            await EndSessionAsync();
            StatusText.Text = $"End of video. {_positiveCount} positive frame(s) in {_positiveDir}";
            return;
        }

        FrameList.Items.Clear();
        for (var i = 0; i < _batchPngs.Count; i++)
        {
            using var stream = new MemoryStream(_batchPngs[i]);
            FrameList.Items.Add(new ListBoxItem
            {
                Content = new Image { Source = new Bitmap(stream), Stretch = Avalonia.Media.Stretch.Uniform },
                Tag = i,
            });
        }
        StatusText.Text =
            $"Frames {_frameIndex + 1}–{_frameIndex + _batchPngs.Count} — {_positiveCount} positive so far. " +
            "Select the frames to keep.";
        SetReviewEnabled(true);
    }

    private void SetReviewEnabled(bool enabled)
    {
        KeepButton.IsEnabled = enabled;
        SkipButton.IsEnabled = enabled;
        StopButton.IsEnabled = enabled;
    }

    private async Task EndSessionAsync()
    {
        SetReviewEnabled(false);
        _batchPngs.Clear();
        _eofReceived = false;
        if (_call is not null)
        {
            try
            {
                await _call.RequestStream.CompleteAsync();
            }
            catch (Exception)
            {
                // Already completed or the stream is broken; disposal handles it.
            }
            _call.Dispose();
            _call = null;
        }
        _channel?.Dispose();
        _channel = null;
    }
}
