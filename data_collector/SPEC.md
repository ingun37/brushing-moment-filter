`data_collector` is an multiplatform(MacOS, Windows, Linux) GUI app that is designed to let user produce learning data conveniently.

# Development environment

- FFmpeg
- Qt 6

Assume all the libraries are installed on the system by brew.

# App

## Start Scene

Let user upload a video.

Let user set initial values for

- Sampling Interval (default 5 seconds)
- Sample Length (Default 1 second)
- Sample Size (Default 12)

When a video is uploaded calculate the md5 of the video contents using ffmpeg apis from `libavutil/md5.h`.

The md5 is the unique id of the video.

Check the log in app directory to see if there's previous progress.

Move on to the next scene

## Collect Scene

Make the these states, with the initial values from the previous scene

- Sampling Interval
- Sample Length
- Sample Size
- Cursor (previous progress if exist, otherwise the beginning of the video)

Make UI for each state so user can adjust it.

### Iteration

Sample (sample size)-number of (sample length)-long clips where each clips are sampled for every (sampling interval)-time.

Show a grid view where the each clip is loop-playing in a cell.

Let user select some of the clips.

Let user click "Next" when they are done selecting.

The selected clips are "positive" data and the others are "negative" data.

Downsize all the clips to 320x180. If the aspect doesn't match then use "contain" (html term) strategy. 

Save the positive/negative clips a separate folders in the app directory.

Log the labeling and progress status in a log file in app directory.

Advance the cursor and go back to the beginning of "Iteration"

Terminate when it hits the end of video.

## Termination

Assuming all the records and positive/negative clips are saved on every iteration, there's nothing to do. Let user go back and upload another video.

# Implementation detail

Use modern standard libraries (ranges, concepts, expected, etc.) whenever possible and appropriate.


# AI's final comment

I'll look at the existing scaffold first, then build the app per the spec.

Now I'll check how the Qt build is configured (Qt path), then write the app.

Qt 6.11.1 — good. Writing the C++ core first.

Now the FFmpeg clip extraction/encoding module:

Now the clip store, image provider, and the `Session` controller:

One robustness fix in the extractor — an empty tail clip should stop the batch rather than be skipped (keeps cursor advancement consistent):

Now `main.cpp` (with a headless `--selftest` mode so the pipeline can be verified without clicking through the GUI) and the QML scenes:

Now update the CMakeLists and build:

As suspected, AppleClang's libc++ doesn't ship `std::views::enumerate` yet — switching those three loops to plain index iteration:

Builds clean. Now verifying the FFmpeg pipeline headlessly with a generated test video, and checking the QML loads without errors:

The `out2` dir wasn't created (the GUI path does `mkpath`, my test didn't). Retrying:

Letterboxing is correct — the square video is pillarboxed inside 320×180. Now the QML smoke test:

QML loads cleanly (only a benign font notice; the app ran until I killed it). Saving one toolchain note to memory:

The app is built and working. It implements the full SPEC and I verified the FFmpeg pipeline headlessly: the MD5 matches the system `md5` tool exactly, clips come out as 320×180 H.264 mp4s at the source frame rate, "contain" letterboxing is correct (verified with a square test video), and the QML loads with no errors.

**What was added** (all in `data_collector/`):

- [md5.cpp](data_collector/md5.cpp) — file MD5 via `libavutil/md5.h`, returning `std::expected` (this is the video's unique id).
- [clips.cpp](data_collector/clips.cpp) — the FFmpeg core: `extractClips` seeks/decodes N clips of the given length every interval, scaling each frame into a black 320×180 canvas with the "contain" strategy; `encodeClip` writes an mp4 (libx264, mpeg4 fallback).
- [session.cpp](data_collector/session.cpp) — the `Session` QObject exposed to QML as a singleton. Heavy work runs on QtConcurrent threads. It loads/saves per-video progress and labeling to `~/Library/Application Support/data_collector/sessions/<md5>.json`, saves clips to `.../clips/<md5>/positive|negative/<start-ms>.mp4`, advances the cursor each iteration, and flags `finished` at end of video.
- [clipstore.h](data_collector/clipstore.h) + [clipimageprovider.cpp](data_collector/clipimageprovider.cpp) — the current batch's frames served as `image://clips/<epoch>/<clip>/<frame>`, which is how the grid cells loop-play.
- [StartScene.qml](data_collector/StartScene.qml) — Choose Video → MD5/duration shown, "Previous progress found — resuming at N s" when a log exists, and the three parameters (defaults 5 s / 1 s / 12; restored from the log on resume).
- [CollectScene.qml](data_collector/CollectScene.qml) — toolbar with editable Interval/Length/Size/Cursor (plus a **Resample** button that re-extracts at the edited cursor without saving), a grid of loop-playing clips where clicking toggles positive/negative, and **Next** which saves + logs + advances + fetches the next batch. At end of video it shows a termination view with "Upload another video".

Two things worth knowing:

- **AppleClang's libc++ has no `std::views::enumerate` yet**, so I used `views::iota`/index loops alongside `std::expected` and `expected::transform`.
- I added a dev convenience: `appdata_collector --selftest <video> <out-dir>` runs md5 → extract → encode headlessly and exits — that's how the pipeline was verified without clicking through the GUI.

To run it from Qt Creator just hit Run, or from the terminal:

```bash
cd /Users/ingun/projects/brushing-moment-filter/data_collector/build/Desktop_arm_darwin_generic_mach_o_64bit_Debug && ./appdata_collector.app/Contents/MacOS/appdata_collector
```

One design note: clip previews decode all frames into memory at extraction time (12 clips × 1 s ≈ 7 MB per clip at 30 fps), which keeps playback dead simple and stutter-free; very long sample lengths × large sample sizes would raise memory use proportionally.