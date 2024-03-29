FROM debian:testing-slim AS build
WORKDIR /build/

RUN apt-get update --yes \
  && apt-get install --yes --no-install-recommends \
  cmake \
  frei0r-plugins-dev \
  g++ \
  libboost-dev \
  libboost-iostreams-dev \
  libvips-dev \
  make \
  rapidjson-dev \
  && rm -rf /var/lib/apt/lists/*

COPY . /build/
RUN cmake . && make


FROM scratch AS artifacts
COPY --from=build /build/jsonblur.so /
