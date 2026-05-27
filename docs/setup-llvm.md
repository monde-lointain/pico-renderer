# LLVM/Clang + Orthodoxy plugin setup

This project enforces an Orthodox C++ subset on `src/{game,gfx,app}` via the
[Orthodoxy](https://github.com/d-musique/orthodoxy) clang plugin. Enforcement is
**optional locally** (a build with no plugin still works) but **required in CI**
on the Linux/clang job. This doc shows how to install LLVM/Clang dev headers,
build + install the plugin, and verify enforcement is actually live.

## Why a plugin (and why a version match matters)

Orthodoxy is a Clang compiler plugin: it inspects the AST and turns forbidden
C++ features (`auto`, `class`, named casts, exceptions, etc. — see
`.orthodoxy.yml`) into compile errors. Because it links against Clang/LLVM
internals, **the plugin must be built against the SAME major version of
LLVM/Clang you compile the project with.** A clang-22 build needs an
orthodoxy plugin built against LLVM 22.

The plugin's `find_package(orthodoxy)` config locates the installed
`.../<clang-major>/orthodoxy.so` by your compiler's reported major version, so a
mismatch silently yields "plugin NOT FOUND" (enforcement skipped). That is the
exact failure mode `PICOSYSTEM_TEMPLATE_REQUIRE_ORTHODOXY` exists to catch.

## Prerequisites

- **LLVM/Clang dev libraries**, v22 (the plugin's README notes it "tightly
  integrates with a particular version of the Clang compiler"; this project is
  validated against clang-22). You need the dev packages, not just the
  compiler: `llvm-config`, LLVM and libclang (C and C++) dev headers.
- **CMake ≥ 3.31** to *build the plugin* (the orthodoxy `CMakeLists.txt`
  pins `cmake_minimum_required(VERSION 3.31)`). This project itself only
  needs CMake ≥ 3.28.
- **Ninja** (recommended generator).

## 1. Install LLVM/Clang dev headers

### Debian/Ubuntu (apt.llvm.org)

```sh
LLVM_VERSION=22
# Easiest: the official installer script (this is what CI uses).
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh ${LLVM_VERSION}
# Dev libs the plugin links against. Note the inconsistent package naming:
# libclang-${LLVM_VERSION}-dev (hyphen) vs libclang-cpp${LLVM_VERSION}-dev (none).
# libclang-*-dev pulls in llvm-${LLVM_VERSION}-dev (llvm-config + libLLVM.so).
sudo apt-get install -y \
  libclang-${LLVM_VERSION}-dev \
  libclang-cpp${LLVM_VERSION}-dev
```

This gives you `clang-22`, `clang++-22`, and `llvm-config-22`. (Plain
`apt install clang llvm-dev libclang-dev libclang-cpp-dev` also works if your
distro's default LLVM is already 22.)

### macOS (Homebrew)

```sh
brew install llvm        # provides clang + llvm-config under $(brew --prefix llvm)/bin
brew install ninja cmake
```

Add the Homebrew LLVM to your PATH (or pass `ORTHODOXY_LLVM_CONFIG` explicitly).

> Note: CI runs Orthodoxy only on the Linux job. macOS/Windows build with
> AppleClang/MSVC where the plugin is not engaged. You only need the plugin
> locally if you want enforcement on your own machine.

## 2. Build + install the Orthodoxy plugin

```sh
git clone https://github.com/d-musique/orthodoxy.git
cd orthodoxy
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang-22 \
  -DCMAKE_CXX_COMPILER=clang++-22 \
  -DORTHODOXY_LLVM_CONFIG=/usr/bin/llvm-config-22
cmake --build build
sudo cmake --build build --target install     # installs to /usr/local by default
```

- `ORTHODOXY_LLVM_CONFIG` pins which LLVM the plugin builds against; omit it to
  let the build auto-discover `llvm-config` on PATH.
- The install drops `orthodoxy.so` under a clang-major-versioned dir and an
  `orthodoxyConfig.cmake` package config so `find_package(orthodoxy)` finds it.
- If you install to a non-standard prefix, point the project configure at it
  with `-DCMAKE_PREFIX_PATH=/your/prefix`.

## 3. Configure with enforcement live

The `host` preset already pins `clang`/`clang++`. Build it with enforcement
required so configure fails loudly if the plugin is not actually picked up:

```sh
cmake --preset host \
  -DCMAKE_C_COMPILER=clang-22 -DCMAKE_CXX_COMPILER=clang++-22 \
  -DPICOSYSTEM_TEMPLATE_REQUIRE_ORTHODOXY=ON
```

Expected configure output when the plugin is live:

```
-- Found orthodoxy plugin: .../22/orthodoxy.so
-- Orthodoxy plugin found: enforcement enabled on src/{game,gfx,app}
```

If instead you see `Orthodoxy plugin not found` while
`PICOSYSTEM_TEMPLATE_REQUIRE_ORTHODOXY=ON`, configure aborts with a
`FATAL_ERROR` — that is the enforcement-active assertion doing its job. Common
causes: clang major version mismatch, plugin not installed, or wrong
`CMAKE_PREFIX_PATH`.

## 4. Verify enforcement is actually live (the canary)

`PICOSYSTEM_TEMPLATE_REQUIRE_ORTHODOXY=ON` proves the plugin was *found*. The
**canary** proves it actually *rejects* bad code.
`tests/orthodoxy_canary/canary.cc` contains deliberate, unsuppressed violations
(`auto`, `static_cast`, `class`). When the plugin is active, that target is
wired in and exposed as a `WILL_FAIL` ctest:

```sh
ctest --preset host -R picosystem_template_orthodoxy_canary --output-on-failure
```

- **Test PASSES** => the canary failed to compile as required => enforcement is
  live.
- **Test FAILS** (canary compiled clean) => enforcement is silently dead;
  investigate the plugin install / version match before trusting any green
  build.

When the plugin is absent (a normal local build), the canary target is not
created and the test simply does not exist — the rest of the suite still runs.

## What CI does

The Linux job in `.github/workflows/ci.yml` performs steps 1-2 automatically,
then configures with `-DPICOSYSTEM_TEMPLATE_REQUIRE_ORTHODOXY=ON` and runs
`ctest` (which includes the canary). Windows (MSVC) and macOS (AppleClang) build
and test without the plugin, covering portability. This keeps enforcement
honest: a regression that silences the plugin turns the Linux job red.
