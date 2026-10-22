# Predictive Assist (Geode mod, GD 2.2081)

Bot-assisted play for Geometry Dash. Runs made with this are not legitimate
completions; the on-screen "Bot-assisted" label is on by default.

## What it does

**Predictive delay (all gamemodes).** When you click or release, the mod
copies your player onto a hidden copy and steps it forward with the game's own
collision and movement code. If doing the input now dies within the look-ahead
but holding it back 1 to N physics steps survives, the input waits for the
first safe step. It only ever delays your input; it never clicks for you.

**Early-click buffer (cube, ball, robot, spider).** A tap in the air that you
release before landing is replayed when you touch the ground or an orb within
the window, with your original tap length.

## Limits of v0.1

- The simulation stops at orbs, pads, gravity/mode/dual/mirror/teleport
  portals, and treats those spots as "unknown", which lets the input through
  unchanged. Speed and size portals are simulated.
- The world is frozen during simulation: moving objects and move triggers do
  not advance, so "safe" can be wrong next to moving hazards.
- The simulation assumes you keep holding (or keep released) for the whole
  look-ahead. That fits cube jumps well and wave/ship poorly, which is why the
  fly-mode look-ahead is shorter.
- Delayed inputs go through the game's normal input path one step at a time,
  so Click Between Frames' sub-step precision is lost on the inputs that get
  delayed. Inputs that are not delayed are untouched.
- Noclip from a mod menu may hide deaths from the simulation (depends on hook
  order). Turn noclip off when using this.
- Windows only.

## Settings

Open from the pause menu (**Assist** button) or the Geode mod list.

| Setting | Default | Notes |
|---|---|---|
| Max delay | 3 steps | ~12 ms at 240 TPS |
| Look-ahead ground | 30 steps | cube, ball, robot, spider |
| Look-ahead fly | 12 steps | ship, UFO, wave, swing |
| Also delay releases | on | ship/wave/robot |
| Early window | 4 steps | orbs and landings |
| Show predicted path | off | turn on while testing |
| Debug logging | off | turn on while testing |

## Building

Push this folder to a GitHub repo. The workflow in `.github/workflows/build.yml`
builds it on GitHub Actions (pinned to Geode SDK 5.11.2 bindings) and uploads
`anthonyvp.predictive-assist.geode` as the **Build Output** artifact. Drop that
file into `Geometry Dash/geode/mods` or use Geode's "Install from file".

## Test checklist (first in-game session)

Turn on **Debug logging** and **Show predicted path**, open the Geode console
(Geode menu → Developer → Open console, or the log file).

1. **Step rate.** The log prints "N steps per second" every 2 s while playing.
   It should match your TPS (240 by default). If it is double, every window
   in the settings is half as long as labelled; report the number.
2. **Path accuracy (cube).** Play the first cube part of an easy level. Each
   click draws a path. It should follow where the cube actually goes. Note any
   place where it diverges.
3. **Delay triggers.** Click a jump a little early into a spike. The label
   should count "delayed" and the log should say "delayed +N steps".
4. **Orbs.** Tap slightly early before a yellow orb and release before touching
   it. The label should count "buffered" and the jump should happen.
5. **Robot.** Same early tap before a robot landing. Jump height should match
   the tap length, not always minimum.
6. **Side effects.** Watch for orbs flashing or triggers firing early, coins
   being collected, sounds playing at the wrong time, or trail spikes. Any of
   these means a simulation side effect is leaking.
7. **Dual.** A one-player dual section: both icons should be handled.
8. **Performance.** Spam clicks in wave. Note any stutter.

Send back: the steps-per-second number, which checks failed, and the log lines
around a failure.

## Roadmap

- **v0.2: simulate more objects.** Gravity portals, mode portals, pads and
  orbs on the hidden copy, once v0.1 shows the base simulation matches.
- **v0.3: moving objects.** Advance move triggers during simulation, or detect
  moving hazards near the player and skip assisting there.
- **v1.0: rollback (late clicks).** Keep a ring buffer of full level snapshots
  (practice-mode checkpoints: player, object states, move/rotate triggers,
  sequence triggers) for the last N steps. When a click arrives, try applying
  it 0..N steps in the past on the snapshot, re-simulate to now plus the
  look-ahead, and keep the smallest rewind that survives. Needs: per-step
  snapshot cost measured on a trigger-heavy level first; snapping the visible
  player to the rewound timeline; audio left as is.
