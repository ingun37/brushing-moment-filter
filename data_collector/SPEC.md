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
