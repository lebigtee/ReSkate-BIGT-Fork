# Death Race

Downhill races you make yourself, raced against AI riders, or against other players who run this
build of ReSkate. Everything works on the controller.

## Playing

1. **Make a race.** Radial menu › Throwdowns › **DEATH RACE**. Away from any race, the tile sets a
   new race's start where you stand. Ride to where it should end and press the tile again
   (**SET FINISH HERE**): the race is saved and you are put back on its start. Checkpoints can be
   added from the console (`race checkpoint`).
2. **Open it.** On a race's start, the tile opens it as a throwdown, carried by the game's own
   Skate Jam flow. Its setup page shows only the **AI riders** slider (0 to 4) and Confirm. The
   race's flag goes up: rivals join there. Start the race from the flag.
3. **Race.** The game's intro and 3-2-1 play, then you are put on your board on the start line,
   facing the way, just before GO. Ride through the checkpoints in order to the finish. The HUD
   points to the next flag and shows the clock, then your place at the finish (**YOU WIN!** or
   **YOU LOSE**, *Nth OF M*).
4. **The end.** The throwdown ends once everyone has crossed the line, and the game's own results
   rank everyone by place (100 points a place ahead of last, none for not finishing). The race is
   then over: its flags come down and the tile offers a new one.

During a race a bail puts you back on your board, a moment before where you fell, after about two
seconds; the d-pad's way back to the session marker (the start) is refused.

The pause menu's **CUSTOM STUFF › DEATH RACE** page lists this map's races (select one, go to its
start, delete it, make a new one).

## AI riders

A race keeps a **line**: your own ride of it (the ride that set its finish, then your fastest run
since), kept in memory for the game's session. The riders ride that line's path again, each at a
pace of its own that swells and ebbs, wandering a little either side of it and looking ahead with
the game's collision rays to keep off walls and props. Their bodies never copy what yours did on
the line (tricks, falls): they come from the **riding library**, stretches of your plain riding,
pushing, tucking and airs gathered from all your rides, plus your sketchy landings and emotes.
Ride a course once without tricks and the riders have a body.

They are shown as the game's own skaters (in the last remote-player slots, which a session never
reaches), wearing the costume ReSkate provides, with collision capsules. A light run-in pushes you
apart, the rider stumbling; a hard one floors both of you, and each is put back on the board a
little before.

## Multiplayer

With other players in a multiplayer session, a race is theirs too when they run this build:

- The host's race goes out with its throwdown through the throwdown relay
  (`Extension/Throwdowns/throwdown_relay.cpp`), under its own series (`DeathRace`, its flags in a
  `DeathRaceFlags` offer). ReSkate builds without Death Races ignore both without a word: their
  players see nothing and are not bothered.
- The others see the flags and join at the race's flag; the AI riders step aside as soon as one
  does (in a session, at most 2 riders, so a player joining never fills the queue with them).
- A race takes 5 racers at most. On the start line each has a lane, 1.3 m apart, in the same order
  on every machine (the host in the middle).
- Each machine runs the race on its own copy of the throwdown. Each racer's finish time (from its
  own GO) reaches the others, and every machine ranks everyone the same way. Each copy ends once
  everyone has crossed the line, quit or left.

The log (`logs\ReSkate.log`) follows each step: `is a Death Race`, `counting down`, `lane`,
`crossed the line after ... ms`, `everyone crossed the line`. Ask testers for the host's and a
guest's log.

## Console (`~`)

| Command | Effect |
|---|---|
| `race status` | This map's races and their best times |
| `race new` | Set a new race's start where you stand |
| `race checkpoint` | Add a checkpoint to the new race where you stand |
| `race finish-here` | Set the new race's finish where you stand and save it |
| `race cancel` | Drop the new race |
| `race select <n>` | Select a race: its props stand in the world |
| `race hide` | Put the selected race's props away |
| `race go [n]` | Teleport to a race's start |
| `race delete [n]` | Delete a race |
| `race abort` | Abort the race in progress |
| `race solo on\|off` | Race alone by stopping on a start line, without a throwdown (off by default) |

## Files

| File | What it does |
|---|---|
| `death_race.h` | The public side: races, the HUD's snapshot, the tile, the throwdown events |
| `death_race.cpp` | Game thread: races and their file, the race's state machine, props, scores, the end, races with other players, the `race` commands |
| `death_race_riders.h`, `.cpp` | The AI riders: lines, the riding library, the riders' simulation and poses |
| `death_race_limits.h` | How many race together, and their spacing on the start line |
| `death_race_hud.cpp`, `death_race_view.cpp` | The overlay: flags, arrow, clock, result (overlay library) |
| `death_race_commands.cpp` | Registers the `race` commands |

Outside this folder, the race reaches the game through:

- `Extension/UI/NativeMenu/native_throwdown_tile.cpp`: the DEATH RACE tile, the setup page (the
  AI riders slider, no Customize), the throwdown's timer kept off the HUD.
- `Extension/Throwdowns/native_throwdowns.cpp`: the Skate Jam's text dressed as the race's, the
  riders' names on the leaderboards, the "you left" messages kept from the player at the end.
- `Extension/Throwdowns/throwdown_lab.cpp`: the setup's players, timer and zone (big enough for
  the whole course); `throwdown_relay.cpp`: races with other players.
- `Extension/Skater/no_bail.cpp`: a hard run-in's native bail, and the session marker guard;
  `Extension/Skater/client_noclip.cpp`: a run-in's push.
- `Extension/Profile/local_profile_objects.cpp`: the game's collision rays, and teleports facing a
  heading; `Extension/Objects/network_object_runtime.cpp`: the race's props (the game's build-kit
  start line, slalom flags and finish arch); `Extension/Multiplayer/Remote/remote_collision.cpp`:
  the riders' capsules.

## Data

`%LOCALAPPDATA%\ReSkate\deathrace\`: `races.json` (the races, per map), `riding.bin` (the riding
library), `fall.bin` (your last fall, which a floored rider plays).

## Tuning

Every number is a named constant at the top of `death_race.cpp` and `death_race_riders.cpp`, with
what it does beside it: the riders' paces and speed swings, how far they wander, the run-in
thresholds (`hit_speed`, `crash_speed`), the respawn after a bail (`respawn_after`,
`respawn_back`), the points a place (`points_a_place`).

## Known limits

- Made for the game build ReSkate 1.1.3 supports (`Engine/Game/Build/20260929`): a game update
  means checking the native addresses again.
- Lines live in memory: after a restart, the riders of an older race need a new ride of it.
- Races with other players are not tested with several players yet.
- A racer who never finishes holds the end back until the Skate Jam's own timer (5 minutes,
  hidden) runs out.
- Between real players, collisions are the game's own (the light push and stumble rules are the
  AI riders').
