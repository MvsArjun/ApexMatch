# Native GCC pool-resource allocation-failure issue

Status on 18 October 2026: **open dependency failure**. The native w64devkit
GCC 16.2/libstdc++ runtime segfaults under a specific injected pool allocation
failure. The mandatory application regression remains enabled. No test was
skipped, no assertion weakened, no standard library replaced, and no issue was
filed externally.

## Observed application failure

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

The demonstrated dependency failure is specific to the tested native GCC 16.2
runtime. Other GCC versions, Linux libstdc++ builds and hosted CI must be tested
independently. The failure does not establish that ordinary allocations or
matching rules fail; it does establish that the full GCC suite fails and that
safe pool unwinding under this resource failure cannot be claimed there.

ApexMatch already treats resource exhaustion as fatal to a session and promises
no transactional rollback. Safe unwinding still matters to the audit, and a
dependency access violation is not silently reclassified as a successful
exception test. Future resolution is to verify a corrected runtime with this
standalone reproducer, then rerun the unchanged complete application suite.
