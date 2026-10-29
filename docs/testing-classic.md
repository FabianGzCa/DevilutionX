# Classic regression testing

The root CMake presets keep game builds, regression tests, and sanitizer builds in
separate directories. They use the existing GoogleTest/CTest suite and do not
change gameplay options or the multiplayer player limit.

## Requirements

Install the dependencies in [Building from Source](building.md), Ninja, GoogleTest
(including GoogleMock), and Google Benchmark. The test presets use system packages
for GoogleTest and Benchmark. Lua is built from the revision selected by the project.
Other source dependencies are fetched by CMake as usual, so the first configuration
needs network access unless those sources are already cached locally.

On Arch Linux, the additional test dependencies are:

```sh
sudo pacman -S --needed ninja gtest benchmark
```

## Configure and build

Run these commands from the repository root:

```sh
cmake --preset classic-play
cmake --build --preset classic-play

cmake --preset classic-tests
cmake --build --preset classic-tests

cmake --preset classic-sanitizers
cmake --build --preset classic-sanitizers
```

`classic-play` builds the game in `build/` with debug symbols and tests disabled.
`classic-tests` uses `build/classic-tests/` with the same engine configuration and
tests enabled. `classic-sanitizers` uses a Debug build in
`build/classic-sanitizers/` with AddressSanitizer, leak detection, and
UndefinedBehaviorSanitizer. The sanitizer preset is intended for compilers that
support those sanitizers.
All builds use loose engine assets rather than requiring the optional `smpq` tool.

## Game data and isolated profiles

Place or symlink your own `DIABDAT.MPQ` or `spawn.mpq` next to the test executables in
`build/classic-tests/` and `build/classic-sanitizers/`. Use an absolute symlink target.
Do not commit game archives or personal saves. The existing timedemo fixtures are
copied to each build directory; test runs must not replace the source references.

The test presets select SDL's dummy audio/video drivers. On Linux, XDG data/config
variables select private profile directories inside the respective build directory.
Tests run sequentially, since existing fixtures can write to shared paths within a
build. Do not run two
CTest processes against the same build directory at once.

## Run and inspect results

```sh
ctest --preset classic-tests
ctest --preset classic-sanitizers
```

Use `ctest --preset classic-tests -R 'Timedemo|ClassicMultiplayer'` for a focused run.
CTest logs are in each build's `Testing/Temporary/` directory. Add
`--output-junit /absolute/path/results.xml` to retain machine-readable results
outside the build directory.

Record compiler and dependency versions along with the source revision when
establishing a baseline. Report failed and skipped cases separately: a successful
build or an asset-dependent test skipped for missing game data is not a passing
gameplay regression check. Keep existing failures visible when comparing revisions.
