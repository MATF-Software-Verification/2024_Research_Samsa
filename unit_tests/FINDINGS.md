# Defects found by the test suites

Six defects in KArchive at commit `633dc09`, found across the tools of this
analysis and all reproduced deterministically. Findings 1-4 come from the test
suites and the memory tools; findings 5-6 come from the AFL++ campaign
(`../afl/TRIAGE.md`). None is reported by clang-tidy or cppcheck.

| # | Component | Severity | Status |
|---|---|---|---|
| 1 | `KRcc` | **crash on untrusted input** (SIGBUS/SIGSEGV) | reproduced, minimised; ASan-confirmed |
| 2 | `K7Zip` | data corruption — filenames | reproduced, root-caused |
| 3 | `KZstdFilter` | uninitialised-memory use | reproduced (memcheck), root-caused |
| 4 | `K7Zip` | memory scales pathologically on write (2.84 GB for 64 MiB) | measured (heaptrack), root-caused |
| 5 | `K7Zip` | **out-of-bounds read** on malformed stream graph | reproduced (AFL++), minimised to 153 B |
| 6 | `K7Zip` | **resource-amplification DoS** (153 B → >12 GB RAM) | reproduced (AFL++), root-caused |

---

## 1. KRcc crashes on a corrupted `.rcc` file

**Reproduces every time. One flipped byte is enough.**

### Symptom

Reading the contents of any entry of a `.rcc` whose header has been modified
kills the process with SIGBUS (signal 7) or SIGSEGV (signal 11). It was found by
`vs_robustnesstest`'s `byteCorruption` case, which aborted the whole suite:

```
Received signal 11 (SIGSEGV), code 2, for address 0x0000738fe59bc000
#11 KRccFileEntry::data (this=0x61ab517421e0) at vendor/karchive/src/krcc.cpp:53
```

### Minimised cases

Against the unmodified `vendor/karchive/autotests/data/runtime_resource.rcc`
(1569 bytes), altering a **single byte** and then reading the entries:

| Offset | New value | Result |
|---:|---:|---|
| 12 | 0xFF | **SIGSEGV** |
| 15 | 0x00 | **SIGBUS** |
| 15 | 0xFF | **SIGBUS** |
| 20 | 0xFF | **SIGBUS** |

`open()` alone is safe for all of these — measured separately. Only reading an
entry's data crashes, which is why `vs_robustnesstest` still exercises the `.rcc`
parse path and withholds only the read.

### Root cause

`KRcc::openArchive` hands the file straight to Qt's resource system
(`krcc.cpp:123`):

```cpp
if (!QResource::registerResource(fileName(), d->m_prefix)) {
    setErrorString(...);
    return false;
}
```

The return value *is* checked, but `registerResource` validates little more than
the header magic. Qt then **memory-maps** the file and trusts the tree, name and
data offsets inside it. `KRccFileEntry::data()` (`krcc.cpp:53`) reads through
that mapping:

```cpp
QFile f(m_resourcePath);
if (f.open(QIODevice::ReadOnly)) {
    return f.readAll();
}
```

With a corrupted offset, the read lands outside the mapped region. **SIGBUS is
the tell**: it is what you get for touching a page past the end of a
memory-mapped file, as opposed to ordinary heap corruption.

### Assessment

The proximate fault is arguably Qt's — `QResource` was designed for resources
compiled into the binary, which are trusted by construction. But the defect that
matters here is KArchive's: `KRcc` presents the same `KArchive` interface as
`KTar`, `KZip` and `K7Zip`, and callers such as Ark or KIO reasonably assume that
interface may be pointed at a file a user downloaded. **KArchive routes untrusted
input into an API that assumes trusted input**, and there is no validation in
between.

Worth noting: this is not a subtle bug, and `krcc.cpp` has the weakest branch
coverage in the library (37% at baseline). The two facts are related.

### Reproducing

```bash
./unit_tests/reproducers/build_and_run.sh
```

Source: `unit_tests/reproducers/krcc_crash.cpp`. It is kept out of the CTest
suite deliberately — a crash cannot be caught by QTest, so including it would
mean a suite that always aborts.

---

## 2. K7Zip corrupts every non-ASCII filename

**Reproduces every time.** Covered by `vs_roundtriptest`'s `nonAsciiFilenames`,
where the 7z rows are marked `QEXPECT_FAIL`, so the suite stays green while
continuing to assert the bug is present. If upstream fixes it, those rows become
XPASS and the suite says so.

### Symptom

Two distinct failures, depending on the character:

| Filename character | Result |
|---|---|
| US-ASCII | round trips correctly |
| U+0080 – U+00FF | archive opens, **filename silently corrupted** — `U+00E9` returns as `U+FFE9` |
| above U+00FF | **the archive K7Zip just wrote cannot be reopened by K7Zip** |

Measured:

```
wrote U+[ 0061 0062 ]  read U+[ 0061 0062 ]   MATCH        (ASCII "ab")
wrote U+[ 00E9 ]       read U+[ FFE9 ]        *** CORRUPTED ***
wrote U+[ 00E5 ]       read U+[ FFE5 ]        *** CORRUPTED ***
wrote U+[ 010D ]       UNREADABLE: Read size failed (checkRecordsSize: 1, d->pos - ppp: 3, size: 5)
```

### Root cause

`k7zip.cpp:2477-2487`, writing the `kName` record:

```cpp
for (int t = 0; t < name.length(); t++) {
    wchar_t c = name[t].toLatin1();
    writeByte((unsigned char)c);
    writeByte((unsigned char)(c >> 8));
}
```

The 7z format stores names as UTF-16LE, and the size computed earlier is correct
for that — `namesDataSize += (name.length() + 1) * 2`. The loop, however, funnels
each character through `QChar::toLatin1()`, which is wrong twice over:

1. **It returns a signed `char`.** For U+00E9 that is `(char)0xE9` = −23.
   Assigned to `wchar_t` it sign-extends to `0xFFFFFFE9`, so `c >> 8` yields
   `0xFF` and the code emits `E9 FF` — U+FFE9 instead of U+00E9. Silent
   corruption, and the archive still opens.

2. **It returns `'\0'` for anything it cannot represent**, i.e. everything above
   U+00FF. `'\0'` is 7z's name terminator, so the name is cut short mid-record.
   The reader then finds fewer name bytes than `namesDataSize` promised and
   rejects the file: `checkRecordsSize` fails having consumed 3 bytes of a
   declared 13.

So K7Zip produces archives that **it cannot read back itself**, and that no
conforming 7z implementation could read either.

### Assessment

A data-loss bug on the write path, reachable by any caller archiving files whose
names are not pure ASCII — which on a KDE desktop is routine. `KTar` and `KZip`
handle all four of our test names correctly, so this is specific to `K7Zip`.

The fix is to encode the name as real UTF-16LE, e.g. via `QString::utf16()` or a
`QStringEncoder`, rather than per-character `toLatin1()`. Note `decryptAES` in the
same file already uses `QStringEncoder(QStringEncoder::Utf16LE)` for exactly this
job — the correct idiom is present in the file, 1100 lines away.

---

## 3. KZstdFilter uses an uninitialised struct member on empty input

Found by memcheck, not by the QTest assertions — the program behaves correctly,
so only a memory checker sees it. Full triage: `../valgrind/memcheck/TRIAGE.md` §1.

### Symptom

Closing a `.tar.zst` (or any zstd-compressed archive) to which **no data was
written** makes `ZSTD_compressStream2` act on an uninitialised pointer. memcheck,
with `--track-origins=yes`:

```
Conditional jump or move depends on uninitialised value(s)
   at ZSTD_compressStream2                    (libzstd)
   by KZstdFilter::compress(bool)             kzstdfilter.cpp:126
   by KCompressionDevice::writeData(...)      kcompressiondevice.cpp:479
   by KCompressionDevice::close()             kcompressiondevice.cpp:291
   by KArchive::close()                       karchive.cpp:254
Uninitialised value was created by a heap allocation
   by KZstdFilter::KZstdFilter()              kzstdfilter.cpp:32
```

### Root cause

`KZstdFilter::Private` holds `ZSTD_inBuffer inBuffer` and `ZSTD_outBuffer
outBuffer` as members (kzstdfilter.cpp:27-28). `new Private` does not
value-initialise them, and `init()` sets only `inBuffer.size` and `inBuffer.pos`
(lines 46-47). When an archive is closed with nothing written, `setInBuffer()`
is never called, so `inBuffer.src` is still garbage when zstd reads it.

### Assessment

Low severity: zstd does not dereference `src` when `size == 0`, so there is no
crash and the output is correct. But it is a real uninitialised-memory use in
KArchive's own code, on the ordinary empty-archive edge case, and the fix is one
line — value-initialise the struct (`ZSTD_inBuffer inBuffer{};`) or set
`inBuffer.src = nullptr` in `init()`. ASan did not catch it (the memory is
initialised as far as ASan's shadow is concerned — it is *uninitialised value*
use, which only memcheck tracks).

### Reproducing

`./unit_tests/reproducers/build_and_run.sh` builds it; or directly:

```bash
valgrind --track-origins=yes ./kzstd_uninitialised
```

Source: `unit_tests/reproducers/kzstd_uninitialised.cpp`.

## 4. K7Zip write path uses pathological amounts of memory

Found by heaptrack, not by any correctness check — the writer works, it just
uses ~240x the memory the other formats do. Full triage:
`../heaptrack/TRIAGE.md` §1.

### Symptom

Writing a 64 MiB archive peaks at **2.84 GB** of heap for 7z, versus ~12 MiB for
tar and ~4.5 MiB for zip:

```
format     peak-heap
tar          11.75M
zip           4.45M
7z         >> 2.84G <<
```

### Root cause

K7Zip has no streaming write path. `doWriteData` accumulates every entry into
one member buffer `d->outData` (k7zip.cpp:480, 3183), and `closeArchive` then
makes several full copies of it: `createItemsFromEntities` copies each entry via
`outData.mid()` (k7zip.cpp:1988), the result is assigned back with
`d->outData = data` (3082), and compression allocates another buffer. Memory
therefore scales with total uncompressed size, several times over, inflated
further by QByteArray's reallocating growth.

### Assessment

Not a crash or a correctness bug, and no malformed input is involved — a
scalability defect on the ordinary write path. A 1 GiB 7z would attempt tens of
GiB and OOM. Only a profiler surfaces this, because functionally everything
works. A fix would stream entries to the device instead of buffering the whole
archive, which is a larger change than the one-liners in findings 2 and 3.

### Reproducing

```bash
./heaptrack/run_heaptrack.sh
```

## 5. K7Zip out-of-bounds read on a malformed stream graph

Found by AFL++. Full triage and stack: `../afl/TRIAGE.md` §"Finding 5";
minimised reproducer `../afl/crashes/k7z_getOutStream_oob.7z` (153 bytes).

### Symptom

A crafted 7z drives `getOutStream` to index a `QList` past its end while
parsing the coder stream graph:

```
QList::at index out of range          qlist.h:453
K7Zip::readAndDecodePackedStreams     k7zip.cpp:1842
K7Zip::openArchive                    k7zip.cpp:2901
```

In this Qt debug build the bounds assertion aborts; with assertions off it is an
out-of-bounds read.

### Root cause

`findInStream` (k7zip.cpp:375) is `void` and simply falls through when a stream
index is not found in the coder graph, leaving `coderIndex == folderInfos.size()`
with no error signalled. The caller then does `folderInfos[coderIndex]`, one past
the end. Reachable on the plain open path, no password needed.

### Assessment

A memory-safety defect on the untrusted-input path, distinct from findings 1-4:
the stream-graph helpers were not exercised by the corpus the other tools ran on.
The fix is to give `findInStream`/`findOutStream` a failure return and check it.

## 6. K7Zip resource-amplification DoS

Found by AFL++ (as "hangs"). Full triage and measurements: `../afl/TRIAGE.md`
§"Finding 6"; reproducers `../afl/hangs/k7z_amplification_*.7z` (153 bytes each).

> Reproduce only under a memory cap:
> `ASAN_OPTIONS=hard_rss_limit_mb=2048 timeout 10 <fuzzer> <file>`.
> Uncapped it can exhaust system RAM.

### Symptom

A 153-byte 7z makes K7Zip consume 8-12 GB of RAM and 28-160 s of CPU before
aborting with Qt's *"Out of memory"* at `qarraydataops.h:276`.

### Root cause

Attacker-controlled count fields in the 7z header, read as variable-length
numbers up to 2^64, are used directly to size containers with no check against
the remaining input length:

```cpp
folder->folderInfos.reserve(numCoders);          // k7zip.cpp:777
packCRCsDefined.resize(numPackStreams, false);   // k7zip.cpp:939
```

A one-byte field claiming ~10^8 coders makes reserve request billions of bytes
at once. Same class as finding 4's write-side blow-up and as the "infinite loop
in malformed file" fixes in KArchive's own history.

### Assessment

A denial-of-service on the read path: a tiny hostile archive exhausts the memory
of any process that opens it (Ark, a KIO slave, a file-manager thumbnailer). The
defence is to validate declared counts against the actual input size before
allocating.

## Status

Both are reproducible against upstream `633dc09` with no local modifications to
the submodule. Neither has been reported upstream yet; both are candidates.

Finding 1 also gives the fuzzing stage a concrete target: `.rcc` is the format
with the weakest branch coverage, the least validation, and a demonstrated crash
from single-byte mutation — which is precisely what a mutational fuzzer does.
