# Valgrind memcheck — results and triage

Tool: valgrind 3.22.0 (memcheck)
Target: KArchive at commit `633dc09`, uninstrumented `-O1 -g` build
Reproduce: `./valgrind/memcheck/run_memcheck.sh`
Outputs: per-suite logs plus [`summary.txt`](summary.txt)

memcheck is the one Valgrind tool counted (the rules allow at most one). It was
chosen over callgrind/massif because it does the one thing nothing else in this
analysis can: it needs no recompilation, so it sees **inside the uninstrumented
system compression libraries** where ASan is blind, and it detects **use of
uninitialised memory**, which ASan cannot detect at all.

That choice paid off — memcheck found a KArchive defect that ASan's clean run
did not (§1).

---

## Raw results

```
vs_robustnesstest   2 errors  | definitely lost: 0 bytes in 2 blocks
vs_roundtriptest    4 errors  | definitely lost: 0 bytes in 0 blocks
karchivetest       16 errors  | definitely lost: 0 bytes in 0 blocks
```

No suppression file is used. The findings are few, and the only suppressible
noise (§3) has fully unsymbolised error stacks, so any suppression would have to
be `obj:*`-broad and could hide a real defect — and §1 shows real defects are
present. Each class is triaged instead.

## 1. Uninitialised member in KZstdFilter — **CONFIRMED KArchive defect**

Every uninitialised-value finding in `vs_roundtriptest` (all of them, via
`--track-origins`) has the same origin:

```
Uninitialised value was created by a heap allocation
   by KZstdFilter::KZstdFilter()  kzstdfilter.cpp:32
...used at:
   ZSTD_compressStream2                     (libzstd)
   KZstdFilter::compress(bool)              kzstdfilter.cpp:126
   KCompressionDevice::writeData(...)       kcompressiondevice.cpp:479
   KCompressionDevice::close()              kcompressiondevice.cpp:291
   KArchive::close()                        karchive.cpp:254
```

`KZstdFilter::Private` (kzstdfilter.cpp:20-29) holds two C structs as members:

```cpp
ZSTD_inBuffer  inBuffer;   // { const void *src; size_t size; size_t pos; }
ZSTD_outBuffer outBuffer;  // { void *dst; size_t size; size_t pos; }
```

`new Private` does not value-initialise them, and `init()` (line 46-47) sets only
`inBuffer.size` and `inBuffer.pos`. So `inBuffer.src` stays garbage until
`setInBuffer()` is called — and when an archive is closed with **no data ever
written**, `setInBuffer()` is never called, so `ZSTD_compressStream2` acts on an
uninitialised `src`.

**Reproduced in 12 lines** (`../../unit_tests/reproducers/kzstd_uninitialised.cpp`):
open a `.tar.zst` for writing, write nothing, close. In practice zstd does not
dereference `src` when `size == 0`, so no crash results and the compressed output
is correct — the severity is low. But it is a genuine uninitialised-memory use in
KArchive's own code, on an ordinary edge case (an empty archive), and the fix is
one line: value-initialise the struct, e.g. `ZSTD_inBuffer inBuffer{};` or set
`inBuffer.src = nullptr` in `init()`.

This is finding 3 in `../../unit_tests/FINDINGS.md`.

## 2. Zero-byte leak in KRcc::openArchive — real, low severity

`vs_robustnesstest` reports two `definitely lost: 0 bytes` blocks, both:

```
   KRcc::openArchive(...)   krcc.cpp:123   (QResource::registerResource)
   KArchive::open(...)      karchive.cpp:192
```

They come from the `.rcc` inputs that register successfully. The lost blocks
carry no payload bytes (0 bytes), so this is a small bookkeeping allocation from
Qt's resource registration that is not released — most likely a QResource-internal
record rather than a KArchive logic error, but it is charged to KArchive's call
site and is worth noting. Low severity: the leak is bounded and payload-free.

## 3. Uninitialised values inside Qt's calendar — external, not KArchive

`karchivetest`'s 16 findings all originate inside
`QGregorianCalendar::julianFromParts` (qgregoriancalendar.cpp:235), reached from
tar/zip timestamp handling. The value is created and consumed within Qt; the
error stacks contain no KArchive frames (they are unsymbolised Qt code). This is
Qt-internal and not actionable here. It is left visible rather than suppressed,
because the only possible suppression would be broad enough to also hide findings
like §1.

## Conclusion

memcheck earned its place. On a codebase where ASan reported nothing, its unique
ability to track uninitialised memory across the C-library boundary surfaced a
real KArchive defect (§1) that no other tool in this analysis detected. The
remaining findings are a payload-free Qt-registration leak (§2) and Qt-internal
calendar noise (§3).
