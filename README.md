# nasdaq-ITCH-orderbook

Reconstructs the full order-by-order limit order book from Nasdaq's raw TotalView-ITCH 5.0
binary feed — one complete trading day, 8.66 GB, 282,229,684 messages — and measures the cost
of doing so.

ITCH does not send you the book. It sends you the events, and the messages that modify an
order carry only its reference number:

```
D | stock locate | tracking | timestamp | order reference
```

No price. No side. No symbol. Applying that delete requires already knowing that order
#4,829,201 was a buy of 300 AAPL at $150.00 — from an add message possibly a hundred million
messages earlier. Reconstruction therefore means holding every open order on the exchange in
memory and applying every event in sequence. That state, not the byte parsing, is the problem.

## Results

**30 July 2019, full session, single thread, all 8,849 listed symbols**

| | |
|---|---|
| Messages | 282,229,684 |
| Orders added | 125,460,750 |
| Executions | 7,717,995 (921,453,624 shares) |
| Symbols in directory | 8,849 |
| Symbols with a live two-sided book | 8,841 concurrently |
| Replay time | 32.2 s (**8.8 M msg/s**) |
| Book update cost | **68 ns/message** |
| Book state | ~190 MB (see below) |

**Per-message latency** (M3 Pro; the host clock ticks at 24 MHz, so percentiles below ~100 ns
are quantised at 41 ns and should be read as bounds rather than exact values):

| p50 | p90 | p99 | p99.9 | p99.99 | max |
|---|---|---|---|---|---|
| 83 ns | 167 ns | 333 ns | 500 ns | 875 ns | 54.8 µs |

The tail is page faults: the file is memory-mapped, so occasionally a message is the one that
touches a new page and waits on the SSD.

## Where the time goes

Measured by running the pipeline in three stages against the same warm mapping:

| Stage | M msg/s | ns/msg | GB/s |
|---|---|---|---|
| Framing only | 280 | 3.6 | 8.7 |
| + field decoding | 192 | 5.2 | 6.0 |
| + book maintenance | 13.6 | 73.3 | 0.4 |

Framing and decoding together account for 5.2 ns of a 73 ns message. **93% of the work is
maintaining book state**, which is what the optimisation below targets.

## Optimisation

Both containers were chosen naively at first, then replaced and measured. The book state
fingerprint (below) was identical before and after, so the speedup came with no behavioural
change.

Two measurement bases, kept separate because they answer different questions. The first
isolates the component that changed; the second is what the whole program actually does.

| Measurement | Before | After | Ratio |
|---|---|---|---|
| Book update cost (60M-message benchmark, like-for-like) | 185.1 ns/msg | **68.1 ns/msg** | **2.7x** |
| Full-day replay, wall clock | 77.6 s | **32.4 s** | **2.4x** |
| Full-day replay, throughput | 3.64 M msg/s | **8.72 M msg/s** | **2.4x** |

The component improved by 2.7x but the program by 2.4x, because book maintenance is 93% of
runtime rather than all of it.

Process RSS is not quoted as a comparison: the input is memory-mapped, so resident file pages
dominate it (~6 GB either way) and swamp the structures themselves. The flat containers are
fixed and exactly measurable instead — 8,388,608 slots x 20 bytes = 168 MB for the order map,
3.1 MB for the 65,536-entry book array, and 18.8 MB for the 784,619 live price levels at the
15:00 snapshot. Roughly 190 MB total, allocated once, with no per-order heap node.

**Order map.** `std::unordered_map` allocates each entry as a separate node, so a lookup
follows a bucket pointer into memory that is almost certainly not cached — on a workload where
roughly half of all messages perform exactly one lookup. Replacing it with a flat
open-addressed table of fixed capacity keeps probes in contiguous memory.

*The interesting failure:* the first version used tombstones for deletion. A session churns
125M orders through 8M slots, so tombstones accumulated until no truly empty slot remained, at
which point every lookup for an absent key scanned the entire table. Replay time went from 78
seconds to over 16 minutes. Switching to backward-shift deletion (Knuth 6.4R), which repairs
the probe chain in place, fixed it and is what the numbers above reflect.

**Price levels.** `std::map` is a red-black tree: one heap node per level, a cache miss per
step of the descent. Book depth turns out to be heavily skewed — median 45 levels per side,
p99 461, max 3,597 — and the deepest books belong to the most heavily traded symbols (AAPL,
TSLA, MSFT). A sorted array searches contiguous memory, but insertion cost depends on *where*
in the array the new level lands, so both sides store the best price at the back: bids
ascending, asks descending. Activity concentrates at the touch, which is now the cheap end.

**Sequential-access page hints** (`MADV_SEQUENTIAL`/`MADV_WILLNEED`) made no measurable
difference on macOS — 70.4 ns/msg against 69.6 without, within run-to-run noise. Retained
because it is free and Linux honours the hint more aggressively.

## Correctness

There is no reference book to compare against, so correctness is established by invariants that
a wrong implementation would violate.

| Check | Result |
|---|---|
| Message payload lengths match the fixed sizes for their type | 282,229,684 / 0 mismatches |
| Add-order symbol field matches the locate → symbol table | 125,460,750 / 0 mismatches |
| Execute/cancel/delete/replace names a known order | ~151 M / **0 unknown** |
| Orders remaining at end of session | **0 of 125,460,750** |
| Price levels rebuilt independently from the order map (15:00 snapshot) | 784,619 levels, 1,864,951 orders, **0 mismatches, 0 orphans** |

The last is the strongest: it discards the incrementally maintained levels, rebuilds every one
from scratch by grouping the order map, and compares. Any drift accumulated over 282 million
updates would show up.

Books are maintained for every symbol concurrently, not one at a time -- 8,841 of the 8,849
listed symbols hold a two-sided book simultaneously by mid-session (the remaining 8 never
traded). Top of book was cross-checked against an independent Python reconstruction sharing no
code with this implementation, across the liquidity spectrum, at message 200,000,000:

| Symbol | Bid | Ask | Match |
|---|---|---|---|
| SPY | 3,500 @ 301.0400 | 2,200 @ 301.0500 | exact |
| AAPL | 100 @ 209.7100 | 205 @ 209.7200 | exact |
| MSFT | 1,300 @ 141.1400 | 645 @ 141.1600 | exact |
| ZYXI | 100 @ 8.3600 | 100 @ 8.3900 | exact |
| BIQI | 35 @ 0.8000 | 93 @ 0.6579 | exact |
| CCIH | 10 @ 2.0000 | 300 @ 0.0100 | exact |
| ATEST | 100 @ 18.0200 | 100 @ 32.0800 | exact |

Per-symbol routing is cheap by design: every message carries a 2-byte stock locate, so books
live in a flat array indexed by it. The expensive shared state is the order map, which is
necessarily global -- order reference numbers are exchange-wide, not per-symbol.

**Book state fingerprint:** `0xcb0e7562cbdb2217` — a 64-bit digest of the complete book at
15:00, iterated in canonical order so it describes state rather than container layout. Every
optimisation above was required to leave it unchanged.

### Books do cross, and that is not a bug

The obvious invariant — best bid below best ask — is **false** on real data. 459 crossed
observations occurred out of ~146 million, in 5 symbols:

```
09:03:32.606880234  ARRY     bid 48.0000 > ask 47.9800
09:30:01.151630533  CCIH     bid  0.8100 > ask  0.0100
09:30:01.345601471  BIQI     bid  0.7000 > ask  0.6615
11:06:36.883320373  SES      bid  5.5600 > ask  5.3500
16:01:16.102322490  ACRS     bid  1.9200 > ask  1.9100
```

All are illiquid names, clustered at the open and close. `CCIH` is crossed because a one-cent
stub quote rests against an 81-cent bid moments before the opening auction clears it. An
independent Python reconstruction sharing no code with this implementation reproduces the same
crossings at the same timestamps and prices.

The usable invariant is therefore not "never crosses" but "crossings are rare, brief, and
confined to illiquid symbols around auctions" — a bug would scatter them across liquid names
throughout the day.

## Queue position

Order-by-order data exists to answer questions aggregated depth cannot. Two feeds both showing
300 shares at $150.00 say nothing about whether an order joining now sits behind one order or
thirty — and that determines whether it ever fills.

Fill outcome by queue position on join, regular session only:

**AAPL** — 640,274 orders joined, 5.70% filled, mean 3,586 shares ahead

| Queue position | Joined | Filled | Fill rate | Mean wait |
|---|---|---|---|---|
| 0 (front) | 68,817 | 7,435 | **10.80%** | 143 s |
| 1 | 79,823 | 6,663 | 8.35% | 127 s |
| 2–3 | 181,603 | 11,635 | 6.41% | 124 s |
| 4–7 | 233,590 | 7,636 | 3.27% | 211 s |
| 8–15 | 34,773 | 1,302 | 3.74% | 707 s |

**SPY** — 1,034,192 orders joined, 4.20% filled, mean 2,279 shares ahead

| Queue position | Joined | Filled | Fill rate | Mean wait |
|---|---|---|---|---|
| 0 (front) | 66,175 | 3,877 | 5.86% | 28 s |
| 1 | 45,569 | 3,772 | **8.28%** | 14 s |
| 2–3 | 104,524 | 8,083 | 7.73% | 14 s |
| 4–7 | 278,456 | 13,313 | 4.78% | 22 s |
| 8–15 | 436,048 | 12,717 | 2.92% | 38 s |
| 16–31 | 102,335 | 1,627 | 1.59% | 57 s |

Joining near the front is worth roughly 3× the fill probability of joining ten deep, and SPY
fills an order of magnitude faster than AAPL at equivalent depth. The sparse deep buckets for
AAPL (16+) invert the trend on small samples with multi-hour waits; they are quiet-period
orders that eventually filled, not a contradiction of the pattern.

## Market structure

From the message census across the full session:

| Type | Count | Share |
|---|---|---|
| `A` Add Order | 124,164,371 | 43.99% |
| `D` Order Delete | 119,999,061 | 42.52% |
| `U` Order Replace | 21,253,951 | 7.53% |
| `E` Order Executed | 7,582,422 | 2.69% |
| `X` Order Cancel | 2,358,032 | 0.84% |

**86% of feed traffic is orders appearing and disappearing.** Roughly 6% of orders ever
generate an execution — consistent with the per-symbol fill rates above. Most displayed
liquidity on Nasdaq is posted and pulled without ever trading.

## Building

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

No dependencies beyond a C++20 compiler. The test suite runs without any data file.

`Debug` builds keep assertions on; benchmarks are only meaningful on `Release`, which supplies
`-O3 -DNDEBUG -march=native`.

## Running

```
./build/itch_replay data/07302019.NASDAQ_ITCH50                  # message census, format checks
./build/itch_replay data/07302019.NASDAQ_ITCH50 --symbols        # locate -> symbol table
./build/itch_replay data/07302019.NASDAQ_ITCH50 --sample 10      # decoded add orders
./build/itch_replay data/07302019.NASDAQ_ITCH50 --replay --watch AAPL,SPY
./build/itch_replay data/07302019.NASDAQ_ITCH50 --bench --latency
./build/itch_replay data/07302019.NASDAQ_ITCH50 --queue AAPL
```

`--replay` reconstructs the book and reports every invariant above plus the fingerprint.
`--limit N` stops after N messages for quicker iteration.

Sample output at 14:23 on 30 July 2019:

```
AAPL  (locate 14)                          SPY  (locate 7397)
    bid size        bid  |  ask   ask size     bid size        bid  |  ask   ask size
         100   209.7100  |  209.7200   205         3500   301.0400  |  301.0500  2200
         630   209.7000  |  209.7300  2220         3610   301.0300  |  301.0600  1700
         330   209.6900  |  209.7400   800         2800   301.0200  |  301.0700  1600
spread: 0.0100                                spread: 0.0100
```

## Data

Nasdaq publishes sample days at <https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/>. This project uses
`07302019.NASDAQ_ITCH50`.

```
curl -L -C - -o data/07302019.NASDAQ_ITCH50.gz \
  "https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/07302019.NASDAQ_ITCH50.gz"
gunzip -k data/07302019.NASDAQ_ITCH50.gz
```

| | |
|---|---|
| Compressed | 3,662,140,094 bytes — md5 `8744aba2ea125bfdde1a340ee2cea924` |
| Decompressed | 8,661,679,413 bytes — md5 `3ea7eb57d1ea584b6951db42fd79bb28` |

The `.md5sum` files listed in Nasdaq's directory index return 404, so those checksums were
computed locally after verifying the download by exact `content-length` match and gzip CRC.

Decompressing up front is deliberate: the parser memory-maps the file, which requires raw bytes,
and decompressing inline would turn any benchmark into a measurement of zlib.

## Design notes

**Memory-mapped, zero-copy.** The file is mapped rather than read, so there is no 8.66 GB
buffer and no copy; fields are decoded directly out of the mapping.

**No packed structs.** Messages are read through accessor views that call `memcpy`-based
big-endian loaders. Overlaying a `#pragma pack`'d struct on the mapping would require a
`reinterpret_cast` — strict-aliasing UB, and unaligned on a format with no padding. The
`memcpy` form compiles to a single `ldr`/`rev` pair, so correctness here is free.

**Prices are integers.** Four implied decimal places; `1500000` is `$150.0000`. No floating
point anywhere in the parser or the book.

**Locate-indexed, not symbol-indexed.** Only four message types carry a ticker; everything else
identifies its stock by a 2-byte locate. Books live in a flat array indexed directly by it,
which is precisely why the feed sends an integer rather than a string.

**Single-threaded by necessity.** Every message depends on the state left by all previous ones,
so the replay cannot be parallelised across messages.

## Layout

```
src/
  platform.h            memory mapping, big-endian loads
  itch/
    reader.h            length-prefix framing
    messages.h          per-type accessor views
    symbol_table.h      locate -> symbol
  book/
    order_map.h         flat open-addressed order table
    price_levels.h      sorted price level arrays
    order_book.h        one symbol's two sides
    handler.h           message dispatch and order state
    validate.h          invariants and state fingerprint
  analysis/queue.h      queue position tracking
  bench.h               phase timing and latency histogram
```
