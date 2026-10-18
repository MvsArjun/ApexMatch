# Hosted CI evidence: run 39149291801

[GitHub Actions run](https://github.com/MvsArjun/ApexMatch/actions/runs/39149291801), attempt 1, tested commit `8337a3d91200e990ab0586859871606ffe8136e4` on Ubuntu 24.04.5 LTS (image `20261012.334.1`). The run finished on 2026-10-18 at 17:23:54 UTC.

All 11 jobs configured and built successfully. Four Clang 21.1.8 / libc++ 21.1.8 jobs passed all six CTest groups: Debug, Release, ASan/UBSan with leak detection, and TSan. No AddressSanitizer, UndefinedBehaviorSanitizer, LeakSanitizer, or ThreadSanitizer diagnostic appears in those four logs. Seven runner-default libstdc++ jobs failed the mandatory allocation-failure core test, while their other five groups passed. The overall workflow remains **failure**.

## Evidence and scope

- [summary.json](summary.json) records every job URL, exact compiler version, group results, diagnostic excerpts, and standalone probe result.
- Each `job-<id>.log` is a focused, sanitized excerpt fetched through the GitHub Actions job-log connector. All 11 original logs contained the tested commit SHA. Checkout/authentication details, package-manager noise, and cleanup output are omitted; runner repository and temporary paths are replaced by `<repo>` and `<runner-temp>`.
- Passing CTest logs print six group results, not individual named-case or randomized-request counts.
- The successful libc++ package version recorded in the logs is `1:21.1.8~++20251221032922+2078da43e25a-1~exp1~20251221153059.70`.
- Original jobs used GCC 13.3.0 or Clang 18.1.3. Their installed `libstdc++6` runtime package version was not printed. The sanitizer stacks identify `libstdc++.so.6` Build ID `753c6c8608b61d4e67be8f0c890e03e0aa046b8b`; compiler/header versions do not establish that runtime's package version.

## Sanitizer configuration

The `asan` preset enables `APEX_SANITIZERS=ON`. At the tested commit, `CMakeLists.txt` adds `-fsanitize=address,undefined`, `-fno-omit-frame-pointer`, and `-fno-sanitize-recover=all`; `CMakePresets.json` sets `ASAN_OPTIONS=detect_leaks=1` and `UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1`. The `tsan` preset enables `-fsanitize=thread` and `TSAN_OPTIONS=halt_on_error=1`. The workflow sets `vm.mmap_rnd_bits=28` for TSan. All six groups ran successfully in each successful sanitizer configuration; no tests were excluded or failing tests marked as expected success.

## Independent standard-library probe

Every job separately compiled `tools/repro_pool_failure.cpp`, which uses only the C++ standard library. All seven libstdc++ probes crashed with exit 139 after the third upstream allocation threw `std::bad_alloc`. Their trace is 528-byte allocation, 1016-byte allocation, failed 192-byte allocation, then 1016-byte deallocation before the crash. ASan/TSan core-test stacks locate the same failure in `std::pmr::unsynchronized_pool_resource::do_allocate`, reading `0x0017fffffff0`, through `MatchingEngine::add` at `src/engine.cpp:140` and `allocation_failure_destruction` at `tests/core_tests.cpp:363`.

All four libc++ probes returned success after printing `bad_alloc propagated safely`, with both prior upstream allocations deallocated. These probes were **uninstrumented**, including in sanitizer jobs; the full core test binary was separately built with its selected sanitizer and includes the allocation-failure regression.

Sanitizer success means these test executions reported no sanitizer issue. It does not prove that every possible execution is free of defects.
