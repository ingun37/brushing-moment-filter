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
When a video is uploaded, move on to the next scene

## Collect Scene

Make the two states, with the initial values from the previous scene

- Sampling Interval
- Sample Length
- Sample Size

Make UI for each state so user can adjust it.

### Iteration

Sample (sample size)-number of (sample length)-long clips where each clips are sampled for every (sampling interval)-time.

Show a grid view where the each clip is loop-playing in a cell.

Let user select "positive" clips.

Let user click "Next" when they are done selecting.

"positive" clips 