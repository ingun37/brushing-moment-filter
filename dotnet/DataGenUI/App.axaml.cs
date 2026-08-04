using System.Diagnostics;
using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;

namespace DataGenUI;

public partial class App : Application
{
    /// The frame_server subprocess started by LaunchWindow; killed on app exit.
    public static Process? ServerProcess { get; set; }

    public override void Initialize()
    {
        AvaloniaXamlLoader.Load(this);
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.MainWindow = new LaunchWindow();
            desktop.Exit += (_, _) =>
            {
                if (ServerProcess is { HasExited: false } server)
                {
                    server.Kill();
                    server.WaitForExit(5000);
                }
            };
        }

        base.OnFrameworkInitializationCompleted();
    }
}
