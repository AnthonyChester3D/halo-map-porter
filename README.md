# Halo Map Porter

Halo Map Porter converts Halo: Combat Evolved maps between the **Master Chief Collection (MCC)**, **Halo Custom
Edition** and the **original Xbox**, in any direction. It can also build one map for all three games straight
from your H1A-EK tags, while you're still working on it.

It's a Windows app with nothing to install. Under the hood it uses [Invader](https://github.com/SnowyMouse/invader),
the free, open-source Halo map compiler, and it can download Invader for you.

**Download:** the latest `HaloMapPorter.exe` is on the [Releases](../../releases) page.
**Project page:** https://anthonychester3d.github.io/halo-map-porter.html

## What it handles for you

- **Fixes.** It repairs broken or out-of-range tag values automatically, and recovers scripts that need
  functions the target game lacks.
- **Xbox output.**
  - It swaps the map's copies of stock weapons, vehicles and the player for the Xbox originals.
  - It gives PC-only weapons Xbox stand-ins, or keeps them with PC animations.
  - It converts Gearbox models, HUDs (MCC's layouts and edge anchors too) and sounds, and flattens layered shaders the
    Xbox can't draw.
- **Fitting the Xbox.** It works within the Xbox's limits: the 47 MB map file, the 22 MB of tag memory, and a
  texture memory target (22 / 20 / 18 MB) that the auto-shrinker halves your own textures to meet. Before halving
  anything it packs your bump maps the way Bungie's Xbox maps store theirs (P8, or DXT1 for steep ones: a quarter to an
  eighth of the memory at full size). HUD textures
  made for big screens come down to the resolution the Xbox draws them at (the same size on screen, often a
  quarter of the memory or less), and scope masks are scaled so they frame the Xbox's screen as made.
- **Speed on the Xbox.** It trims the most detailed version of very dense custom models (a 15,000-triangle beam
  cannon) so they look the same with far fewer triangles, or down to 1.5x what Bungie's own models of their kind and
  size have (measured on every stock model: the beam cannon 15,327 > 1,729, in first person 3,258), lists every model
  of the map's own (and Bungie's without LODs) so you can choose per model whether it gets LODs and how many triangles
  each level aims at (before any map is built if you like, each model made again in a moment to see it), and puts those
  choices in the map in seconds ("Update the map": only the models are done again on the conversion's prepared tags,
  then the map is rebuilt), stores animations as keyframes the way Bungie stored the
  Xbox's cinematics (about half the tag memory for first-person animations), gives custom Warthogs (and other stock
  vehicles' variants and wrecks) Bungie's own collision where it fits them, makes collision of custom scenery that's
  too dense for bipeds to walk against again the way Bungie made theirs (a few convex pieces with big flat faces:
  Deathtrap's pilot chair took each biped near it ten times Bungie's densest), makes lower-detail versions (LODs) of the map's custom
  models that have none, keeps them in a library so other maps reuse them, gives levels made without portals clusters and portals at their
  doorways (and can divide up the rooms of levels that have them), makes nearly opaque fog fully opaque so the
  Xbox stops drawing past it, draws the map's own sky in fewer layers so it looks the same (Precipice's 2.8 layers per
  pixel become 1.6, near Bungie's 0.8 to 1.4: still layers on the same dome drawn in one pass, clear parts cut away), gives
  the map's effects that are busier than Bungie's busiest (particles and sprite area alive at once) just enough fewer
  particles to match, rewrites model shaders' multipurpose maps in the Xbox's channel order (the PC keeps the reflection
  mask and team colour mask in other channels, so custom vehicles, weapons and scenery lost their shine on the Xbox),
  draws the map's shaders flagged with MCC's Custom Edition compatibility flags ("custom edition blending", "use
  alternate normal type blending") the way MCC does where the Xbox can,
  turns off dynamic lights that would have the Xbox redraw most of the level (the kind
  that runs it out of memory), and reports what the Xbox has to draw.
- **Level window.** It lists the level's reflecting shaders with what each reflection costs and how much of the
  picture it is, and draws the level on your graphics card from where players stand, as made, as converted, or
  with only what dropping reflections loses lit up, with its fog, portals and rooms. Fly through it, keep every
  reflection (the default) or drop some, set the fog, how small scenery gets before it stops being drawn, how heavy the rain or snow is, whether the sky keeps its layers, and how busy effects may be.
- **Weapons & vehicles window.** It shows what each weapon and vehicle costs in tag memory and file size, with a
  3D preview and its seats, groups the pieces a map's scripts make of a vehicle under it, and lets you replace or
  drop them.
- **Quick to convert again.** The tags taken out of the 3 most recent maps are kept, so the Level window, the
  Weapons & vehicles window and the conversion take a map apart once; re-encoded sounds and textures, the lighter sky and the level's measurement are kept too;
  and reading and editing tags, listing what a tag uses and renaming references happen inside the app (the tags
  come out byte for byte as Invader's tools save them) instead of starting those tools hundreds of times.
- **Map folders and packs** for the Halo Universal and
  [Halo 1 Unlimited](https://github.com/AnthonyChester3D/halo-1-unlimited) XBEs. These come with map cards
  (preview picture, name, description, players, gametypes and genres), an `order.txt`, and series pictures for
  packs.

The full manual, with setup, every option and troubleshooting, is [README.txt](README.txt).

## What you need

- Windows 10 or 11.
- Halo Custom Edition, for Custom Edition output, or Halo MCC, for MCC output.
- For Xbox output, an Xbox tagset such as [haloxbox-revised](https://github.com/Aerocatia/haloxbox-revised), or
  your own dumped Xbox stock maps.
- The MCC mod tools (H1A-EK), only for the "Build from my tags" tab.

## Building

```sh
./build.sh
```

This uses MinGW-w64: the `x86_64-w64-mingw32` cross compiler on Linux or WSL, or MSYS2's MINGW64 shell on
Windows (`CXX=g++ CC=gcc WINDRES=windres ./build.sh`). `cli_test.cpp` is a command-line driver for the
conversion engine, used for testing on Linux.

`tag_layouts.inc` (the tag layouts the app reads directly) and `tag_defs.inc` (every tag kind as Invader's tools
read and write it, for `tagedit.cpp`, which does what invader-edit, invader-dependency and invader-refactor do inside
the app) are generated from [Invader](https://github.com/SnowyMouse/invader)'s tag definitions:
`python3 gen_tag_layouts.py <invader>/src/tag/hek/definition tag_layouts.inc` and
`python3 gen_tag_defs.py <invader>/src/tag/hek/definition tag_defs.inc`.

## License

MIT, see [LICENSE](LICENSE). The bundled stb and bcdec headers keep their own (MIT or public domain) licenses, and
`meshoptimizer/` (the parts of [meshoptimizer](https://github.com/zeux/meshoptimizer) 1.3 that make the LODs) keeps
its MIT license, see [meshoptimizer/LICENSE.md](meshoptimizer/LICENSE.md).
