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
    private bool _eofReceived;
    private string _keepDir = "";
    private int _frameIndex; // index of the first frame of the current batch
    private int _keptCount;

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
        _keepDir = Path.Combine(
            Path.GetDirectoryName(path)!,
            Path.GetFileNameWithoutExtension(path) + "_kept");
        _frameIndex = 0;
        _keptCount = 0;
        _eofReceived = false;
        _batchPngs.Clear();
        FrameList.Items.Clear();

        try
        {
            OpenButton.IsEnabled = false;
            StatusText.Text = "Uploading…";
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
                while ((read = await file.ReadAsync(buffer)) > 0)
                {
                    await _call.RequestStream.WriteAsync(new ClientMessage
                    {
                        Chunk = new VideoChunk
                        {
                            Data = ByteString.CopyFrom(buffer, 0, read),
                            Last = file.Position == file.Length,
                        },
                    });
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
            .OrderBy(i => i);
        Directory.CreateDirectory(_keepDir);
        foreach (var i in selected)
        {
            var name = Path.Combine(_keepDir, $"frame_{_frameIndex + i:D4}.png");
            await File.WriteAllBytesAsync(name, _batchPngs[i]);
            _keptCount++;
        }
        await RequestNextBatchAsync();
    }

    private async void OnSkip(object? sender, RoutedEventArgs e)
    {
        await RequestNextBatchAsync();
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
        StatusText.Text = $"Stopped. Kept {_keptCount} frame(s) in {_keepDir}";
    }

    private async Task RequestNextBatchAsync()
    {
        if (_call is null)
            return;
        SetReviewEnabled(false);
        _frameIndex += _batchPngs.Count;
        _batchPngs.Clear();
        FrameList.Items.Clear();

        if (_eofReceived)
        {
            await EndSessionAsync();
            StatusText.Text = $"End of video. Kept {_keptCount} frame(s) in {_keepDir}";
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
    }

    private async Task ShowBatchAsync()
    {
        if (_batchPngs.Count == 0) // eof with nothing left to review
        {
            await EndSessionAsync();
            StatusText.Text = $"End of video. Kept {_keptCount} frame(s) in {_keepDir}";
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
            $"Frames {_frameIndex + 1}–{_frameIndex + _batchPngs.Count} — kept {_keptCount} so far. " +
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
