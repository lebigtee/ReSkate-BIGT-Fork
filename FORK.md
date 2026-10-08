# The Hall of Meat fork of ReSkate

This fork adds three things to [ReSkate](https://github.com/Dingo-Shenanigans/ReSkate) and changes
nothing else:

- **[Hall of Meat](Extension/HallOfMeat/README.md)**: skate 3's bail mini-game. Bail, and see
  which bones you broke on an X-ray, with the slow motion and the jerky picture of the original.
- **[Death Race](Extension/DeathRace/README.md)**: downhill races you make yourself, raced against
  AI riders, or against other players who run this fork.
- **[Camera light](Extension/CameraLight/README.md)**: a filmer's lamp on the camera at night,
  one of the game's own lights.

Everything works on a controller. Each is switched from the pause menu's **CUSTOM STUFF › PLAYER**
(and the Death Race from **Throwdowns**), or from the console (`meat`, `race`, `light`).

## Playing with official ReSkate players

The fork plays with everyone on official ReSkate servers and sessions: its own features are local
to the player (the Hall of Meat, the camera light), or only between players of the fork (a Death
Race offered in a session is carried by ReSkate's throwdown relay under its own series, which
official builds ignore without a word). In a session with real players, AI riders step aside.

## Where the fork's code is

- `Extension/HallOfMeat`, `Extension/DeathRace`, `Extension/CameraLight`: the fork itself. Each
  folder's README lists its files.
- `Engine/Game/Build/20260929/no_bail.h`, `camera_light.h`: what the fork knows of the game's
  native code, beside ReSkate's own.
- ReSkate's own files the fork touches, as little as it can (to keep merging new ReSkate
  versions easy):
  - `Runtime/client_tick.cpp`, `Runtime/runtime_init.cpp`: the fork's per-tick calls and the
    session marker guard;
  - `Extension/UI/Overlay/overlay_render.cpp`: the Hall of Meat's and the Death Race's drawing
    calls (a dozen lines; the GPU work is `Extension/HallOfMeat/hall_of_meat_picture.cpp`);
  - `Extension/UI/NativeMenu/` (`native_throwdown_tile.cpp` is the fork's; `native_menu.cpp`,
    `native_tools_view.h`, `native_menu_rows.cpp`): the PLAYER page's rows and the Death Race
    tile and setup page;
  - `Extension/Throwdowns/`: the throwdown relay's races, the lab's AI riders and setup values;
  - `Extension/Skater/no_bail.cpp`, `client_noclip.cpp`: a hard run-in's native bail, the d-pad
    teleport refused during a race, a run-in's push;
  - `Extension/Profile/local_profile_objects.cpp`, `Extension/Objects/network_object_runtime.cpp`,
    `Extension/Multiplayer/Remote/remote_collision.cpp`, `native_cosmetics.cpp`: rays,
    teleports facing a heading, the race's props, the riders' collision and costume;
  - `Extension/Console/commands.*`, `Extension/Settings/named_settings.*`: the commands, and the
    queue countdown setting;
  - `CMakeLists.txt`, `cmake/Runtime.cmake`, `cmake/UI.cmake`: the fork's files and tests.

## Building and testing

Build as ReSkate builds (see its README). The fork's tests and its game data tool:

```
cmake -S . -B build/tests-x64 -DDINGOSDK_BUILD_FORK_TESTS=ON -DDINGOSDK_BUILD_MULTIPLAYER_TESTS=ON
cmake --build build/tests-x64 --config Release --target dingosdk_death_race_tests dingosdk_throwdown_relay_tests dingosdk_native_tools_view_tests
```

- `dingosdk_death_race_tests`: the races file, a race offered to other players, the start line's
  lanes (without the game).
- `dingosdk_throwdown_relay_tests` (ReSkate's, extended): Death Race races between players, as
  the relay carries them.
- `dingosdk_native_tools_view_tests` (ReSkate's, extended): the PLAYER page's fork rows.
- `dingosdk_hall_of_meat_skeleton_dump <Skate folder> [<word> | --dump <asset>]`: lists the
  game's assets by name, or prints an asset's fields with their offsets.

What the game alone can tell (the AI riders' riding, the X-ray, the light) is tested in play.

## When ReSkate moves to another build of Skate

ReSkate accepts one build of the game (Steam build 25414733, `Engine/Game/Build/supported_build.h`)
and refuses to start on any other, so a game update stops the fork with ReSkate until ReSkate
supports the new build. Merging that ReSkate version into the fork, these are the fork's own
assumptions about the game to check again:

- `Engine/Game/Build/20260929/no_bail.h`: the session marker teleport (its code is checked
  against a fingerprint before it is hooked).
- `Engine/Game/Build/20260929/camera_light.h`: the light rig's layout and its lights' transform
  setter (checked against its code before use).
- `Extension/HallOfMeat/hall_of_meat.cpp`, `hall_of_meat_skeleton.h`: the animation skeleton's
  joints (`dingosdk_hall_of_meat_skeleton_dump` reads them from the game's data again) and the
  contacts' layout (`meat probe` records what the game writes).
- `Extension/DeathRace/death_race.cpp`: the riders' player records (the game's UIPlayerInfo
  layout) and the skater's physics states; `death_race_line.cpp`: the push request flag.
- `Extension/UI/NativeMenu/native_throwdown_tile.cpp`: the Throwdowns screen's widgets.
- The `Extension/Throwdowns` changes: event and setting names the relay and the lab use.

The native functions the fork calls are ReSkate's own (checked by ReSkate) or checked the same
way, and what it reads from memory is read with guarded reads: a layout that moved is meant to
stop the feature and say so in the log, not to crash the game. Play-test each feature after
such a merge all the same.

## Code conventions

The fork follows ReSkate's layout and naming. Its comments say what a thing is or does, often as
a noun phrase naming its subject ("The rider's body at `at`…", "The races file's text…"), and
why where it is not obvious; the numbers worth tuning are named constants with their meaning
beside them (`death_race_internal.h`, `death_race_riders_internal.h`, `camera_light_native.cpp`).

## License

ReSkate is free software under the GNU General Public License v3 (`LICENSE`), and so is this fork:
it is a modified version of ReSkate, changed from October 2026 on (its changes and their dates are
its git history, on the `hall-of-meat` branch). Whoever shares its build shares its source with
it, under the same license.

## Assets

The Hall of Meat's skeleton drawings and its panel, the X-rays of its collage included, were made
for the mod by its author.

## Known limits

- Death Races between real players are tested by the relay's unit tests, not yet with several
  players in a live session: feedback from players of the fork will tell.
