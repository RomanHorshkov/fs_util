# fsutil

Small POSIX/Linux filesystem helper built around directory file descriptors as
capabilities.

It avoids string-built absolute paths. Open a trusted directory once, then
operate relative to it with `*at()`-style helpers.

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
- `fsutil.h` - public API
- `fsutil.c` - implementation
- `utils/` - build, test, coverage, and packaging scripts
- `tests/unit/` - cmocka unit tests
- `tests/ITs/` - cmocka integration tests
- `tests/results/unit/` - unit-test and coverage outputs
- `tests/results/ITs/` - integration-test and coverage outputs
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

The integration suite uses `cmocka`.

If you want the whole thing automated in CI, the pipeline is the GitHub Actions
workflow:

```text
.github/workflows/integration-pipeline.yml
```

That workflow:

1. runs `build`
2. runs `build_ITs`
3. runs `run_ITs`
4. archives logs and coverage outputs into a run-specific result directory
5. passes the workspace state between jobs with short-lived artifacts
6. uploads the final result tree as a short-lived GitHub Actions artifact

The workflow is intentionally split into stages:

1. build the libraries
2. build the test executables against the already-built libraries
3. run whatever built test executables are present

Requirements on Debian/Ubuntu:

```sh
sudo apt install libcmocka-dev
```

Optional coverage reports:

```sh
sudo apt install gcovr
```

Sequence:

```sh
./utils/run_pipeline.sh
```

Build unit-test executables:

```sh
./utils/build_unit_tests.sh
```

Run discovered unit-test executables:

```sh
./utils/run_unit_tests.sh
```

Run unit tests and emit a gcovr coverage report:

```sh
./utils/run_unit_coverage.sh
```

Build test executables:

```sh
./utils/build_ITs.sh
```

Run discovered test executables:

```sh
./utils/run_ITs.sh
```

Pipeline stages, if you want the CI-style split locally:

```sh
./utils/run_pipeline.sh build
./utils/run_pipeline.sh build_ITs
./utils/run_pipeline.sh run_ITs
```

Outputs:

- latest overview page: `tests/results/ITs/index.html`
- latest run page: `tests/results/ITs/latest/index.html`
- archived run root: `tests/results/ITs/runs/<run-id>/`
- latest text summary: `tests/results/ITs/integration_result.txt`
- per-run text logs: `tests/results/ITs/runs/<run-id>/<profile>/integration_test.shared.txt`
- per-run text logs: `tests/results/ITs/runs/<run-id>/<profile>/integration_test.static.txt`
- per-run HTML logs: `tests/results/ITs/runs/<run-id>/<profile>/integration_test.shared.html`
- per-run HTML logs: `tests/results/ITs/runs/<run-id>/<profile>/integration_test.static.html`

Coverage outputs, when coverage artifacts are present:

- `tests/results/ITs/runs/<run-id>/ITs_all_coverage.html`
- `tests/results/ITs/runs/<run-id>/ITs_all_coverage.xml`
- `tests/results/ITs/runs/<run-id>/coverage-summary.json`

Coverage note:

- coverage reports are generated only if a coverage-instrumented library build
  exists and the tests were run against it
- `build_libs.sh` can build `release_cov` when `gcov` and `gcovr` are available
- the HTML index links to coverage automatically when those reports exist

## Packaging

Build the Debian package:

```sh
./utils/build_deb.sh
```

Compatibility wrapper:

```sh
./utils/make_deb.sh
```

Artifact:

- `build/debs/libfsutil_<VERSION>_<ARCH>.deb`

Installed payload:

- `/usr/local/include/fsutil.h`
- `/usr/local/include/utils/fsutil.h`
- `/usr/local/lib/libfsutil.so.<VERSION>`
- `/usr/local/lib/libfsutil.so.<MAJOR>`
- `/usr/local/lib/libfsutil.so`
- `/usr/local/lib/libfsutil.a`
- `/usr/local/lib/pkgconfig/fsutil.pc`

The package includes `postinst` and `postrm` hooks that run `ldconfig`.

## GitHub Pipeline

Pipeline file:

- `.github/workflows/integration-pipeline.yml`

The CI pipeline is intentionally simple:

1. checkout
2. run `./utils/run_pipeline.sh build`
3. pass `build/` plus the archived run directory to the next job
4. run `./utils/run_pipeline.sh build_ITs`
5. pass the updated workspace state to the final job
6. run `./utils/run_pipeline.sh run_ITs`
7. upload `tests/results/ITs/` as a short-lived results artifact

Browser story:

- the artifact contains `tests/results/ITs/index.html` as the HTML entrypoint
- the workflow also keeps `tests/results/ITs/latest/index.html` for the latest
  archived run inside that artifact
- the GitHub Actions run page gets a Markdown job summary, because the run UI
  can display Markdown summaries but does not render the generated HTML pages
  inline

## API notes

- single-component helpers accept exactly one component: no `/`, no `.`, no `..`
- walk helpers accept relative paths only
- symlinks are rejected during capability acquisition
- durability stays explicit: create, rename, and unlink helpers do not fsync
  parent directories implicitly

## Build profiles & hardening

Builds go through `utils/build_libs.sh [profile ...]`, driven by the shared
catalog `utils/gcc_build_profiles.sh` (synced verbatim from
`Utils/compilation/`, never edited locally); artifacts land in
`build/<profile>/`; `utils/check_hardening.sh` gates every release artifact.

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
