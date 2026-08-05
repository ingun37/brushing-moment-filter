using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Interactivity;
using Avalonia.Media.Imaging;
using Frameservice;
using Grpc.Core;
using Grpc.Net.Client;

namespace DataGenUI;

// Step three of the startup flow (LaunchWindow -> VideoUploadWindow ->
// MainWindow): reviews the frames of the session VideoUploadWindow started.
// The upload and Start handshake are already done; this window drives the
// Next/Frames ping-pong and returns to VideoUploadWindow when the user wants
// another video.
public partial class MainWindow : Window
{
    private const int BatchSize = 12;

    private GrpcChannel? _channel;
    private AsyncDuplexStreamingCall<ClientMessage, ServerMessage>? _call;
    private readonly List<byte[]> _batchPngs = [];
    private readonly List<double> _batchTimestamps = []; // parallel to _batchPngs
    private bool _eofReceived;
    private readonly string _positiveDir;
    private readonly string _negativeDir;
    private readonly string _manifestPath;
    private readonly SessionManifest _manifest;
    private int _frameIndex; // index of the first frame of the current batch
    private int _positiveCount;
    private readonly double _durationSeconds;

    public MainWindow(VideoSession session)
    {
        InitializeComponent();
        _channel = session.Channel;
        _call = session.Call;
        _manifest = session.Manifest;
        _manifestPath = session.ManifestPath;
        _positiveDir = session.PositiveDir;
        _negativeDir = session.NegativeDir;
        _positiveCount = _manifest.Frames.Count(f => f.Positive);
        _durationSeconds = Math.Max(session.DurationSeconds, 0.001);
        VideoProgress.Maximum = _durationSeconds;
        SetVideoPosition(_manifest.ResumeSeconds);
        UpdateProgress();
        Loaded += OnLoaded;
    }

    // Reflects how far into the video the review has progressed.
    private void SetVideoPosition(double seconds)
    {
        var position = Math.Min(seconds, _durationSeconds);
        VideoProgress.Value = position;
        VideoProgressLabel.Text = $"{position:F1} / {_durationSeconds:F1} s";
    }

    // Fetches the first batch of the already-started session.
    private async void OnLoaded(object? sender, RoutedEventArgs e)
    {
        Loaded -= OnLoaded;
        StatusText.Text = _manifest.ResumeSeconds > 0
            ? $"Waiting for frames… (resuming from {_manifest.ResumeSeconds:F1} s)"
            : "Waiting for frames…";
        await RequestBatchAsync();
    }

    // Shows the overall collection progress across every session manifest
    // (the current video's counts come from the in-memory manifest, which is
    // ahead of disk mid-batch).
    private void UpdateProgress()
    {
        var sessionsDir = Path.Combine(AppContext.BaseDirectory, "DataGenUI_data", "sessions");
        ProgressText.Text = SessionManifest.DescribeProgress(sessionsDir, _manifestPath, _manifest);
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

    // Stops any active session and returns to the video-choosing step.
    private async void OnBack(object? sender, RoutedEventArgs e)
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

        var upload = new VideoUploadWindow();
        if (Avalonia.Application.Current?.ApplicationLifetime
            is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.MainWindow = upload;
        }
        upload.Show();
        Close();
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

            _manifest.Frames.Add(new FrameRecord
            {
                File = Path.GetRelativePath(AppContext.BaseDirectory, name),
                TimestampSeconds = timestamp,
                Positive = positive,
            });
        }
        if (_batchTimestamps.Count > 0)
        {
            // Nudge past the last frame so resuming does not re-serve it.
            _manifest.ResumeSeconds = _batchTimestamps[^1] + 0.001;
            _manifest.Save(_manifestPath);
        }
        UpdateProgress();
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
            SetVideoPosition(_durationSeconds);
            StatusText.Text = $"End of video. {_positiveCount} positive frame(s) in {_positiveDir}";
            return;
        }

        await RequestBatchAsync();
    }

    // Asks the server for the next batch and displays it.
    private async Task RequestBatchAsync()
    {
        try
        {
            await _call!.RequestStream.WriteAsync(new ClientMessage
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
            SetVideoPosition(_durationSeconds);
            StatusText.Text = $"End of video. {_positiveCount} positive frame(s) in {_positiveDir}";
            return;
        }
        SetVideoPosition(_batchTimestamps[^1]);

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
