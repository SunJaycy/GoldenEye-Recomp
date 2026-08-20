# THIS IS A WORK IN PROGRESS

# GoldenEye 007 PC Recompilation

A native PC recompilation of the Xbox 360 version of GoldenEye 007.

## A quick note

I've been getting a lot of hate over this project, so I want to address it once and move on.

This started because I love GoldenEye and wanted to see it running natively on PC. That's it. It's a hobby project that I've spent a lot of my free time working on.

Yes, AI was used during development. I'm not hiding that. AI can help write code, explain things, and speed up development, but it doesn't magically build a project on its own. Every bug still has to be tracked down, every feature still has to be implemented, and every broken build still has to be fixed by a human being.

If AI-assisted development isn't your thing, that's completely fine. But calling the project worthless because AI was involved misses the point. The goal has always been to get a game I care about running properly on PC and share that work with people who are interested.

Anyway, enough of that.

## Installation

1. Create a folder called `assets` next to the executable.
2. Put the game files inside the `assets` folder.
3. Run `ge.exe`.

## Features

* Native PC executable
* Keyboard and mouse support
* Online multiplayer
* Graphics settings
* Post-processing effects
* Higher FPS gameplay

## Online

To play online, someone needs to run a server.

1. Open `ESC -> ONLINE`
2. Enter your username, server address, and port
3. Enable online play
4. Save and restart
5. Host or join a match

## Building

Builds on Windows and Linux from the same sources. Requirements:

* ReXGlue SDK 0.8.0.0 (built and installed, or a source tree)
* CMake 3.25 or newer
* Ninja
* Clang / LLVM -- the CMake presets use clang on both platforms, not MSVC
* Python 3
* Your own game files

Per platform:

* **Windows** -- install LLVM for `clang++` and `llvm-rc` (the icon resource
  is compiled with `llvm-rc`, since MSVC's `rc.exe` may not be on PATH), plus
  Visual Studio or the Build Tools for the Windows SDK headers and libraries
  that clang links against.
* **Linux** -- install clang and the X11 and GTK 3 development packages.
  The Linux input path in `src/ge_hooks.cpp` drives mouse-look through Xlib,
  and the SDK's window is GTK with the GDK backend pinned to x11.

### 1. Provide the game files

Put your own copy of the game in an `assets` folder at the root of the
repository. `assets/default.xex` has to be there before anything else works.
Nothing in `assets/` is ever committed.

### 2. Generate the recompiled code

    rexglue codegen ge_manifest.toml

This reads `ge_manifest.toml` plus `ge_config.toml` and writes the recompiled
C++ into `generated/`. That directory is derived from your own copy of the
game, so it is not committed -- regenerate it locally. CMake will not
configure until it exists.

### 3. Configure

    cmake --preset win-amd64-release      # Windows
    cmake --preset linux-amd64-release    # Linux

Presets are named `<platform>-<arch>-<config>`, so `win-arm64-release` and
`linux-arm64-release` are available too, along with the matching `debug` and
`relwithdebinfo` variants.

CMake locates the SDK through `find_package(rexglue)`. If it is not found,
either point `CMAKE_PREFIX_PATH` at the SDK install prefix, or set
`REXSDK_DIR` to a rexglue-sdk source tree:

    cmake --preset linux-amd64-release -DCMAKE_PREFIX_PATH=/path/to/rexglue-install

### 4. Build

    cmake --build --preset win-amd64-release      # Windows
    cmake --build --preset linux-amd64-release    # Linux

The executable lands in `out/build/<preset>/` -- `GoldenEye.exe` on Windows,
`GoldenEye` on Linux. The SDK's runtime libraries are staged next to it
automatically on both platforms, so it runs in place.

### 5. Run

The game looks for `assets` next to the executable, so copy or link it into
the build directory before starting:

    # Windows (Developer Command Prompt, as administrator for mklink)
    mklink /D out\build\win-amd64-release\assets %CD%\assets
    out\build\win-amd64-release\GoldenEye.exe

    # Linux
    ln -s "$PWD/assets" out/build/linux-amd64-release/assets
    ./out/build/linux-amd64-release/GoldenEye

Only step 4 needs re-running while working on `src/`. Codegen only has to run
again when `ge_manifest.toml` or `ge_config.toml` changes.

## Known Issues

* AMD GPUs may crash or fail to boot the game.
* AMD compatibility is currently being worked on.

Please report any issues you find.


## Legal

This repository does not contain any game assets, game code, ROMs, XEX files, textures, audio, or other copyrighted material.

You must provide your own game files.

## License

Released under The Unlicense.
