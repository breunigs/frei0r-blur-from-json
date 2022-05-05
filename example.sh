#!/bin/bash

make && \
mkdir -p ~/.frei0r-1/lib/ && \
cp jsonblur.so ~/.frei0r-1/lib/jsonblur.so && \
ffmpeg -hide_banner -loglevel fatal -hwaccel auto -re -f lavfi -i testsrc -t 7 -pix_fmt yuv420p -filter_complex '[0]frei0r=jsonblur:blur.json.gz|0[blur0]' -map '[blur0]' -pix_fmt yuv420p -f yuv4mpegpipe output.y4v && \
mpv --pause --no-resume-playback --framedrop=no --audio=no --keep-open=yes --screenshot-format=png output.y4v
