using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace DataGenUI;

// One reviewed frame: where it was saved, when it occurs in the video, and
// whether the user marked it positive.
public class FrameRecord
{
    public string File { get; set; } = "";
    public double TimestampSeconds { get; set; }
    public bool Positive { get; set; }
}

// Per-video review progress, persisted as JSON under DataGenUI_data/sessions/
// so a quit session can resume where it left off (ResumeSeconds is sent to
// the server as VideoChunk.start_seconds).
public class SessionManifest
{
    public string VideoPath { get; set; } = "";
    public double ResumeSeconds { get; set; }
    public List<FrameRecord> Frames { get; set; } = [];

    private static readonly JsonSerializerOptions Options = new() { WriteIndented = true };

    public static SessionManifest LoadOrCreate(string path, string videoPath)
    {
        if (File.Exists(path))
        {
            try
            {
                var loaded = JsonSerializer.Deserialize<SessionManifest>(File.ReadAllText(path));
                if (loaded is not null)
                    return loaded;
            }
            catch (JsonException)
            {
                // Corrupt manifest: start the video over rather than crash.
            }
        }
        return new SessionManifest { VideoPath = videoPath };
    }

    public void Save(string path)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temp = path + ".tmp";
        File.WriteAllText(temp, JsonSerializer.Serialize(this, Options));
        File.Move(temp, path, overwrite: true);
    }
}
