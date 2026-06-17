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

If you want to build from source you'll need:

* ReXGlue SDK
* CMake
* Visual Studio / MSVC
* Python 3
* Your own game files

## Known Issues

* AMD GPUs may crash or fail to boot the game.
* AMD compatibility is currently being worked on.

Please report any issues you find.


## Legal

This repository does not contain any game assets, game code, ROMs, XEX files, textures, audio, or other copyrighted material.

You must provide your own game files.

## License

Released under The Unlicense.
