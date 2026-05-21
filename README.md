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

- `fsutil.h` - public API
- `fsutil.c` - implementation
- `utils/` - build and test scripts
- `tests/ITs/` - cmocka integration tests
- `tests/results/ITs/` - test and coverage outputs
- `build/` - generated objects, libraries, and test binaries

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

1. builds every library profile
2. builds every integration-test executable against the discovered libraries
3. runs the discovered executables
4. archives logs and coverage outputs into a run-specific result directory
5. uploads the whole result tree as a GitHub Actions artifact
6. can publish the HTML result tree to GitHub Pages when enabled

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
./utils/build_libs.sh
./utils/build_ITs.sh
./utils/run_ITs.sh
```

Build test executables:

```sh
./utils/build_ITs.sh
```

Run discovered test executables:

```sh
./utils/run_ITs.sh
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

## GitHub Pipeline

Pipeline file:

- `.github/workflows/integration-pipeline.yml`

The CI job is intentionally simple:

1. checkout
2. install `libcmocka-dev`, `gcovr`, `pkg-config`, and `file`
3. run `./utils/build_libs.sh`
4. run `./utils/build_ITs.sh`
5. run `./utils/run_ITs.sh`
6. upload `tests/results/ITs/` as the `fsutil-integration-results` artifact

Browser story:

- the artifact contains `tests/results/ITs/index.html` as the HTML entrypoint
- the workflow also keeps `tests/results/ITs/latest/index.html` for the latest
  archived run inside that artifact
- if repository variable `ENABLE_RESULTS_PAGES=1` is set and GitHub Pages is
  enabled for Actions, the same HTML result tree is published automatically on
  pushes to the default branch

## API notes

- single-component helpers accept exactly one component: no `/`, no `.`, no `..`
- walk helpers accept relative paths only
- symlinks are rejected during capability acquisition
- durability stays explicit: create, rename, and unlink helpers do not fsync
  parent directories implicitly
