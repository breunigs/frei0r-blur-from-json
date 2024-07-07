A simple frei0r plugin for use in ffmpeg. It reads an external `.json.gz` that
contains per-frame blur information. The plugin will then blur these image
parts.

Example filter definition:
```
frei0r=jsonblur:somefile.MP4.json.gz|14|0.3|0|1
```

* `frei0r` tells ffmpeg to invoke the frei0r filter tooling
* `jsonblur` is the name of the binary that frei0r will invoke, i.e. the output
  of this repo when being built
* `somefile.MP4.json.gz` (param name: `jsonPath`) is the path to the file
  containing the detections that should be blurred. See below for details.
* `14` (param name: `skipFrames`): skip the first n frames in the `.json.gz`.
  Since is unfortunately required, since frei0r only passes a timestamp to the
  filter, but not the FPS (frames per second). Since the detections are indexed
  by frame count and not timestamp, the filter can't find the correct frame by
  itself. If you start the video at the beginning, pass a `0` or omit the param.
  If you cut the video at the start (e.g. `-ss` ffmpeg param), see the Example
  on how to calculate the correct frame count to pass.
* `0.3` (param name: `minScore`): only blur detections whose score exceeds this
  value. You can most likely omit this, the code has a sensible default. The
  intent is to run the detections once, keeping even the ones with low
  confidence. You can then cheaply tune what actually gets blurred.
* `0` (param name: `skipFramesEvery`): how many frames to skip after blurring
  one. This is a performance optimization for the case of "extract every n-th
  image and blur it". Instead of blurring all images, and then selecting every
  n-th, the order can be turned around.
* `1` (param name: `debug`): renders the indexes (0-based) of the blurs used
  onto the image. This can be a make-shift manual editor, but it's mostly meant
  for rendering and detection debugging purposes.

## Example Usage

```bash
export FREI0R_PATH=$(pwd)
ffmpeg \
  -ss 00:00:00.434 \
  -i somefile.MP4 \
  -vf 'frei0r=jsonblur:somefile.MP4.json.gz|14' \
  -c:v yuv4 blurred.mkv
```

The frame rate can be determined by:
```bash
ffprobe -v 0 -of compact=p=0 -select_streams 0 -show_entries stream=r_frame_rate somefile.MP4
# r_frame_rate=30000/1001
```

Since we cut the video and start from only `00:00:00.434`, we need to skip some frames:
```bash
python -c 'from math import ceil; print(ceil(30000 / 1001 * 0.434))'
# 14
```

## Building / Installing

This will build the plugin in a Docker container. See the `Dockerfile` on what
build dependencies are needed. If the versions used in docker are the exact same
as on your system, you can extract the resulting filter like this:
```bash
DOCKER_BUILDKIT=1 docker build -o type=local,dest=. --target artifacts .
```
Note that if the versions don't match ffmpeg will complain with `Could not find
module 'jsonblur'.` even if it found the incompatible version.

You can also move the `jsonblur.so` into one of the folders that ffmpeg checks
for frei0r plugins by default, to avoid having to set `FREI0R_PATH`.

```bash
mkdir -p ~/.frei0r-1/lib/
cp jsonblur.so ~/.frei0r-1/lib/
```

See [upstream filter docs] for details.

## .json.gz format

The format is that of [video-anon-lossless], which is built on top of
[understand.ai's anonymizer]. Here's an example:

```json
{
  "0": [
    {
      "y_min": 779.2870903015137,
      "x_min": 2609.70467376709,
      "y_max": 807.1863460540771,
      "x_max": 2637.5875720977783,
      "score": 0.832179069519043,
      "kind": "face"
    },
    {
      "y_min": 792.1585893630981,
      "x_min": 2562.3684406280518,
      "y_max": 819.4704008102417,
      "x_max": 2587.0525789260864,
      "score": 0.7064805030822754,
      "kind": "face"
    }
  ],
  "1": [
    {
      "y_min": 783.3212852478027,
      "x_min": 1789.7090587615967,
      "y_max": 800.6567811965942,
      "x_max": 1805.2080640792847,
      "score": 0.9060346484184265,
      "kind": "face"
    }
  ]
}
```

This example has two blurs for frame 0, and one blur for frame 1.

`y_min`, `x_min`, `y_max`, `x_max` parameters are used to position the blur
within the video frame. Therefore the values should be within the video frame's
width/height.

`score` is a value between 0 and 1, denoting the confidence of the detection.
Only detections with scores greater than the `minScore` parameter will be
blurred.

`kind` contains the detected object, which is usually a predefined enum from the
detection tooling. This library uses it to round the corners of the rectangle to
make the blur better fit the detected object and the transition to the blur less
jarring. Specifically, a value of `face` will lead to the blur being an ellipse,
`person` gets slightly rounded corners and everything else gets no rounding.


[upstream filter docs]: https://ffmpeg.org/ffmpeg-filters.html#frei0r-1
[video-anon-lossless]: https://github.com/breunigs/video-anon-lossless
[understand.ai's anonymizer]: https://github.com/understand-ai/anonymizer
