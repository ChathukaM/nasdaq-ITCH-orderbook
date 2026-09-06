# Nasdaq ITCH Order Book Reconstruction

A high-performance C++20 parser and limit order book reconstructor for Nasdaq TotalView-ITCH 5.0 binary data. It reads a complete trading-session feed, decodes each order event, and maintains full-depth bid and ask books for every instrument.

Built around cache-friendly custom data structures, the executable supports message inspection, full-session replay, performance benchmarking, book validation, and queue-position analysis.

## The problem

ITCH provides a stream of order events rather than ready-to-use book snapshots. Executions, cancellations, deletions, and replacements may identify an order only by its reference number, so reconstructing the book requires retaining every live order and applying events in sequence.

This project converts that raw event stream into full-depth bid and ask books while preserving the order-level data needed for validation and queue-position analysis.

## Performance

Measured on an Apple M3 Pro using a release build and the 30 July 2019 Nasdaq sample file:

| Metric | Result |
|---|---:|
| Input size | 8.66 GB |
| Messages | 282,229,684 |
| Instruments in Stock Directory | 8,849 |
| Full-session replay | 32.4 s |
| Replay throughput | 8.7 million messages/s |
| Book-update benchmark | 68.1 ns/message |
| In-memory book state at 15:00 | approximately 190 MB |

The 8,849 directory entries are instruments available in the Nasdaq execution system on that day, including securities listed on other exchanges; they are not all Nasdaq-listed stocks.

## Design

- Memory-mapped, zero-copy message parsing
- Big-endian field decoding without packed structs or floating point
- Locate-indexed books for constant-time symbol routing
- Flat order-reference table with backward-shift deletion
- Contiguous bid and ask price levels
- Queue-position and fill-rate analysis by instrument

The engine handles add, execute, cancel, delete, and replace events while retaining the state required by later messages that contain only an order reference number.

## Validation

The full-session replay processed 125,460,750 added orders with:

- zero unknown order references
- zero orders remaining at market close
- zero symbol mismatches across all add-order messages
- zero price-level mismatches or orphaned orders in an independent 15:00 reconstruction of 1,864,951 live orders and 784,619 price levels

The test suite covers binary decoding, byte order, symbol routing, order lifecycle events, executions, cancellations, replacements, and price-level maintenance.

## Build

Requires CMake and a C++20 compiler. There are no runtime dependencies.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## Run

```bash
./build/itch_replay data/07302019.NASDAQ_ITCH50 --replay
./build/itch_replay data/07302019.NASDAQ_ITCH50 --bench --latency
./build/itch_replay data/07302019.NASDAQ_ITCH50 --symbols
./build/itch_replay data/07302019.NASDAQ_ITCH50 --queue AAPL
```

Run the executable without arguments to see all available options.

## Data

The benchmark uses Nasdaq's public `07302019.NASDAQ_ITCH50` sample:

```bash
mkdir -p data
curl -L -C - -o data/07302019.NASDAQ_ITCH50.gz \
  "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/07302019.NASDAQ_ITCH50.gz"
gunzip -k data/07302019.NASDAQ_ITCH50.gz
```

The market-data file is not included in this repository.

## Limitations and future work

- Currently supports 64-bit little-endian macOS and Linux systems, including WSL2. Native Windows would require a Windows memory-mapping backend and compiler-specific byte-swap support.
- Reads decompressed BinaryFILE sessions conforming to TotalView-ITCH 5.0; live MoldUDP64 ingestion and compressed-input streaming are not implemented.
- Uses a fixed-capacity order table sized for the sessions tested so far. Configurable sizing and capacity checks would make it safer for larger feeds.
- Performance depends on the processor, available memory, and storage. Benchmarks should therefore include the host hardware and filesystem.
