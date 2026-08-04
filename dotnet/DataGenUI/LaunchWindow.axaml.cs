using System;
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
    private const string DefaultServerArgs =
        "--port 15071 --sample-interval-seconds 1.0 --dedup-tolerance 5";

    public LaunchWindow()
    {
        InitializeComponent();
        ServerArgsBox.Text = DefaultServerArgs;
        Loaded += OnLoaded;
    }

    private async void OnLoaded(object? sender, RoutedEventArgs e)
    {
        Loaded -= OnLoaded;

        // Prefill: --server-path CLI argument, then frame_server next to the app
        // executable, then a file picker. The user can still edit before Next.
        var args = Environment.GetCommandLineArgs();
        var flagIndex = Array.IndexOf(args, "--server-path");
        if (flagIndex >= 0 && flagIndex + 1 < args.Length)
        {
            ServerPathBox.Text = args[flagIndex + 1];
            return;
        }

        var sibling = Path.Combine(AppContext.BaseDirectory, "frame_server");
        if (File.Exists(sibling))
        {
            ServerPathBox.Text = sibling;
            return;
        }

        LaunchStatusText.Text = "Select the frame_server executable.";
        await BrowseAsync();
    }

    private async void OnBrowse(object? sender, RoutedEventArgs e) => await BrowseAsync();

    private async Task BrowseAsync()
    {
        var picked = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Locate frame_server",
            AllowMultiple = false,
        });
        if (picked.FirstOrDefault()?.TryGetLocalPath() is { } path)
            ServerPathBox.Text = path;
    }

    private async void OnNext(object? sender, RoutedEventArgs e)
    {
        var serverPath = ServerPathBox.Text?.Trim();
        if (string.IsNullOrEmpty(serverPath) || !File.Exists(serverPath))
        {
            LaunchStatusText.Text = "Server executable not found.";
            return;
        }

        NextButton.IsEnabled = false;
        BrowseButton.IsEnabled = false;
        LaunchStatusText.Text = $"Starting {Path.GetFileName(serverPath)}…";

        Process server;
        try
        {
            server = Process.Start(new ProcessStartInfo(serverPath)
            {
                Arguments = ServerArgsBox.Text ?? "",
                UseShellExecute = false,
            }) ?? throw new InvalidOperationException("Process.Start returned null");
        }
        catch (Exception ex)
        {
            LaunchStatusText.Text = $"Failed to start server: {ex.Message}";
            NextButton.IsEnabled = true;
            BrowseButton.IsEnabled = true;
            return;
        }

        // Give it a moment to fail fast (e.g. port already in use).
        await Task.Delay(TimeSpan.FromSeconds(1));
        if (server.HasExited)
        {
            LaunchStatusText.Text =
                $"frame_server exited immediately (code {server.ExitCode}) — check the arguments.";
            NextButton.IsEnabled = true;
            BrowseButton.IsEnabled = true;
            return;
        }

        App.ServerProcess = server;

        var mainWindow = new MainWindow();
        if (Avalonia.Application.Current?.ApplicationLifetime
            is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.MainWindow = mainWindow;
        }
        mainWindow.Show();
        Close();
    }
}
