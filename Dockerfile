FROM debian:stable-slim AS build
WORKDIR /build/

RUN apt-get update --yes \
  && apt-get install --yes --no-install-recommends \
  cmake \
  frei0r-plugins-dev \
  g++ \
  libboost-dev \
  libboost-iostreams-dev \
  libmagick++-6-headers \
  libmagick++-6.q16-dev \
  make \
  && rm -rf /var/lib/apt/lists/*

COPY . /build/
RUN cmake . && make


FROM scratch AS artifacts
COPY --from=build /build/jsonblur.so /
