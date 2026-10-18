# GitHub publication record

## Published repository and approvals

[MvsArjun/ApexMatch](https://github.com/MvsArjun/ApexMatch) is public, with
`master` as its default branch and `origin` set to
`https://github.com/MvsArjun/ApexMatch.git`. The user explicitly approved the
repository name, public portfolio publication, destination account `MvsArjun`,
MIT license, and preservation of existing history including its old personal
path strings. No history was rewritten and no force push was used.

The initial push published the reviewed audit revision
`4f09688f73fb0440014656fd56e46753a444f7a1`. Subsequent commit
`8337a3d91200e990ab0586859871606ffe8136e4` added independent libc++ CI checks
and first-run evidence. Later documentation/evidence commits record actual
outcomes; the tested source/workflow revision is identified explicitly below.
Git Credential Manager authenticated the approved account for the GitHub API
and HTTPS push. No credential was written into tracked files or logs.

## Actual CI results

The [first run](https://github.com/MvsArjun/ApexMatch/actions/runs/39148962889)
at `4f09688` completed with all seven jobs failing `core_and_extensions` in
the injected pool-allocation-failure regression. Every job configured and built,
and each passed the other five CTest groups. Both GCC and default Linux Clang
used libstdc++; changing the compiler did not change that dependency.

The [second run](https://github.com/MvsArjun/ApexMatch/actions/runs/39149291801)
at `8337a3d` completed with **four successful jobs and seven failed jobs**:

| Runtime/configuration | Actual result |
|---|---|
| Clang 21.1.8 / libc++ 21.1.8 Debug | 6 groups passed, 0 failed |
| Clang 21.1.8 / libc++ 21.1.8 Release | 6 groups passed, 0 failed |
| Clang 21.1.8 / libc++ 21.1.8 ASan + UBSan, leak detection enabled | 6 groups passed, 0 failed; no sanitizer diagnostic |
| Clang 21.1.8 / libc++ 21.1.8 TSan | 6 groups passed, 0 failed; no sanitizer diagnostic |
| GCC 13.3 / libstdc++ Debug, Release, ASan | Each 5 groups passed, 1 failed in the mandatory allocation regression |
| Clang 18.1.3 / libstdc++ Debug, Release, ASan, TSan | Each 5 groups passed, 1 failed in the same regression |

All seven libstdc++ jobs also crashed in the standalone standard-library-only
probe. All four libc++ probes propagated `bad_alloc` safely. The probes were
compiled without sanitizer flags; the full ASan/UBSan and TSan CTest runs used
instrumented application/core targets. See [sanitized CI evidence](../results/hosted-ci/39149291801/summary.json),
[verification](verification.md) and [dependency analysis](toolchain-issue.md).

The workflow remains **red**. The failing jobs and regression have not been
disabled, weakened, or marked as allowed failures. Documentation and result-only
pushes are ignored by the workflow; a later documentation commit does not imply
a new source validation run. A manual workflow dispatch remains available.

## Verified Linux toolchain

The passing jobs run on Ubuntu 24.04. The following installation is the same
signed [official LLVM APT repository](https://apt.llvm.org/) used in the workflow:

```sh
sudo apt-get update
sudo apt-get install -y ca-certificates curl gnupg cmake ninja-build python3
curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor | sudo tee /usr/share/keyrings/apexmatch-llvm.gpg >/dev/null
echo 'deb [signed-by=/usr/share/keyrings/apexmatch-llvm.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-21 main' | sudo tee /etc/apt/sources.list.d/apexmatch-llvm.list
sudo apt-get update
sudo apt-get install -y clang-21 libc++-21-dev libc++abi-21-dev
cmake --preset release -DCMAKE_CXX_COMPILER=clang++-21 -DCMAKE_CXX_FLAGS=-stdlib=libc++
cmake --build --preset release --parallel 2
ctest --preset release
```

Use fresh build directories when switching compiler or standard library.
Substitute `debug`, `asan`, or `tsan` consistently for the other tested presets.
The TSan jobs additionally set `sudo sysctl vm.mmap_rnd_bits=28` before testing.
This toolchain is an additional verified configuration; the original libstdc++
jobs remain visible and failing.

## Published content and retained limitations

* [Audit report](audit-report.md): corrections, measurements, exact verification,
  publication and remaining work.
* [Release manifest](release-manifest.md): tracked changes from `bf0e409`.
* [Publication scan](../results/publication-verification/release-scan.json):
  current files and historical pattern/artifact checks.
* [Publication checks](../results/publication-verification/checks.json):
  workflow syntax, local documentation links, evidence consistency and source freeze.
* [Benchmark evidence](../results/audit-native-clang23/summary.md): 108 audited
  observations, with historical observations preserved separately.

Current publishable files have personal absolute paths redacted. Existing
history retains **11 historical blobs with personal path strings**, explicitly
approved for publication. No matched credential pattern was found; a pattern
scan cannot prove the absence of every secret. Compiler installations, local
build trees, executables, archives and temporary profiling data are excluded.

Implementation and approved publication are complete. Verification remains
limited by the reproduced libstdc++ resource-exhaustion defect and unavailable
CPU stack/hardware-counter profiling. Passing libc++ sanitizer runs apply to
the exercised tests, not to all possible executions or production guarantees.
