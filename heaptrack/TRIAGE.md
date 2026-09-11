# heaptrack — heap profiling results

Tool: heaptrack 1.5.0 (KDE's heap profiler)
Target: KArchive at commit `633dc09`
Reproduce: `./heaptrack/run_heaptrack.sh`
Outputs: [`comparison.txt`](comparison.txt) and per-format `*.summary.txt`

heaptrack is one of the tools not covered in the exercises. Being a heap
profiler rather than a memory checker, it answers a different question than
memcheck or ASan: not "is the memory use correct" but "how much memory does the
work take, and where does it go". It is also not a Valgrind tool, so it does not
consume the one-Valgrind-tool budget that memcheck spends.

`heaptrack -o` auto-launches heaptrack_gui, which crashes in this environment
(a snap/glibc symbol clash unrelated to KArchive); the run script uses
`--record-only` and analyses with `heaptrack_print` instead.

---

## Workload

Write, then extract, **64 entries of 1 MiB each** (64 MiB of payload, half
compressible, half random) in each writable format. This is the shape of work
Ark or a KIO slave does, and it is where KArchive's habit of reading whole
entries into `QByteArray` actually costs something.

## Result

```
format        peak-heap     peak-RSS    allocations       leaked
tar              11.75M       27.25M         283445       59.72K
tar.gz           11.66M       27.14M         280264       59.72K
tar.zst          13.21M       29.12M         280477       59.72K
zip               4.45M       18.15M           5295       59.72K
7z             >> 2.84G <<    2.36G           3513       59.72K
```

Two things stand out.

## 1. K7Zip peaks at 2.84 GB to process 64 MiB — **CONFIRMED scalability defect**

7z uses **~240× the peak heap of the tar formats and ~640× that of zip** for the
same payload. heaptrack attributes 2.64 GB of the 2.84 GB peak to a single call
chain on the **write** path:

```
2.64G peak, 64 calls
   QByteArray::append(...)
   K7Zip::doWriteData(char const*, long long)   k7zip.cpp:3183
   KArchive::writeFile(...)                      karchive.cpp:421
```

Root cause is architectural. K7Zip has no streaming write path: `doWriteData`
accumulates every entry into one member buffer, `d->outData` — its own comment
says so (k7zip.cpp:480, *"Store data in this buffer before compress and write"*).
Then `closeArchive` makes several more **full copies** of that buffer:

- `createItemsFromEntities` (k7zip.cpp:1988) does
  `data.append(outData.mid(pos, size))` per entry — `mid()` allocates a fresh
  copy of each slice;
- the result is assigned back with `d->outData = data` (k7zip.cpp:3082);
- compression then produces yet another buffer.

So at peak the process holds the 64 MiB original plus several reallocating
copies of it, and `QByteArray`'s growth strategy inflates each. The tar and zip
writers, by contrast, stream to the device with bounded memory, which is why
they sit around 12 MiB and 4 MiB.

**Impact:** memory scales with total *uncompressed* archive size, several times
over. A 1 GiB 7z would attempt tens of GiB and OOM on most machines. This is not
a crash or a correctness bug, and it needs no malformed input — it is a
scalability defect on the ordinary write path, and only a profiler would have
surfaced it, since functionally everything works. Recorded as finding 4 in
`../unit_tests/FINDINGS.md`.

## 2. tar allocates 280k times, zip/7z ~5k — allocation-count contrast

The tar family makes **~280,000 allocation calls** for the same job zip does in
~5,300. This is the flip side of tar's low peak: it streams in small pieces (low
watermark) but churns many short-lived `QByteArray`s (Qt implicit-sharing
detaches, per-block filter I/O). It is not a defect — the peak is fine and the
total runtime is dominated by compression, not allocation — but it is the kind of
allocation-rate observation heaptrack exists to make, and it cleanly explains the
inverse relationship between peak heap and allocation count across the formats.

## Note on "leaked: 59.72K"

Identical across all formats and independent of the workload, so it is
process-fixed: Qt and the C runtime's one-time global allocations, held for the
process lifetime and reported at exit. Not a KArchive leak — memcheck
(`../valgrind/memcheck`) is the tool that adjudicates leaks, and it found only
the payload-free KRcc registration blocks.
