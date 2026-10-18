# Contributing to Spidy

Spidy is a free fan mod by Ilya Mezerowsky, and help is welcome: fixes,
features, and testing on headsets he doesn't own. This page says how to get a
change into Spidy, and what applies when you run or share your own build.

Talk about code in **#spidy-dev** on [Discord](https://discord.gg/x27ZXDpdNt)
(add it under *Channels & Roles* > *Browse Channels*), or in a GitHub issue.

## Before you write code

For anything larger than a small fix, say what you plan first, in #spidy-dev or
in an issue. Ilya decides what goes into Spidy, and asking first saves you from
building something that won't be merged.

- **Stability comes first right now.** The most welcome changes fix crashes,
  headsets and controllers that don't work, and low frame rates.
- **It may be there already.** Look through the SPIDY VR settings and the
  [changelog](docs/CHANGELOG.md) before you add an option.
- **Changes to how Spidy plays need a yes before the work.** Swinging, combat,
  controls and comfort are design decisions, and not every idea fits. Those
  that do usually arrive as a setting that leaves the default as it is, the way
  flips did.
- **Large reworks of a whole system** can't be reviewed at the moment. Split
  them into steps that each work on their own.

## Pull requests

- **One feature or fix per pull request.** Ten small pull requests can each be
  reviewed, tried and merged; one pull request with ten changes cannot.
- **Start from the latest `main`.** Spidy is released often, some days more
  than once, so rebase before you open the pull request.
- **Write commit subjects for players.** Each one becomes a line in the release
  notes: "Dodge on B", not "fix input bug".
- **Say what it changes for the player**, and how to try it.
- **Say how you tested it:** the tests, the lab, the game on a monitor, or the
  game in a headset (which headset, and how it connects). "Not played in a
  headset" is a fine thing to write; it tells Ilya what is left to check.
- **Leave the version number alone.** The release workflow raises it.
- **Code written with AI tools is welcome;** much of Spidy is. Build it and run
  it yourself first, and be able to say what it changes.

## Build and test

You need Windows, Visual Studio 2022 C++ Build Tools with CMake and Ninja, a
Windows SDK, and Python 3.12.

```powershell
.\tools\bootstrap.ps1 -Observer
.\tools\build.ps1 -Observer
.\build\windows-ninja\spidy_tests.exe
Get-ChildItem tests -Filter *_protocol_tests.py | ForEach-Object { python -B $_.FullName }
```

Both sets of tests must pass; the release build runs them too.
`Launch Spidy VR.cmd` starts VR from your checkout, and `Launch Spidy Lab.cmd`
opens a swinging lab that needs no game.

Worth knowing before you change things:

- **Where things are:** the *Development map* at the end of
  [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md) lists every part of the code.
- **Shared structs carry a version.** The game modules and the Python tools
  pass structs to each other (see `include/spidy/game_swing.hpp`). When you
  change one, change the other side in the same pull request, raise its
  version, and run the Python protocol tests.
- **One game build.** The addresses Spidy uses belong to one Steam build of
  the game.
- **Formatting:** 110 columns, as in `.clang-format`. Format the lines you
  changed, not whole files: reformatting a file buries your change in the diff.

## Running or sharing your own build

Spidy is free software under the [GPL-3.0](LICENSE): you may change it and
share what you made. The license asks two things of a build you share:

- **Publish its source** under the GPL-3.0, with the copyright and license
  notices kept.
- **Say clearly that it is modified.**

Ilya asks three more, so players don't get confused:

- **Give it its own name.** Don't present it as Spidy, as an official version
  or as an update.
- **Support it yourself.** Bugs in a custom build don't go to Spidy's issues
  or #spidy-bugs: nobody can tell whose code caused them.
- **On Spidy's Discord, custom builds live in #spidy-dev.** Talk and clips go
  there, and your code as a GitHub link. No zips or installers: the server's
  downloads come only from the official releases.

A fork drifts fast. Spidy changes almost daily, and what you keep outside it
you re-apply on every release. Whatever you'd like to stay, send as a pull
request. Ilya plans a way to add to Spidy without a fork (add-ons); it isn't
there yet.

## License of your contribution

By sending a pull request you agree that your contribution is licensed under
the GPL-3.0, like the rest of Spidy, and that it is yours to give. Don't add
the game's files or assets, or code from a project whose license doesn't allow
it.
