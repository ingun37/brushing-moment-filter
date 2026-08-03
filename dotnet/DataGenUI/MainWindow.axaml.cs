using System;
using System.IO;
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
    private GrpcChannel? _channel;
    private AsyncDuplexStreamingCall<ClientMessage, ServerMessage>? _call;
    private byte[]? _currentPng;
    private string _keepDir = "";
    private int _frameIndex;
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

        try
        {
            OpenButton.IsEnabled = false;
            StatusText.Text = "Uploading…";
            _channel = GrpcChannel.ForAddress(ServerBox.Text ?? "");
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

            StatusText.Text = "Waiting for the first frame…";
            await ShowNextFrameAsync(); // first frame arrives unprompted
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
        if (_currentPng is null)
            return;
        Directory.CreateDirectory(_keepDir);
        var name = Path.Combine(_keepDir, $"frame_{_frameIndex:D4}.png");
        await File.WriteAllBytesAsync(name, _currentPng);
        _keptCount++;
        await RequestNextAsync();
    }

    private async void OnSkip(object? sender, RoutedEventArgs e)
    {
        await RequestNextAsync();
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

    private async Task RequestNextAsync()
    {
        if (_call is null)
            return;
        SetReviewEnabled(false);
        try
        {
            // This client always reviews one frame at a time.
            await _call.RequestStream.WriteAsync(new ClientMessage { Next = new Next { Count = 1 } });
            _frameIndex++;
            await ShowNextFrameAsync();
        }
        catch (Exception ex)
        {
            StatusText.Text = $"Session failed: {ex.Message}";
            await EndSessionAsync();
        }
    }

    private async Task ShowNextFrameAsync()
    {
        if (_call is null)
            return;
        if (!await _call.ResponseStream.MoveNext(default))
            throw new RpcException(new Status(StatusCode.Unavailable, "stream ended"));

        var message = _call.ResponseStream.Current;
        if (message.MsgCase == ServerMessage.MsgOneofCase.Eof)
        {
            await EndSessionAsync();
            StatusText.Text = $"End of video. Kept {_keptCount} frame(s) in {_keepDir}";
            return;
        }

        // We only ever request one frame per batch, so take the first.
        _currentPng = message.Frames.Pngs[0].ToByteArray();
        using var stream = new MemoryStream(_currentPng);
        FrameImage.Source = new Bitmap(stream);
        StatusText.Text = $"Frame {_frameIndex + 1} — kept {_keptCount} so far";
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
        _currentPng = null;
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
