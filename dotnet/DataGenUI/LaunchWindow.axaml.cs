using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Interactivity;
using Avalonia.Platform.Storage;

namespace DataGenUI;

public partial class LaunchWindow : Window
{
    // Defaults match run.py / dotnet/AGENTS.md.
    private const int Port = 15071;
    private const string ServerArgs =
        "--port 15071 --sample-interval-seconds 1.0 --dedup-tolerance 5";

    public LaunchWindow()
    {
        InitializeComponent();
        Loaded += OnLoaded;
    }

    private async void OnLoaded(object? sender, RoutedEventArgs e)
    {
        Loaded -= OnLoaded;

        LaunchStatusText.Text = "Locating frame_server…";
        var serverPath = await LocateServerAsync();
        if (serverPath is null)
        {
            LaunchStatusText.Text = "No frame_server selected — cannot start.";
            return;
        }

        LaunchStatusText.Text = $"Starting {Path.GetFileName(serverPath)}…";
        Process server;
        try
        {
            server = Process.Start(new ProcessStartInfo(serverPath)
            {
                Arguments = ServerArgs,
                UseShellExecute = false,
            }) ?? throw new InvalidOperationException("Process.Start returned null");
        }
        catch (Exception ex)
        {
            LaunchStatusText.Text = $"Failed to start server: {ex.Message}";
            return;
        }

        // Give it a moment to fail fast (e.g. port already in use).
        await Task.Delay(TimeSpan.FromSeconds(1));
        if (server.HasExited)
        {
            LaunchStatusText.Text =
                $"frame_server exited immediately (code {server.ExitCode}) — is port {Port} in use?";
            return;
        }

        App.ServerProcess = server;
        LaunchStatusText.Text = $"frame_server running on port {Port}.";

        var mainWindow = new MainWindow();
        if (Avalonia.Application.Current?.ApplicationLifetime
            is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.MainWindow = mainWindow;
        }
        mainWindow.Show();
        Close();
    }

    /// Resolution order: --server-path CLI argument, then frame_server next to the
    /// app executable, then a file picker.
    private async Task<string?> LocateServerAsync()
    {
        var args = Environment.GetCommandLineArgs();
        var flagIndex = Array.IndexOf(args, "--server-path");
        if (flagIndex >= 0 && flagIndex + 1 < args.Length)
        {
            var path = args[flagIndex + 1];
            if (File.Exists(path))
                return path;
            LaunchStatusText.Text = $"--server-path not found: {path}";
            return null;
        }

        var sibling = Path.Combine(AppContext.BaseDirectory, "frame_server");
        if (File.Exists(sibling))
            return sibling;

        LaunchStatusText.Text = "Select the frame_server executable…";
        IReadOnlyList<IStorageFile> picked = await StorageProvider.OpenFilePickerAsync(
            new FilePickerOpenOptions
            {
                Title = "Locate frame_server",
                AllowMultiple = false,
            });
        return picked.FirstOrDefault()?.TryGetLocalPath();
    }
}
