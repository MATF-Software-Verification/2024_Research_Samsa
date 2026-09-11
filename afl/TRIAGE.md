# AFL++ — fuzzing campaign, results and triage

Tool: AFL++ 4.09c (afl-clang-fast, LLVM 17) with AddressSanitizer
Target: KArchive at commit `633dc09`, OSS-Fuzz harnesses in `autotests/ossfuzz`
Reproduce: `./afl/build_fuzzers.sh` then `./afl/run_campaign.sh k7z 300`
Build script: [`build_fuzzers.sh`](build_fuzzers.sh) · campaign: [`run_campaign.sh`](run_campaign.sh)

AFL++ is one of the two tools not covered in the exercises (the course taught
libFuzzer, not AFL++). It reuses KArchive's existing `LLVMFuzzerTestOneInput`
harnesses unchanged, driven through AFL++'s `libAFLDriver.a` via the harnesses'
own `LIB_FUZZING_ENGINE` hook — so nothing in the analysed project is patched.

> **Memory warning.** Two of the findings below make K7Zip allocate many
> gigabytes from a 153-byte input. Reproduce saved hangs only under a memory
> cap (`ASAN_OPTIONS=hard_rss_limit_mb=2048 timeout 10 …`). An uncapped run can
> exhaust system RAM.

---

## Campaign

`k7z_fuzzer`, seeded from the 37 sample 7z archives in `autotests/data` plus one
hand-crafted AES-header seed (`seeds/aes_header.7z`), with upstream's
`k7z_fuzzer.dict`. A single core:

```
run_time        196 s
execs_done      264,143      (1342/s)
stability       97.93%
bitmap_cvg      14.40%
saved_crashes   4
saved_hangs     7
```

Short by fuzzing standards, and it still produced two distinct, reproducible
defect classes. KArchive is already in OSS-Fuzz, so novel crashes were not
expected — the value here is that the harness reaches them at all on a
workstation in minutes, and that both classes reproduce deterministically for
the report.

## Finding 5 — out-of-bounds read in K7Zip stream-graph parsing (CONFIRMED)

All 4 saved crashes share one root cause. Minimised to **153 bytes**
(`crashes/k7z_getOutStream_oob.7z`, stack in `.stack.txt`):

```
QList::at index out of range          qlist.h:453
getOutStream(...)                     k7zip.cpp (via qlist.h:453)
K7Zip::readAndDecodePackedStreams     k7zip.cpp:1842
K7Zip::openArchive                    k7zip.cpp:2901
KArchive::open                        karchive.cpp:192
```

Root cause is in the folder/coder stream-graph helpers. `findInStream`
(k7zip.cpp:375) is declared `void` and walks the coders subtracting stream
counts:

```cpp
void findInStream(quint32 streamIndex, quint32 &coderIndex, ...) const {
    for (coderIndex = 0; coderIndex < folderInfos.size(); coderIndex++) {
        quint32 curSize = folderInfos[coderIndex].numInStreams;
        if (streamIndex < curSize) { ...; return; }
        streamIndex -= curSize;
    }
    // falls through: coderIndex == folderInfos.size(), no error signalled
}
```

When a malformed archive references a stream index the coder graph does not
contain, the loop falls through leaving `coderIndex == folderInfos.size()`. The
caller (`getOutStream`, and the sibling path at k7zip.cpp:1301) then does
`folderInfos[coderIndex]` / `folder->inIndexes[binderIndex]`, indexing one past
the end. In this Qt debug build `QList::at` asserts and aborts; in a release
build (assertions off) it is a genuine **out-of-bounds read**.

This is a memory-safety defect on the untrusted-input open path, reachable with
no password and no special build options. It is distinct from every static and
dynamic finding so far — the stream-graph helpers were not exercised by the
corpus the other tools ran on.

## Finding 6 — resource-amplification DoS in K7Zip header parsing (CONFIRMED)

5 of the 7 saved hangs are not infinite loops — they terminate, but a **153-byte**
input drives K7Zip to consume enormous CPU and memory before it does:

| hang input | wall time | peak RSS |
|---|---:|---:|
| `k7z_amplification_1.7z` | 35 s | 9.1 GB |
| (id 000002) | 48 s | 12.0 GB |
| (id 000003) | 160 s | 12.1 GB |
| (id 000004) | 49 s | 9.5 GB |
| `k7z_amplification_2.7z` | 28 s | 8.2 GB |

Capped, the process aborts with Qt's own *"Out of memory"* in
`qarraydataops.h:276` — i.e. it dies inside a `QList`/`QByteArray` growth. The
mechanism is unbounded allocation from attacker-controlled count fields in the
7z header, which are read as variable-length numbers (`readNumber()`, up to
2⁶⁴) and used directly to size containers, with no check against the actual
input size. Representative sites:

```cpp
folder->folderInfos.reserve(numCoders);          // k7zip.cpp:777
packCRCsDefined.resize(numPackStreams, false);   // k7zip.cpp:939
packCRCs.resize(numPackStreams, 0);              // k7zip.cpp:940
```

A one-byte field claiming, say, 10⁸ coders makes `reserve` request billions of
bytes immediately. This is the same class as the write-side blow-up heaptrack
found (finding 4) and as the *"infinite loop in malformed file"* fixes in
KArchive's own recent history — an amplification / denial-of-service on the read
path: a tiny hostile archive exhausts the memory of any process that opens it.

The general defence is to validate declared counts and sizes against the
remaining input length before allocating — a bounded reader cannot legitimately
contain 10⁸ coders in 153 bytes.

## The two non-hang "hangs"

The other 2 of 7 saved hangs (id 000001, 000005) complete in ~5–6 s and are
merely slow decompression of the sample corpus, not defects. AFL flagged them
against its 2 s per-exec timeout; they are noted here so the count reconciles.

## The AES path

The k7z fuzzer is built with `-DUSE_PASSWORD=1` (upstream enables it when
OpenSSL is present), which sets a dummy password and so exercises the
AES-decryption path that carries the two static-analysis findings from
`../cppcheck/TRIAGE.md` §6. The 196 s campaign did not surface a crash there —
reaching `decryptAES` needs a validly-structured AES coder the mutator did not
assemble in that budget, which is why the hand-crafted `seeds/aes_header.7z`
seed exists. A longer campaign from that seed is the natural next step; within
this analysis those two findings remain source-confirmed but not yet
fuzzer-triggered, and this document does not claim otherwise.

## Conclusion

AFL++ found the two memory-safety/DoS defects (5 and 6) that the corpus-driven
tools did not, both on the untrusted-input open path, both reproducible from a
153-byte file. Together with findings 1–4 they make the case that K7Zip's
malformed-input handling is the weakest part of the library — which is exactly
where its upstream commit history has been concentrated.
