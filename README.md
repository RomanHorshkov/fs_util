# fsutil

[![Quality](https://github.com/RomanHorshkov/fs_util/actions/workflows/quality.yml/badge.svg?branch=master)](https://github.com/RomanHorshkov/fs_util/actions/workflows/quality.yml?query=branch%3Amaster)
[![Security](https://github.com/RomanHorshkov/fs_util/actions/workflows/security.yml/badge.svg?branch=master)](https://github.com/RomanHorshkov/fs_util/actions/workflows/security.yml?query=branch%3Amaster)
[![Release](https://github.com/RomanHorshkov/fs_util/actions/workflows/release.yml/badge.svg?branch=master)](https://github.com/RomanHorshkov/fs_util/actions/workflows/release.yml?query=branch%3Amaster)
![Coverage](.github/badges/coverage.svg)
[![License: MIT](https://img.shields.io/badge/license-MIT-informational)](./LICENSE)

Small POSIX/Linux filesystem helper built around directory file descriptors as capabilities.

It avoids string-built absolute paths. Open a trusted directory once, then operate relative to it with `*at()`-style helpers.

## Goals

- dirfd-based traversal
- single-component file/dir operations
- relative path walking
- symlink rejection during acquisition
- explicit metadata verification
- explicit crash-consistency barriers
- predictable fd ownership

## Layout

- `VERSION` - package/library version
- `app/fsutil.h` - public API
- `app/fsutil.c` - implementation
- `utils/` - build, test, coverage, packaging, and formatting scripts
- `tests/UTs/` - cmocka unit tests (contract tests + fault-injection tests through the private syscall seam `tests/UTs/fsutil_test_hooks.h`, the latter compiled only with `-DFS_UTIL_TESTING`)
- `tests/ITs/` - cmocka integration tests, run against every build profile, static and shared
- `tests/results/UTs/` - unit-test coverage outputs (HTML/XML/JSON, 100% line + branch gate)
- `tests/results/ITs/` - integration-test runs and coverage outputs
- `build/` - generated objects, libraries, test binaries, and `.deb` artifacts

## Platform

POSIX.1-2008 / Linux-like systems.

Not a Windows abstraction.

## Return convention

All functions return:

```c
0       // success
-errno  // failure
```

## Build

Builds are driven by scripts under `utils/`.

The repository is standalone:

- every build entrypoint resolves paths from its own script location
- the GCC profile policy is loaded from `utils/gcc_build_profiles.sh`
- no sibling `../compilation` checkout is needed anymore

Build static and shared libraries:

```sh
./utils/build_libs.sh
```

Artifacts:

- `build/<profile>/libfsutil.a`
- `build/<profile>/libfsutil.so`
- optional coverage variant: `build/release_cov/libfsutil.a`
- optional coverage variant: `build/release_cov/libfsutil.so`

## Testing

Both suites use `cmocka`. Scripts under `utils/` follow the house naming shared with uuid7/EMlog/SPSCring/MPSCring:

- `utils/build_UTs_release.sh` compiles `tests/UTs/*.c` at `-O2` and links them against `build/release/libfsutil.a`, the exact object code that gets packaged. The fault-injection tests are compiled out here, so this run proves the shipped library honours every documented contract.
- `utils/build_UTs.sh` compiles `app/fsutil.c` once, instrumented, with `-DFS_UTIL_TESTING` (every kernel call goes through a replaceable function pointer), runs every unit-test executable against it, and gates on **100% line and 100% branch** coverage of `app/fsutil.c`. The fault-injection tests are what reach the cleanup paths a real filesystem never produces on demand: `fchmod()` failing on a file just created, `fstat()` failing on an open fd, `fcntl()` refusing `F_GETFL`, `fsync()` reporting `EIO`, a cleanup `unlinkat()` clobbering `errno`.
- `utils/build_ITs.sh` + `utils/run_ITs.sh` build and run the integration suite for every profile in the catalog (`debug`, `audit`, `sanitize`, `release`, `native`, `extreme`, plus `release_cov`), static and shared, and archive an HTML report under `tests/results/ITs/runs/<run id>/`.
- `utils/build_sanitizer_tests.sh` runs UTs and ITs under ASan, UBSan, and LSan with the seam enabled, so an fd leaked on an injected error path is caught.
- `utils/run_pipeline.sh` runs the whole board: `build`, `unit_release`, `unit_cov`, `build_ITs`, `run_ITs`, `sanitizers`, `package`. Each stage is also invocable on its own.

```sh
./utils/run_pipeline.sh
```

Symlink rejection note: a symlink where a directory is required is refused by `openat(O_DIRECTORY|O_NOFOLLOW)`; current kernels report that as `-ENOTDIR`, older ones as `-ELOOP`. The tests accept both because the property under test is "never followed". File helpers use `O_NOFOLLOW` alone and report `-ELOOP`.

## Packaging

Build the Debian package:

```sh
./utils/build_deb.sh
```

Compatibility wrapper:

Artifact:

- `build/debs/libfsutil_<VERSION>_<ARCH>.deb` — runtime: `/usr/local/lib/libfsutil.so.<VERSION>`, `libfsutil.so.<MAJOR>`
- `build/debs/libfsutil-dev_<VERSION>_<ARCH>.deb` — development: `/usr/local/include/fsutil.h`, `/usr/local/include/utils/fsutil.h`, `/usr/local/lib/libfsutil.a`, `libfsutil.so` linker symlink, `pkgconfig/fsutil.pc`; depends on `libfsutil (= <VERSION>)`

The package includes `postinst` and `postrm` hooks that run `ldconfig`.

## GitHub Pipeline

Four workflows, the same connected graph every sibling library uses:

- `.github/workflows/quality.yml` - `build` → `compiler-portability` (gcc + clang, `-Werror`), `unit-tests-coverage` (release UTs + 100% gate), `integration` (all profiles) → `sanitizers` → `package-smoke` (build the `.deb`, install it, compile and run a program against ONLY the installed package). Runs on every branch push, every tag push, and every pull request; also invocable via `workflow_call`.
- `.github/workflows/security.yml` - CodeQL and GCC's `-fanalyzer` on master/`safety_upgrades` pushes, tags, pull requests, and weekly.
- `.github/workflows/release.yml` - on a `v*.*.*` tag: the full Quality gate, then the release bundle (tarball with header + libraries, the `.deb`, `SHA256SUMS`) published as a GitHub Release. The tag must equal `VERSION`.
- `.github/workflows/coverage-badge.yml` - after a successful Quality run on master, refreshes `.github/badges/coverage.svg` from the unit-test coverage summary.

## API notes

- single-component helpers accept exactly one component: no `/`, no `.`, no `..`
- walk helpers accept relative paths only
- symlinks are rejected during capability acquisition; `fs_dir_open_abs_nofollow()` extends that to every component of an absolute path (start from `/`, walk with `O_NOFOLLOW`, refuse `..`)
- `fs_rename_noreplace_at()` renames without ever replacing an existing destination, in one kernel call (`renameat2` + `RENAME_NOREPLACE`): no probe-then-rename window
- durability stays explicit: create, rename, and unlink helpers do not fsync parent directories implicitly

## Build profiles & hardening

Builds go through `utils/build_libs.sh [profile ...]`, driven by the shared catalog `utils/gcc_build_profiles.sh` (synced verbatim from `Utils/compilation/`, never edited locally); artifacts land in `build/<profile>/`; `utils/check_hardening.sh` gates every release artifact.

| Profile | Optimization | Warnings | Instrumentation | Hardened | Use it for |
|---|---|---|---|---|---|
| debug | `-Og -g3` | core | — | no | day-to-day development |
| audit | `-O1 -g3` | everything + `-fanalyzer` | — | yes | compiler-driven validation |
| sanitize | `-O1 -g3` | strict | ASan+UBSan+LSan | yes minus FORTIFY — conflicts with ASan | runtime bug hunting |
| release | `-O2 -DNDEBUG` | strict | — | yes — full set below | production / the deb payload |
| native | `-O3 -flto -march=native` | strict | — | yes | benchmarks on the deploy box |
| extreme | `-O3 -flto -march=native` | core | — | deliberately none | max-perf experiments only |

Release hardening by stage:

| Flag | Stage | Purpose |
|---|---|---|
| `-fstack-protector-strong` | compile | stack canary on frames with arrays / address-taken locals |
| `-fstack-clash-protection` | compile | page-by-page stack growth — the guard page can't be jumped |
| `-fcf-protection=full` | compile | x86-64 CET: indirect-branch tracking + shadow stack, NOP on older CPUs |
| `-D_FORTIFY_SOURCE=3` | preprocess | checked libc calls with dynamic object sizes |
| `-fPIC` | compile | position-independent code — libraries |
| `-Wl,-z,relro -Wl,-z,now` | link | GOT/PLT read-only after load — full RELRO |
| `-Wl,-z,noexecstack` | link | non-executable stack asserted |
| `-Wl,-z,defs` | link .so | undefined symbols fail the build not the load |
