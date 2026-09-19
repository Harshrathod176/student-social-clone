# Two stages: the build needs vcpkg and a compiler, the finished image needs
# neither. Shipping only the binary keeps the deployed image small.
FROM ubuntu:24.04 AS build

# The same packages setup.sh installs in the codespace. autoconf, automake and
# libtool are not optional: vcpkg builds libsodium with autotools.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl zip unzip tar ca-certificates \
        pkg-config libcurl4-openssl-dev \
        autoconf automake libtool autoconf-archive \
    && rm -rf /var/lib/apt/lists/*

ENV VCPKG_ROOT=/opt/vcpkg
RUN git clone --depth 1 https://github.com/microsoft/vcpkg "$VCPKG_ROOT" \
    && "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics

WORKDIR /src

# The ports are installed before the source is copied so that editing main.cpp
# does not throw away the cached library build, which is the slow part.
RUN "$VCPKG_ROOT/vcpkg" install crow sqlite3 libsodium

COPY CMakeLists.txt ./
COPY src ./src
RUN cmake -B build -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
    && cmake --build build

FROM ubuntu:24.04

# Only the shared libraries the binary actually loads. libsodium and sqlite are
# linked statically by vcpkg; curl is the system copy, so it has to be here.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=build /src/build/student_profiles ./student_profiles
COPY templates ./templates
COPY database ./database
COPY static ./static

# The database and the uploads are the only state worth keeping, and both live
# on the mounted volume. static/uploads stays a path inside the image because
# the URL it is served under is fixed, so it is a link to the volume instead.
RUN rm -rf static/uploads && ln -s /data/uploads static/uploads

COPY docker-entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-entrypoint.sh

ENV DB_PATH=/data/students.db
ENV PORT=8080
EXPOSE 8080

ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["./student_profiles"]
