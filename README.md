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

- `tests/results/ITs/integration_result.txt`
- `tests/results/ITs/<profile>/integration_test.shared.txt`
- `tests/results/ITs/<profile>/integration_test.static.txt`

Coverage outputs, when coverage artifacts are present:

- `tests/results/ITs/ITs_all_coverage.html`
- `tests/results/ITs/ITs_all_coverage.xml`
- `tests/results/ITs/coverage-summary.json`

Coverage note:

- coverage reports are generated only if a coverage-instrumented library build
  exists and the tests were run against it
- `build_libs.sh` can build `release_cov` when `gcov` and `gcovr` are available

## API notes

- single-component helpers accept exactly one component: no `/`, no `.`, no `..`
- walk helpers accept relative paths only
- symlinks are rejected during capability acquisition
- durability stays explicit: create, rename, and unlink helpers do not fsync
  parent directories implicitly
