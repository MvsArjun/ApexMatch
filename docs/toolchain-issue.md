# libstdc++ pool-resource allocation-failure issue

Status on 18 October 2026: **open dependency failure reproduced on Windows and
Linux**. The native w64devkit GCC 16.2/libstdc++ runtime and the Ubuntu 24.04
runner's default libstdc++ segfault under a specific injected pool allocation
failure. Linux Clang 21.1.8/libc++ 21.1.8 passed the complete suite in Debug,
Release, ASan/UBSan with leak detection enabled, and separate TSan jobs. The
mandatory regression and original failing matrix remain enabled. No test was
skipped, no assertion weakened, no library implementation patched, and no
issue was filed externally.

The public [run 39149291801](https://github.com/MvsArjun/ApexMatch/actions/runs/39149291801)
at commit `8337a3d91200e990ab0586859871606ffe8136e4` completed with four successful
libc++ jobs and seven failed libstdc++ jobs. The overall workflow is therefore
failed. [Recorded job evidence](../results/hosted-ci/39149291801/README.md)
preserves both outcomes.

## Observed application failure

The initial native result below is preserved. Hosted verification subsequently
reproduced the issue independently:

| Observed compiler and library | Configurations | CTest result per job | Standalone probe |
|---|---|---|---|
| Native w64devkit GCC 16.2/libstdc++ | Debug, Release | 5/6 groups passed; C++ group segfaulted | Windows access violation |
| Ubuntu 24.04 GCC 13.3.0, runner-default libstdc++ | Debug, Release, ASan/UBSan | 5/6 groups passed; mandatory allocation case failed | Exit 139, segmentation fault |
| Ubuntu 24.04 Clang 18.1.3, runner-default libstdc++ | Debug, Release, ASan/UBSan, TSan | 5/6 groups passed; mandatory allocation case failed | Exit 139, segmentation fault |
| Ubuntu 24.04 Clang 21.1.8/libc++ 21.1.8 | Debug, Release, ASan/UBSan, TSan | 6/6 groups passed, no skipped tests | Exit 0, `bad_alloc` propagated safely |

Compiler version does not identify the shared libstdc++ package version. The
Linux failure logs record `libstdc++.so.6` build ID
`753c6c8608b61d4e67be8f0c890e03e0aa046b8b`; the installed runtime package version
was not recorded. Clang 18 diagnostics reference GCC 14 headers, so describing
this shared runtime simply as “libstdc++ 13.3” would be unsupported.

Fresh GCC Debug and Release builds completed. Their CTest results were five
groups passed and one group failed: `core_and_extensions` terminated with a
segmentation fault. The other groups were replay, network, extension examples,
benchmark tooling and release tooling. Their success does not make the GCC
C++ group a passing result.

Unbuffered test output and GDB localized the crash to
`allocation_failure_destruction`, pool mode, upstream allocation allowance 2,
on the first `MatchingEngine::add`. Twelve earlier named core cases had passed.
The first accepted-ID node had not yet been inserted. A stack excerpt was:

```text
std::pmr::unsynchronized_pool_resource::do_allocate(...)
std::pmr::memory_resource::allocate(...)
std::pmr::polymorphic_allocator<hash node>::allocate(...)
std::unordered_set<...>::insert(...)
apexmatch::MatchingEngine::add(...)
allocation_failure_destruction()
```

Original failure evidence is retained under
[`results/audit-verification`](../results/audit-verification). The final
configuration matrix belongs to [verification.md](verification.md).

## Standalone reproduction

[`tools/repro_pool_failure.cpp`](../tools/repro_pool_failure.cpp) uses only the
standard library. It permits two successful allocations from an injected
upstream, then throws `std::bad_alloc`. It constructs an unsynchronized pool and
requests up to 128 small blocks. Requesting multiple blocks ensures both tested
libraries reach their third upstream allocation, despite different chunking.
The pool owns all its allocations when unwinding. ApexMatch code and containers
are not part of this program.

From the repository root in PowerShell, with the selected compiler and its
matching runtime on PATH and an existing `build` directory:

```powershell
g++ -std=c++20 -O0 -g tools/repro_pool_failure.cpp -o build/pool_probe_gcc.exe
.\build\pool_probe_gcc.exe
gdb -batch -ex run -ex bt --args build/pool_probe_gcc.exe

# Switch PATH to the tested LLVM-MinGW/Clang toolchain before this comparison.
clang++ -std=c++20 -O0 -g tools/repro_pool_failure.cpp -o build/pool_probe_clang.exe
.\build\pool_probe_clang.exe
```

The GCC 16.2 probe printed:

```text
upstream allocate 528 alignment 8 remaining 2
upstream allocate 1016 alignment 16 remaining 1
upstream allocate 192 alignment 8 remaining 0
upstream deallocate 1016
```

It exited with Windows access violation `0xc0000005` (unsigned status
3221225477; PowerShell can display signed status -1073741819). GDB placed the
fault inside the pool's allocation implementation.

The native LLVM-MinGW Clang 23.1.3/libc++ probe printed:

```text
upstream allocate 288 alignment 8 remaining 2
upstream allocate 1048 alignment 16 remaining 1
upstream allocate 1320 alignment 16 remaining 0
upstream deallocate 1048
upstream deallocate 288
bad_alloc propagated safely
```

It exited 0. Allocation sizes are implementation details; the comparison is
exception propagation and unwinding. Portable logs, compiler versions, source
and executable hashes, exit statuses and the standalone GDB stack are in
[`pool-probe/result.json`](../results/audit-verification/pool-probe/result.json)
and the adjacent log files. The complete Clang application failure-injection
case also passed, as recorded in the verification matrix.

## Diagnosis and limits

Inspection of the [GCC 16 branch's memory_resource.cc](https://github.com/gcc-mirror/gcc/blob/releases/gcc-16/libstdc%2B%2B-v3/src/c%2B%2B17/memory_resource.cc)
supports this explanation: when chunk metadata insertion throws, the replenish
handler frees the newly acquired chunk but does not rethrow. Its caller then
accesses the chunk vector's last element even when that vector is empty. The
local disassembly and engine-free failure corroborate this source-based
inference. The project does not claim to have rebuilt or patched libstdc++.

Hosted Linux sanitizer failures also locate the invalid read inside
`std::pmr::unsynchronized_pool_resource::do_allocate`, and all seven original
jobs independently reproduce the same 528/1016/192-byte failure trace with no
engine code. This broadens the observed dependency issue beyond the native
GCC 16.2 installation. It does not establish that every libstdc++ release is
affected, that ordinary allocations fail, or that matching rules are wrong.
Safe unwinding under this injected failure cannot be claimed for these tested
libstdc++ runtimes.

The four passing Clang 21/libc++ jobs each ran all six CTest groups, including
the allocation-failure regression, 29 named C++ cases, 40,000 differential
requests and 13 Python tooling cases. Linux network coverage includes the POSIX
SIGTERM drain check. The ASan preset enables AddressSanitizer and
UndefinedBehaviorSanitizer with `ASAN_OPTIONS=detect_leaks=1`; the separate TSan
preset uses `TSAN_OPTIONS=halt_on_error=1`. No sanitizer diagnostic was observed
in those passing jobs. Their standalone dependency probes are uninstrumented;
the full application allocation-failure test is instrumented in sanitizer jobs.
Hosted logs report group totals; the individual-case/request counts come from
the test inventory at the tested commit.

The engine-free Linux comparison in the workflow can also be repeated directly:

```sh
mkdir -p build
g++ -std=c++20 -g tools/repro_pool_failure.cpp -o build/pool-probe-libstdcxx
./build/pool-probe-libstdcxx
clang++-21 -std=c++20 -stdlib=libc++ tools/repro_pool_failure.cpp -o build/pool-probe-libcxx
./build/pool-probe-libcxx
```

The observed libstdc++ probe exits 139; the observed libc++ probe exits 0 after
propagating the injected exception safely. These commands reproduce a dependency
failure investigation, and are separate from the instrumented application tests.

To reproduce the passing Linux library configuration, use a fresh checkout or
fresh build directories with Clang 21 and its matching libc++ installed:

```sh
cmake --preset release -DCMAKE_CXX_COMPILER=clang++-21 -DCMAKE_CXX_FLAGS=-stdlib=libc++
cmake --build --preset release --parallel 2
ctest --preset release
```

Substitute `debug`, `asan` or `tsan` for their independent builds. The workflow
records the LLVM package setup and TSan host shadow-memory configuration; inspect
the [verified workflow](https://github.com/MvsArjun/ApexMatch/blob/8337a3d91200e990ab0586859871606ffe8136e4/.github/workflows/ci.yml)
and [job evidence](../results/hosted-ci/39149291801/README.md) before transferring
those commands to another host.

ApexMatch already treats resource exhaustion as fatal to a session and promises
no transactional rollback. Safe unwinding still matters to the audit, and a
dependency access violation is not silently reclassified as a successful
exception test. The independent passing libc++ matrix is useful verification;
it does not resolve the retained libstdc++ failures. Resolution requires verifying
a corrected libstdc++ runtime with the standalone reproducer, then rerunning the
unchanged complete application suite.
