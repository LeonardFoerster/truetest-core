# Stage 1: Build
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake g++ git \
    libboost-all-dev libssl-dev libsqlite3-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_WEB_UI=ON \
    -DENABLE_BINANCE=ON \
    -DENABLE_SQLITE=ON \
    && cmake --build build --parallel "$(nproc)"

# Stage 2: Runtime
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 libssl3 libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /src/build/truetest /app/truetest
COPY --from=builder /src/web/ /app/web/

EXPOSE 8765

CMD ["./truetest", "--web-ui"]
