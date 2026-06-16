# --- Stage 1: Build the application ---
FROM ubuntu:24.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive

# Install compiler and Qt6 dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    qt6-base-dev \
    libqt6sql6-sqlite \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . .

# Compile using CMake
RUN mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make

# --- Stage 2: Runtime image ---
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive

# Install minimal runtime libraries for headless Qt6
RUN apt-get update && apt-get install -y \
    libqt6core6 \
    libqt6sql6 \
    libqt6sql6-sqlite \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the built binary from Stage 1
# NOTE: Replace 'KrtkServer' below if your executable binary name in CMakeLists.txt is different!
COPY --from=builder /app/build/KrtkServer .

# Run the application
CMD ["./KrtkServer"]
