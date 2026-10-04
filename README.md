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
- `utils/` - kyr-build's wrappers and `fs_util_project.sh` (the declarations)
- `tests/UTs/` - cmocka unit tests (contract tests + fault-injection tests through the private syscall seam `tests/UTs/fsutil_test_hooks.h`, the latter compiled only with `-DFS_UTIL_TESTING`)
- `tests/ITs/` - cmocka integration tests, run against every build profile, static and shared
- `tests/results/UTs/` - unit-test coverage outputs (HTML/XML/JSON, 100% line + branch gate)
- `tests/results/ITs/coverage/` - integration-test coverage outputs
- `tests/package/smoke.c` - the installed-package test
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

`utils/` holds the pinned kyr-build's wrappers and `utils/fs_util_project.sh`, which declares what is built, tested and packaged (the steps are kyr-build's, its README "Wrapper contract").

Build from a clean clone: `./utils/deps.sh sysroot && eval "$(./utils/deps.sh env)" && ./utils/run_pipeline.sh` (no sudo; every pin of `deps.txt` lands in `build/sysroot/`). Diagram: `docs/diagrams/tooling_sequence.puml`.

`./utils/build_libs.sh [profile ...]` writes `build/<profile>/libfsutil.a` and `libfsutil.so.<VERSION>` (no argument: every profile).

## Testing

Both suites use `cmocka`. Scripts under `utils/` follow the house naming shared with uuid7/EMlog/SPSCring/MPSCring:

- `utils/build_UTs_release.sh` compiles `tests/UTs/*.c` with the release profile and links them against `build/release/libfsutil.a`, the exact object code that gets packaged. The fault-injection tests are compiled out here, so this run proves the shipped library honours every documented contract.
- `utils/build_UTs.sh` compiles `app/fsutil.c` once, instrumented, with `-DFS_UTIL_TESTING` (every kernel call goes through a replaceable function pointer), runs every unit-test executable against it, and gates on **100% line and 100% branch** coverage of `app/fsutil.c`. The fault-injection tests are what reach the cleanup paths a real filesystem never produces on demand: `fchmod()` failing on a file just created, `fchmodat()` failing on a directory just created, `fstat()` failing on an open fd, `fcntl()` refusing `F_GETFL`, `fsync()` reporting `EIO`, a cleanup `unlinkat()` clobbering `errno`, `open()` interrupted by a signal, `write()` accepting one byte per call or none at all.
- `utils/build_ITs.sh` builds and runs the integration suite against every profile in the catalog (`debug`, `audit`, `sanitize`, `release`, `native`, `extreme`), static and shared, then once at the coverage profile (`tests/results/ITs/coverage/`).
- `utils/build_sanitizer_tests.sh` runs UTs and ITs under ASan, UBSan, and LSan with the seam enabled, so an fd leaked on an injected error path is caught.
- `utils/run_pipeline.sh` runs the whole board: deps and tiers, template, ABI, every profile, release UTs, coverage UTs, ITs, sanitizers, package, and the smoke test against the built debs (`utils/smoke_test_package.sh`, `tests/package/smoke.c`). Each wrapper also runs on its own.

```sh
./utils/run_pipeline.sh
```

Symlink rejection note: a symlink where a directory is required is refused by `openat(O_DIRECTORY|O_NOFOLLOW)`; current kernels report that as `-ENOTDIR`, older ones as `-ELOOP`. The tests accept both because the property under test is "never followed". File helpers use `O_NOFOLLOW` alone and report `-ELOOP`.

## Packaging

Build the Debian packages:

```sh
./utils/build_deb.sh
```

Artifacts:

- `build/debs/libfsutil_<VERSION>_<ARCH>.deb` — runtime: `/usr/local/lib/libfsutil.so.<VERSION>`, `libfsutil.so.<MAJOR>`
- `build/debs/libfsutil-dev_<VERSION>_<ARCH>.deb` — development: `/usr/local/include/fsutil.h`, `/usr/local/include/utils/fsutil.h`, `/usr/local/lib/libfsutil.a`, `libfsutil.so` linker symlink, `pkgconfig/fsutil.pc`; depends on `libfsutil (= <VERSION>)`

The package includes `postinst` and `postrm` hooks that run `ldconfig`.

## GitHub Pipeline

Four workflows, the same connected graph every sibling library uses:

- `.github/workflows/quality.yml` - `build` → `compiler-portability` (gcc + clang, `-Werror`), `unit-tests-coverage` (release UTs + 100% gate), `integration` (all profiles) → `sanitizers` → `package-smoke` (build the `.deb`, install it, compile and run a program against ONLY the installed package). Runs on every branch push, every tag push, and every pull request; also invocable via `workflow_call`.
- `.github/workflows/security.yml` - CodeQL and GCC's `-fanalyzer` on master/`safety_upgrades` pushes, tags, pull requests, and weekly.
- `.github/workflows/release.yml` - on a `v*.*.*` tag: the full Quality gate, then the release bundle (tarball with header + libraries, the `.deb`, `.buildinfo`, `SHA256SUMS`) built twice on two runners, published as a GitHub Release only when every hash agrees. The tag must equal `VERSION`.
- `.github/workflows/coverage-badge.yml` - after a successful Quality run on master, refreshes `.github/badges/coverage.svg` from the unit-test coverage summary.

## API notes

- single-component helpers accept exactly one component: no `/`, no `.`, no `..`, no control characters
- walk helpers accept relative paths only
- symlinks are rejected during capability acquisition; `fs_dir_open_abs_nofollow()` extends that to every component of an absolute path (start from `/`, walk with `O_NOFOLLOW`, refuse `..`)
- `fs_rename_noreplace_at()` renames without ever replacing an existing destination, in one kernel call (`renameat2` + `RENAME_NOREPLACE`): no probe-then-rename window
- durability stays explicit: create, rename, unlink, and rmdir helpers do not fsync parent directories implicitly; `fs_file_fdatasync()` is the cheaper barrier for the commit path
- `fs_file_write_all()` / `fs_file_read_all()` and the positional `fs_file_pwrite_all()` / `fs_file_pread_all()` loop over short transfers and EINTR; a read that hits end of file early is `-ENODATA`, never a short count
- `open()`, `openat()`, `fsync()`, and `fdatasync()` are retried on EINTR, so a signal on a FUSE or network mount never surfaces as `-EINTR`
- composed helpers, each one call: `fs_symlink_at()`, `fs_symlink_replace_at()` (tmp link + rename + parent fsync: the switch a reader sees as old or new, never absent), `fs_write_file_atomic_at()` (tmp + write + fsync + rename + parent fsync; the canonical atomic write), `fs_link_tree_at()` (hard-link mirror of a directory tree; another filesystem is `-EXDEV`)

## Build profiles & hardening

Builds go through `utils/build_libs.sh [profile ...]`, driven by the profile catalog of the pinned `kyr-build` package (`deps.txt`; `utils/gcc_build_profiles.sh` and `utils/check_hardening.sh` are thin wrappers over `build/sysroot/share/kyr-build/`, never copies); artifacts land in `build/<profile>/`; `utils/check_hardening.sh` gates every release artifact.

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

ABI: `abi/layout.txt` is the committed public ABI of `app/fsutil.h` (layouts, enumerator values, callbacks, function signatures, numeric constants), checked by kyr-build's gate through `utils/check_abi.sh` (pipeline stages `abi` and `abi-exports`): a changed line needs a raised major, a new public name a raised minor, and `libfsutil.so` exports exactly the declared functions as `<name>@@FSUTIL_<major>` (`utils/fsutil.map`); `utils/fs_util_project.sh` says what is public. Debs through kyr-build's `lib/deb.sh` (`X-Kyr-Tier`, byte-reproducible). Diagram: `docs/diagrams/abi_exports.puml`.
