# AddressSanitizer + UndefinedBehaviorSanitizer

Tools: clang/g++ ASan + UBSan (LLVM/GCC 18/13)
Target: KArchive at commit `633dc09`
Reproduce: `./sanitizers/run_sanitizers.sh`
Outputs: [`asan_ubsan.log`](asan_ubsan.log), [`summary.txt`](summary.txt),
[`krcc_asan_stack.txt`](krcc_asan_stack.txt)

ASan and UBSan are counted as one dynamic-analysis item. ASan is also the
runtime under which the fuzzing stage runs (see `../afl`): a fuzzer without a
sanitizer finds only crashes that happen to fault, whereas with ASan it finds
the far larger set of memory errors that would otherwise corrupt silently.

Instrumentation is injected from the top-level CMakeLists.txt before
`add_subdirectory(vendor/karchive)`, so it covers KArchive's own sources without
any patch — the analysed code stays byte-identical to upstream.

---

## 1. The suite runs clean

All seven suites — KArchive's own autotests plus our `vs_robustnesstest`
(187 malformed-input cases) and `vs_roundtriptest` (41 cases) — pass under
ASan+UBSan with **no findings**:

```
RESULT: clean -- no ASan or UBSan findings across any suite
100% tests passed, 0 tests failed out of 7
```

This is a real and non-trivial result. `vs_robustnesstest` throws thousands of
truncated and corrupted archives at every parser, and none produced a
detectable memory error or undefined operation on the paths those inputs reach.
In particular, the two `decryptAES` findings from static analysis
(`../cppcheck/TRIAGE.md` §6) did **not** fire here — because none of the corpus
or generated inputs reach that OpenSSL-gated, password-protected code path. That
is expected, and is exactly why it is handed to the fuzzing stage with a
targeted seed (`../afl/seeds/aes_header.7z`), not claimed as disproven.

## 2. ASan confirms and localises the KRcc crash

The KRcc crash from the tests stage (`../unit_tests/FINDINGS.md` §1) is not in
the clean run above, because `vs_robustnesstest` deliberately withholds entry
reads for `.rcc` to avoid aborting the suite. Run directly against the crashing
input, ASan turns the bare SIGBUS/SIGSEGV into a precise diagnosis
(`krcc_asan_stack.txt`):

```
ERROR: AddressSanitizer: unknown-crash on address 0x7ac079227000
    #2 QResourceFileEngine::read(char*, long long)   qresource.cpp:1479
    #3 QFileDevice::readData(char*, long long)        qfiledevice.cpp:471
    #5 QIODevice::readAll()                           qiodevice.cpp:1262
    #6 KRccFileEntry::data() const                    krcc.cpp:53
```

The faulting operation is a `memcpy` inside Qt's resource engine reading from the
memory-mapped `.rcc`, entered through `KRccFileEntry::data()` at `krcc.cpp:53`.
This is the instruction-level confirmation of the root cause argued in the tests
stage: KArchive routes an untrusted file into `QResource`, whose read trusts the
mmap'd offsets. ASan reports it as `unknown-crash` rather than
`heap-buffer-overflow` precisely because the memory is a file mapping, not an
ASan-instrumented heap allocation — the same fact the SIGBUS signalled.

## 3. What this tool does and does not cover

ASan instruments only code compiled with it. KArchive's four compression
backends — zlib, bzip2, liblzma, zstd — are system libraries linked
un-instrumented, so buffer errors *inside* them are invisible here, and ASan
cannot detect reads of uninitialised memory at all. Both gaps are the specific
reason memcheck is also run (`../valgrind`): it needs no recompilation, so it
sees into those C libraries, and it detects uninitialised reads, which for a
parser filling buffers from partial input is a real bug class. The two tools are
deployed for different jobs, not redundantly.
