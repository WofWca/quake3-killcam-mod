CoD-like killcam, finally in Quake.

Fully client-side, no server or engine support needed.

This started as a vibe-coded proof-of-concept, pending a proper rewrite (possibly indefinitely).

Based on [baseq3a mod](https://github.com/ec-/baseq3a).

## Usage

Install like any other Quake III Arena mod. Copy the `.pk3` file to your `baseq3` folder.
Alternatively, copy to a folder alongside `baseq3`, then select the mod in the "mods" in-game menu.

### New CVARs

Open the console, type `cg_killcam` and press Tab to see all of them.

## How it works

It keeps a few seconds worth of snapshots (server packets). When you die, it starts using the recorded snapshots as the input for the client-side logic, instead of the actual current snapshots.
But it also keeps running the original game logic in the background, without passing its state to `CG_DrawActiveFrame`. This is to ensure that switching to the killcam context doesn't affect the real game.

The code is actually quite concise, the initial implementation (first few commits) is ~500 lines of code.

## Similar projects

- Thanks to @zturtleman for pioneering the split-screen mode in the [Spearmint mod](https://github.com/clover-moe/spearmint) (or [mint-arena](https://github.com/clover-moe/mint-arena) rather?), which showed how things like this can be implemeted.
- [wolfcamql: quakelive/quake3 demo player](https://github.com/brugal/wolfcamql): see `cg_autoChaseMissile`.
