A simple frei0r plugin for use in ffmpeg. It reads an external `.json.gz` that
contains per-frame blur information. The plugin will then blur these image
parts. Since the FPS are not known to the video filter, a param can be given
indicating the number of frames to skip from the blur `.json.gz`.

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
DOCKER_BUILDKIT=1 docker build -o type=local,dest=. --target artifacts
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

This example has two blurs for frame 0, and one blur for frame 1. Only `y_min`,
`x_min`, `y_max`, `x_max` parameters are used, but the implementation will break
if any other non-float field than `kind` is added (due to a lazy
implementation).

[upstream filter docs]: https://ffmpeg.org/ffmpeg-filters.html#frei0r-1
[video-anon-lossless]: https://github.com/breunigs/video-anon-lossless
[understand.ai's anonymizer]: https://github.com/understand-ai/anonymizer
