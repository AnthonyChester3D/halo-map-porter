HALO MAP PORTER
===============

Converts Halo: Combat Evolved maps between the Master Chief Collection (MCC),
Halo Custom Edition and the original Xbox, in any direction. It can also
build one map for all three games straight from your H1A-EK tags while
you're still working on it.

Under the hood it uses Invader, a free, open-source Halo map compiler.


WHAT YOU NEED
-------------
- Windows 10 or 11
- Halo Custom Edition installed (for Custom Edition output)
- Halo MCC installed (for MCC output)
- For Xbox output: an Xbox tagset such as haloxbox-revised
  (https://github.com/Aerocatia/haloxbox-revised), or your own dumped
  Xbox stock maps
- The MCC mod tools (H1A-EK), only if you use the "Build from my tags" tab
- Invader (the app can download it for you, see below)


FIRST-TIME SETUP
----------------
1. Run HaloMapPorter.exe. There's nothing to install. The first time, it
   opens on the Settings tab.

2. Fill in the folders (use the Browse buttons):

   Custom Edition folder
       The folder that has haloce.exe and the "maps" folder.
       Example: D:\Games\HaloCustomEdition

   MCC maps folder
       MCC's Halo CE maps folder. On a default Steam install:
       C:\Program Files (x86)\Steam\steamapps\common\
           Halo The Master Chief Collection\halo1\maps
       Some MCC mods ship their own bitmaps.map / sounds.map (a workshop
       item keeps them in its halo1\maps folder, next to its maps folder).
       When you convert a map from inside the mod's folder, the app finds
       and uses them; anything the mod doesn't ship comes from this folder.
       Use halo1\maps itself, not the custom_edition folder inside it:
       custom_edition has Custom Edition's bitmaps.map and sounds.map (MCC
       uses them for classic Custom Edition maps), while MCC maps point into
       MCC's own bitmaps.map, and with the wrong one their textures come out
       scrambled. If you pick custom_edition, the app uses the folder above
       it for MCC maps. For Custom Edition maps, custom_edition stands in
       for the Custom Edition folder if that isn't set.

   H1A-EK folder (optional)
       Your MCC mod tools folder (the one with "tags" and "data" inside).
       Only needed for converting a Custom Edition map to MCC, so that
       stock content uses MCC's own versions.

   Xbox tags folder  (needed for Xbox output)
       An Xbox tagset, ideally haloxbox-revised. Point this at its tags
       folder (the one with "globals", "vehicles", "ui" inside).
       Example: D:\SteamLibrary\steamapps\common\HCEEK\tags_xbox
       The revised tagset is recommended because it already fixes several
       broken stock tags.

   Xbox maps (if no tags)
       Only needed if you don't have an Xbox tagset: a folder with your own
       dumped Xbox stock maps (bloodgulch.map etc.). The app pulls the stock
       tags out of them once and caches them. Leave this empty if the Xbox
       tags folder is set.

   Invader folder
       Click "Download Invader". The app gets the latest Windows build and
       fills this in automatically. The status next to the button should
       read "Invader found - you're all set".

       If the download fails: go to
       https://github.com/SnowyMouse/invader/releases, download the Windows
       .zip, unzip it anywhere, then use Browse to pick the folder that
       contains invader-build.exe.

   LOD library folder
       Where the lower-detail versions of models the app makes for Xbox
       maps are kept (see "Levels of detail" below). Filled in for you;
       change it to share one library between PCs (a synced folder).

3. Your settings are saved automatically.


CONVERTING A FINISHED MAP  (tab: "Convert a map")
-------------------------------------------------
Use this for a map you already have as a .map file.

1. Map to convert: click Browse and pick the .map file.
   The app reads the file and sets "From game" for you
   (it shows "Detected: MCC map ..." or similar).

2. To game: pick where it's going (MCC, Halo Custom Edition or Xbox).

3. Save as: filled in with the target game's maps folder. Change it with
   Browse if you want it somewhere else.

4. Leave "Use Custom Edition's shared bitmaps / sounds / loc maps" ticked.
   The map then reads stock textures and sounds from Custom Edition's
   bitmaps.map, sounds.map and loc.map instead of carrying its own copies.
   (Spiral: about 21 MB instead of 66 MB.) Untick it only if you want a
   map that doesn't depend on those files.

5. Click Convert. Progress shows in the log box. When it's done, use
   "Open output folder" or "Open report".

The first conversion to Custom Edition takes about a minute longer than
later ones, because the app reads the stock tags from your Custom Edition
maps once and caches them.


WORKING ON A MAP FOR BOTH GAMES  (tab: "Build from my tags")
-----------------------------------------------------------
Use this while you're still editing a map in the H1A-EK tools. It builds
straight from your tags, so you never have to convert a finished map.

One-time setup:
1. Scenario tag: Browse to your map's .scenario file inside the H1A-EK
   tags folder, for example:
       ...\H1A-EK\tags\levels\spiral\spiral.scenario
2. Tick the games you want (MCC, Custom Ed., Xbox), and pick each one's
   output folder (normally each game's maps folder; for Xbox, any folder
   you copy maps to your Xbox from, plus the map it replaces under
   "named:" - see "Replacing a stock Xbox map" - or, with the Halo
   Universal XBE, a map card - see "Map folders and map cards").

Every time you change something:
3. Save your work in Guerilla/Sapien, then click Build.
   Each ticked game gets a fresh <mapname>.map in its folder.
4. Launch the game and test.

Good to know:
- Your tags folder is never modified. Automatic fixes are made to a
  temporary copy.
- Scripts are compiled from your H1A-EK "data" folder
  (data\levels\<map>\scripts\*.hsc), so script changes are picked up too.
- For Custom Edition, stock tags (vehicles, weapons, HUD...) use the
  original Custom Edition versions. The exception is any stock tag you
  have edited yourself (spotted by its newer date). Your version is kept,
  and the log says "[keep]".
- Close the game, or at least leave the map, before rebuilding. Windows
  won't let the app replace a map that the game has open.
- Xbox builds only copy the tags your map actually uses to a scratch
  folder before converting them, so they stay quick even with a large
  H1A-EK tags folder.
- Prefer MCC's official tools for your MCC build? Untick MCC and use this
  tab only for the Custom Edition build.


XBOX OUTPUT: WHAT THE APP DOES FOR YOU
--------------------------------------
Xbox maps can't share bitmaps.map/sounds.map the way Custom Edition maps
do. Instead, stock content (vehicles, weapons, HUD, stock sounds) comes from
the Xbox tagset in the format the Xbox already uses, and only your own
content is converted:

- Models: Gearbox models (.gbxmodel) become Xbox models (.model).
- Shaders: PC-only "extended" shaders become their Xbox equivalent.
- HUD: PC "high resolution" HUD scaling is converted so elements keep the
  same on-screen size. PC meter bitmaps (heat, fuel, shield bars...) store
  the fill gradient in a different channel than the Xbox reads, so each PC
  meter gets a converted copy of its bitmap (*_meter); without it the meter
  never fills or drains.
- Sounds: any custom sound stored as Ogg Vorbis (which the Xbox can't play)
  is re-encoded to Xbox ADPCM. In "Build from my tags" the original WAVs
  from your data folder are used when they're there. Sounds are also split
  into 227.5 KiB pieces like stock sounds: the original Xbox crashes when it
  plays a longer unsplit sound (MCC doesn't split them). Dialogue (unit and
  scripted dialog, as in campaign maps) can't be split, so it's re-encoded
  whole; any line still longer than 227.5 KiB is listed in a note. Sounds that
  are already short-enough Xbox ADPCM (most MCC sounds) are left alone.
- Scripts: converted automatically (see "Scripts" below).
- MCC textures: textures stored in MCC's BC7 format (which the Xbox and
  Custom Edition can't use) are re-encoded as DXT. Each 4x4 block is
  converted on its own, so they stay the same size or get smaller.
- Tagset fixes: known mistakes in the Xbox tagset are corrected in the map
  (your tagset folder isn't changed). So far: in haloxbox-revised the pause
  menu box is 8 pixels too narrow, which leaves a gap before its right
  border.

Size limit: Xbox multiplayer maps can be at most 47 MB uncompressed, and
stock multiplayer content alone uses about 42 MB. Spiral comes in at about
43 MB. A bigger map hangs when the match starts. If a map is too big, the
app says so and by how much. "Shrink textures to fit" (Xbox tab, on by
default) fixes that: it halves the map's own textures, biggest first, just
until the map fits. Stock textures are never touched. Halved textures look
softer up close. (New Mombasa: about 64 MB -> just under 47 MB.)

Texture memory target (Xbox tab, the list beside "Shrink textures to fit"):
"22 MB - Max", "20 MB - Recommended" (the default) or "18 MB - Safe". After
a build the app adds up the memory all the map's textures take (stock
weapons, vehicles and HUD included; stock maps take 20-23 MB) and compares
it with the target plus the tag memory the map leaves free. OpenCE (test
build 24 and later) lends that free tag memory to its texture cache: the
22 MB of tag memory holds the map's tag data and its level geometry (BSP),
and what's between them goes to textures for that map (Blood Gulch leaves
13.7 MB free, a big port with a heavy BSP 1 MB or less). If the textures
come out over, "Shrink textures to fit" halves the map's own textures,
biggest first, and builds again until they fit; with it off, the build
warns. The log and the report show the figures, e.g.:
  Xbox memory: tag memory 10.1 MB of 22 (tag data 7.6 MB, BSP 2.5 MB);
  textures 22.2 MB; target 20 MB + 11.9 MB of free tag memory (OpenCE
  lends it) = 31.9 MB
Lower targets leave more room for what you see at once (effects, other
players' weapons), at the cost of softer textures on maps that are over.
On Halo Universal (no lending), "18 MB - Safe" is the closer match.

Texture sizes (Xbox tab): the Xbox loads textures as things come into
view. A map that needs much more texture memory than stock maps stutters
(or briefly loses HUD graphics) wherever you can see most of it at once.
"Max texture size" handles the map's own textures (HUD and menu art are
left alone):
- "Stock-like budget (smoothest)", the default: level and sky textures at
  most 512 px, and the level's textures together kept to about 6 MB, like
  stock outdoor maps (Blood Gulch 3.3 MB, Sidewinder 5.3 MB). If the level
  needs more, bump and detail maps are halved first (not below 128 px),
  then the biggest color textures. Props, vehicles and effects at most
  256 px.
- "512 max (sharper)": everything at most 512 px, no budget.
- "1024" / "No limit": for testing.
Uncompressed textures are also compressed to DXT (a quarter of the memory
or less). "Lightmaps" scales the map's lightmaps (its baked lighting);
"Max 256" is like stock maps and the default. 512 looks sharper but costs
about three times the memory, which big outdoor maps may not have to spare.

Other Xbox fixes (automatic):
- Layered transparent shaders: shaders that stack extra layers on top of
  each other (MCC-era holograms, for example) hang the Xbox as soon as
  they come into view, so they're flattened to their base layer. They
  look simpler but work.
- Looping effects on props (fires, smoke, sparks, burning wrecks): PC and
  MCC maps often use far more particles than the Xbox can draw smoothly.
  Each one keeps a quarter of its particles, and very fast-repeating ones
  repeat a third as often. Stock effects are left alone.

Flashlight: on Xbox, the flashlight stops working when too many vehicles
spawn in a match, on stock maps too. In testing, up to 6 Warthogs or 2
Ghosts were fine, 8 Warthogs or 4 Ghosts were not (Ghosts count about
double); 2 Scorpions or 2 Banshees were fine too. Players don't count.
Blood Gulch with its usual vehicles has no flashlight; with
Warthog-only, Scorpion-only or no-vehicle gametypes it does. PC and MCC
don't have this limit. The app leaves a map's vehicles as you placed them,
so gametypes decide whether the flashlight works.

Big maps: if your Xbox is modded to load bigger maps, set "Size limit" on
the Xbox tab to "Big maps (115 MB)". Maps over 47 MB won't load on an
unmodded Xbox. The app's report still compares the size to 47 MB; that's
expected. Separately, every Xbox map has 22 MB of memory for its level
geometry and tag data, and that limit stays the same.

Xbox cache: the Xbox copies each map to its hard-drive cache and, on a
stock XBE, keeps using that copy as long as one with the same map name is
there. So after copying over a new build, the old one plays until the
cache is cleared, either from your dashboard or automatically by an XBE
that clears it at launch. The app also gives every Xbox build its own
header checksum (from the map's contents), so tools that compare map
headers can tell builds apart.

SCRIPTS
Each game has a slightly different set of script functions. MCC added
many (sv_say, sleep_forever, pin, script parameters, ...), and the Xbox
lacks the PC server commands. When a map is converted, or built for a
game, the app rewrites the map's scripts for that game:
  - If the game has an equivalent, it's used. For example
    player_effect_set_max_vibrate becomes player_effect_set_max_rumble on
    Xbox, pin becomes (min (max ...)), abs_real becomes (max x (* -1 x)),
    sleep_forever becomes (sleep -1), local_players becomes players.
  - MCC scripts with parameters are rewritten to pass them through globals.
    Their result is given a definite type, and so is a (begin ...), (if ...)
    or (cond ...) used as the first value of a comparison such as (= ...):
    left untyped, a comparison crashes the game as soon as it runs (it made
    Deathtrap crash on the Xbox right as the match started). If the built
    map still has an untyped script expression, the report warns about it.
  - MCC scripts can use "none" as a team (no team), which the Xbox and
    Custom Edition can't read: it becomes unused9, a team nothing is on, so
    allying with it or comparing with it still does nothing.
  - Nav points: a map can add its own nav point arrows to its HUD globals
    (Coldsnap's landing pads use "orange_landingpad" and
    "green_landingpad"). On the Xbox the Xbox's own HUD globals is used,
    so the map's extra arrows are added after the Xbox's, and their
    pictures are added to a copy of the Xbox's arrow sheet. If an arrow
    still can't be added (the HUD has room for 16), the scripts that use
    it show the plain arrow ("default") instead.
  - Anything else becomes a stand-in that does nothing: calls with no
    result (like sv_say, which posts a chat message on PC servers) are
    removed, true/false checks return false, numbers return 0, and
    distance checks return "far away".
The log lists every change as [scripts] lines with how many times it
happened, so you can check that nothing important was lost.

XBOX TAB: VEHICLE SLOTS AND PC WEAPONS
All of this was tested on original Xbox hardware and the Halo 1 decomp.

Vehicle slots
  The Xbox only spawns three multiplayer vehicle types. They are three
  "slots", and the gametype's vehicle options (Warthog / Ghost / Scorpion)
  pick them by slot. You choose what goes in each slot:
      "Warthog" slot:  Xbox warthog (default), PC warthog, rocket warthog...
      "Ghost" slot:    Ghost (default), Banshee...
      "Scorpion" slot: Scorpion (default), Banshee...
  For example, putting the Banshee in the "Scorpion" slot makes Banshees
  spawn where the map places them, and a "Scorpion only" gametype then
  spawns Banshees. The menu still says "Scorpion".
  Placed vehicles go to the slot holding the same vehicle (a map's custom
  Banshee goes to the Banshee slot). If there isn't one, they go to a slot
  of the same family (warthogs; Ghost/Banshee), or are removed (e.g. the
  Covenant turret, or scorpions when the scorpion slot holds something
  else). Custom vehicles are recognized by name (banshee, ghost, warthog,
  rocket warthog, scorpion/tank, turret/shade).
  If the slotted vehicle isn't in the map's tags: with "Use the Xbox's own
  weapons, vehicles..." ticked, the app takes the stock PC one from Custom
  Edition's maps (Xbox multiplayer has no Banshee; parts the Xbox already
  has, like the Banshee's effects, use the Xbox's copies). Otherwise it
  uses the map's own vehicle of that type, or pulls it from your tags.
  The Covenant turret in a slot is untested.

Keep every vehicle (Halo Universal XBE)
  The Halo Universal XBE spawns every vehicle a map places, not just the
  three slot vehicles, when the gametype's Vehicles option is All.
  With "Map names" on "Map folders", this box (ticked by default) keeps
  the vehicles that have no slot of their own type, like Banshees, rocket
  warthogs, turrets and custom vehicles, instead of swapping or removing
  them. A vehicle of exactly a slot's type still becomes the slotted one
  (a PC warthog becomes the slotted warthog), so Warthogs / Ghosts /
  Scorpions-only gametypes still use its placements; those gametypes
  spawn only their slot's vehicle. A stock XBE leaves the extra vehicles
  out, and every console in a system link game needs the Halo Universal
  XBE.
  Each extra vehicle type uses some of the 22 MB of tag memory. If the map
  doesn't fit with them, the app swaps or removes them as it would for a
  stock XBE and says so in the log. More vehicles also make the Xbox stop
  the flashlight sooner (see Flashlight above).

Levels of detail (LODs)
  A model can have up to five versions, from "super high" detail down to
  "super low", and switch distances that say when each is used: a version
  is drawn once the model is at least that many pixels across the screen,
  and below the "super low" one the model isn't drawn at all. With every
  switch distance 0 (common in custom maps), the engine always draws the
  most detailed version, however far away the model is.
  Some models use their levels for something else: a rock with five shapes
  whose lower levels all point at the first shape. With switch distances,
  every far-away rock would turn into that one, so the app only counts a
  level as a lower-detail version when it has a geometry of its own (a
  permutation the engine picks itself, like a tire's "~blur", may still
  fall back to another's), and leaves models like the rock alone.
  The app gives the map's own models that have lower-detail versions but
  no switch distances ones like Bungie's (vehicles 200/100/50/20/0,
  characters 280/200/80/30/0, scenery 300/90/30/5/0, weapons and items
  100/50/25/10/0 pixels; super low stays 0, so nothing disappears). The
  log lists them as [lod] lines. First-person models are left alone: the
  engine asks them for their most detailed version by drawing them as 0
  pixels across, so switch distances would make them use their least
  detailed one.
  "Also use the lower-detail versions of Bungie's own trees, rocks and
  plants" (Xbox tab, off by default) does the same for stock models: 36 of
  the Xbox's models ship with lower-detail versions and no switch
  distances (also the multiplayer Scorpion). Faster on maps full of
  scenery, but those models then switch detail where they never did on
  retail.

  Making lower-detail versions
  Many custom models have only one version, so the Xbox draws all of
  their triangles however far away they are. "Make lower-detail versions
  (LODs) of the map's models that have none" (Xbox tab, on by default)
  makes them for the map's own models with 1,000 triangles or more: a
  medium version with about half the triangles (used from the "high"
  switch distance down) and a low one with about a fifth (from "medium"
  down). It takes out the vertices that change the shape least and keeps
  every other vertex exactly as it was (position, texture coordinates,
  normal, bones), the edges where one shader's part meets another (no
  cracks), and the edges between pieces moved by different bones (a wheel
  stays separate from the body). A version is only added when it has at
  least a fifth fewer triangles than the one above it, and a model with
  no switch distances gets the stock-style ones above.
  Left alone: stock models, characters (bipeds), first-person models,
  models whose shape needs most of their triangles, and the models listed
  in skip.txt (below). The [lod] lines in the log name each model with its
  triangles, most detailed > medium > low.
  The versions cost memory: on the Xbox, a model's vertices and triangles
  are part of the 22 MB of tag memory (and of the map file), and the
  [lod] line says how much they add (about 0.5 MB for three big models).
  If the map then needs more tag memory than the Xbox has, or is over the
  size limit by less than the versions take, the app takes them out again
  and rebuilds, and the log says so. The weapons & vehicles window
  measures maps without them.

  Seeing them: after an Xbox conversion that changed any model's levels of
  detail, "See the LODs" (next to "Open report") lists those models, how
  many are on the map and their triangles by level. Click one to see each
  level ("Most detailed", "Medium", "Low"...: drag to turn it, wheel to
  zoom; "Show the triangles" draws their edges) and roughly how far away
  the game switches to it, in metres and feet (on a 4:3 screen; a model's
  size on screen decides it). Versions you're happy with need nothing:
  they're kept in the LOD library and reused every time. "Never make LODs
  for this model" adds it to skip.txt, so no conversion makes lower-detail
  versions or gives switch distances for it again, on this map or any
  other that uses it (convert the map again to drop the ones it has now;
  delete its line from skip.txt to undo). Hover over the button for what
  it will do to the model shown. It also works after a map that didn't
  fit: those versions were taken out again, and the list says so.

  The LOD library
  Every version the app makes is kept in the LOD library folder (Settings
  tab; %LOCALAPPDATA%\HaloMapPorter\lod_library unless you change it), so
  the same model in another map, or the same map converted again, reuses
  them instead of making them again. In the folder:
    *.lod       The versions of one model's geometry, named after it (an
                edited model gets new ones).
    index.txt   One line per model added: date, tag path, triangles (most
                detailed > medium > low) and its .lod files.
    skip.txt    Models to leave alone (no lower-detail versions made, no
                switch distances given), one per line: a tag path
                (vehicles\jet\jet.model) or a name (jet: the model's or
                its object's). Lines starting with # are ignored. "Never
                make LODs for this model" in the LODs window adds to it,
                or write it yourself.
  Deleting .lod files (or the whole folder) has them made again next time.
  Stock maps are the exception: converting Custom Edition's Blood Gulch,
  Sidewinder and the other maps the Xbox also has gives you the Xbox's own
  version of the map, with the Xbox's vehicles. Maps the Xbox never had
  (Gephyrophobia, Infinity, Ice Fields...) keep their vehicles.

Portals ("Portals", Xbox tab)
  The engine draws a level by clusters: it starts in the one the camera is
  in and goes on only into the clusters it can see through the portals
  (openings) between them, so what's behind a wall isn't drawn. A level
  made without portals is one cluster: everything of it in front of the
  camera is drawn, walls or not. Bungie's portals are why Damnation, Chill
  Out and Danger Canyon draw from a half down to a quarter of what they
  would without them; Prisoner, Hang 'Em High and Longest have none.
  For a level that has none, the app finds its rooms and the doorways,
  corridor and tunnel mouths between them (where the open space narrows
  between two wider spaces), puts the boundary between two rooms on the
  doorway's wall plane where it can, and makes the rooms clusters and the
  openings portals. Glass, water, grates and fences (alpha-tested shaders)
  and invisible walls count as openings, since you can see through them.
  The level's geometry, collision and lightmaps stay exactly as they are;
  each cluster gets the level's sky, fog, sound and weather. With their
  portals taken out, indoor levels get close to Bungie's (in a typical
  view, Chill Out draws 198 triangles, Bungie's portals 182, none 591;
  Damnation 498, Bungie's 419, none 900) and open outdoor ones about
  half way (Danger Canyon 274, Bungie's 156, none 570). Prisoner gets 11
  rooms and draws 40% less, Longest 16 and draws a third as much.
  It only adds them when they'd cut what's drawn by 15% or more:
  open levels (Hang 'Em High, Spiral) are left as they are, and the log
  says so. The Xbox's own levels are never changed.
  "Also divide up the rooms of levels that have them" goes further: a level
  with clusters of its own has each of them divided further, the same way
  (each new cluster keeps its cluster's sky, fog, sound and weather, and
  sees what its cluster saw), and a level the app just gave portals is
  divided a second time. Either is kept only when it draws 15% less than
  before: Prisoner goes from 11 rooms to 17 and from 538 triangles in a
  typical view to 429; Deathtrap's own portals stay (dividing its rooms
  would save under 5%). Portals only hide what's behind walls: open
  outdoor areas draw everything in view however they're divided (the fog
  is what limits how far is drawn, see "Fog" below). "Leave every level's
  as they are" adds none. The [portals] line in the log says
  how many rooms and portals the level got and what it draws in a typical
  and a busy view, before and after (see "Performance" below). If Invader
  ever rejects the level with them, the app builds it as it was.

Custom weapons and "Use the Xbox's own weapons, vehicles and player model"
  Every Xbox map always contains the stock multiplayer weapons: the Xbox
  globals list them for the gametype weapon options. With the box ticked,
  the map's custom copies of stock weapons (and its custom vehicles and
  player model) are swapped for the stock ones, so there's one of each.
  Unticked, the map carries both: its custom versions, used where the map
  places them, plus the stock set. Custom models take a lot of the Xbox's
  22 MB of tag memory (Precipice unticked needs 25.5 MB, so it can't be
  built that way).
  With the box ticked, players are the Xbox's Master Chief. Players can
  only get into a seat their animations cover, and the Xbox's have none
  for the seats of many custom vehicles (a Falcon's pilot, a Mongoose, a
  truck bed). So those seats are switched to the closest seat the Xbox
  player has: the stock seat the map's own seat was made from, otherwise
  the usual one for the seat (driver, gunner, passenger). The log lists
  each one under [seats].
  Some custom vehicles aren't swapped for the stock one even with the box
  ticked:
    - wrecks and props nobody can ride ("scorpion_destroyed"), which must
      not turn into a working Scorpion
    - with "Keep every vehicle", a vehicle the map's scripts depend on
      (they read its health or shields, attach things to it, or seat
      players in it). For example, Bigass wrecks its Scorpion by script
      when the Scorpion's shields run out. The Xbox's Scorpion has no
      shields, so the script would keep wrecking and respawning it.

PC-only weapons (flamethrower, fuel rod gun)
  "Keep it" (default) keeps them, and both work on Xbox. The app
  automatically:
    - uses the PC player animations, so they can be picked up (the Xbox
      ones don't include the fuel rod),
    - uses the PC pickup icon sheet, so "Press X to pick up" shows the
      fuel rod icon,
    - uses the PC HUD for the flamethrower (the Xbox's copy is an empty
      placeholder), so its ammo counter, heat meter and crosshair show,
    - keeps the Xbox flamethrower's own model, animations and built-in
      ammo display, but fixes where it fires from (it was cut from the
      Xbox game unfinished): shots start at the nozzle and go where you
      aim, and its third-person flames and particle stream come out of
      the nozzle instead of a point beside the gun, as on PC,
    - takes them out of the Xbox weapon list, because the Xbox gametype
      weapon sets swap listed weapons by position (that's why a
      flamethrower turned into a rocket launcher). Side effect: special
      weapon sets like "Rocket launchers only" don't replace kept weapons.
  Or pick an Xbox weapon to replace it with (assault rifle, pistol,
  sniper rifle, rocket launcher, shotgun, plasma pistol, plasma rifle,
  needler).

The map's own level
  Tags the map has at the same path as a stock tag normally use the target
  game's stock copy. The level itself is the exception: the map's scenario,
  its BSPs with their lightmaps, and the scenario's own text (HUD messages,
  object names, help text) are always the map's, even for a modded stock
  level (levels\test\bloodgulch, levels\a10...). The log says "[keep]".

Weapons & vehicles window (Xbox)
  The "Weapons & vehicles..." button on the Xbox tab lists every weapon and
  vehicle a player can get on the map (the Convert tab's map, or the Build
  tab's scenario), with:
  - where the map uses it (spawn points, placed, starting weapon, spawn
    weapon, weapon list / vehicle slot);
  - Memory: how much of the Xbox's 22 MB of tag memory replacing it with
    none frees. Parts several weapons share (a family built on one model)
    only free up when all of them go, so pick those together;
  - Map size: how much of the map file it takes (textures and sounds
    included);
  - Replace with: Keep, None (remove it), another of the map's weapons or
    vehicles, or an Xbox one (the list shows what an Xbox one would add).
  The top says how much the map needs and how much has to go; the line under
  the list adds up your picks as you make them. Select several rows with
  Ctrl or Shift to set them all at once.
  Click a row to see it: the model shows on the right (its most detailed
  version, with its textures; drag to turn it, mouse wheel to zoom,
  double-click to put it back), with what it is: a weapon, or a vehicle and
  its seats (driver, gunner, passengers), or "no seats" for one nobody can
  drive or ride in.
  Pieces of vehicles: some maps make a vehicle break apart by script (a
  jet's wings, engines and burnt hull appear when it's shot down, a mech
  leaves a wreck). Those pieces are vehicles too, so they show up here. The
  app finds them in the map's scripts (the ones the scripts make and attach
  to another vehicle) and lists them right below their vehicle as "piece
  of ...". Replacing a vehicle with none (or with another vehicle) removes
  its pieces with it, and Keep brings them back; a piece can also go on its
  own (Keep or None).
  Measuring takes about as long as a conversion (the app prepares the map and
  builds a test copy, then reads back what each part takes). A conversion for
  the Xbox that doesn't fit (memory or file size) measures the map on the way,
  from the tags it already prepared, so the window opens right away after it
  (this happens when no picks are set yet; with picks, the earlier measurement
  is still the one shown). Measurements are kept between runs of the app, until
  the map file or the Xbox tab's weapon/vehicle options change; "Measure again"
  starts over. The figures
  before Invader's optimize rebuild are exact; what that rebuild saves on top
  is an estimate (within about 0.1 MB), so leave a little room when it says
  "right at the limit".
  Picks are saved per map and used every time you convert it for the Xbox;
  the log lists them as a [replace] line. None takes the weapon out of its
  item collections (a collection left empty loses its spawn points), the
  palettes and the players' spawn weapons; in the globals weapon list and
  vehicle slots its place stays, empty. Placements the map's scripts refer to
  by name (a jet the scripts respawn, its pieces) stay too, empty: Invader
  only builds scripts whose names all belong to a placement. The game makes
  nothing for an empty placement, and the scripts find no object there, as
  if it had been destroyed: a jet's pieces are never made, its health reads
  as gone. Try the map once to check its scripts still do what you want.

Level reflections, fog and far scenery: the "Level..." window (Xbox)
  The Xbox draws every level surface at least twice: its lightmap, then
  its textures. A shader with a reflection (a cube map, and a perpendicular
  or parallel brightness above 0) draws it a third time, and a fourth when
  its lightmap brightness scale is under 1 (which dims the reflection in
  the shadows). On a level where most surfaces reflect that's most of the
  level drawn 3 or 4 times: Deathtrap reflects on 97% of its triangles and
  costs the Xbox about 5,900 triangle passes in a typical view and 16,900
  in a busy one, against Sidewinder's 3,350 / 8,260 (among the busiest
  stock maps that still run smoothly).
  Reflections are part of how a level looks, though, and they're only part
  of what it costs: dropping 45 of Deathtrap's 50 halved those passes but
  took its worst spot on an Xbox from 10-12 to only 12-14 frames a second,
  and the level looked noticeably flatter. So a conversion keeps every
  reflection unless you choose otherwise for that map in this window. When
  a level's reflections cost much more than Sidewinder's, the Performance
  section says so.
  Dropping a reflection sets its brightnesses to 0 and removes its cube map
  (freeing its memory too); the surface keeps its textures, bump map and
  lightmap. The log's [reflections] line lists the ones dropped.
  The "Level..." button on the Xbox tab shows this for the Convert tab's map
  (or the Build tab's scenario), with the level's fog and far scenery (see
  "Fog" and "Far scenery" below). Measuring prepares the map as for a
  conversion without building it (seconds to a minute).
  - When converting: keep every reflection (the default), drop the
    suggested ones (the ones that cost the most passes for how little they
    show, until the level costs about what Sidewinder does: cheaper, but
    flatter in places), or drop the ones you tick. Ticking or unticking one
    switches to your own picks, starting from what was shown.
  - The list: each reflecting shader with its extra passes (its triangles
    drawn again in an average view), Shows (how much of what you see is its
    reflection), Covers (how much of the screen it is), its triangles and
    what's suggested. Biggest cost first.
  - The picture: the level drawn from where players stand (textures,
    lightmaps and reflections, close to the Xbox, not exact), on your
    graphics card (OpenGL 2; without it, the app draws it itself, more
    slowly). As made, As converted, or What it loses (the converted picture
    dimmed, with the reflections that go lit up). Click a shader to see the
    views where its reflection shows, tinted pink ("Tint selected"); the
    arrows go through them. To fly around, click the picture, then drag
    with the mouse to look and hold W A S D to move the way you're looking
    (Q / E down and up, the arrow keys turn and move too; Shift faster,
    Ctrl slower); the mouse wheel steps forward and back.
  - "Show portals": each portal outlined in yellow, glowing orange where
    you can see through it, and each room (cluster) tinted a colour of its
    own. The engine draws the room you're in, then only the rooms it can
    see through portals. The line above the list says how many rooms and
    portals the level has (after the app's own, when it adds them).
  - The line under the picture: the level's triangle passes in a typical /
    busy view as made and with your choices, next to Sidewinder's.
  - Fog: as the conversion leaves it (automatic, the default), as the map
    has it, fully opaque at its distance, three quarters of it or half of
    it, or Other: fully opaque at a distance you type in the box beside it
    (in world units; 1 unit = 10 feet, about 3 m). The picture shows it,
    and follows as you type (its sky is a plain gradient, not the level's
    own sky).
  - Far scenery: always drawn (the default), or not past 300, 200, 150,
    100 or 60 units. The picture shows the level only, not its objects.
  Hover over the choices and checkboxes for what each does. Choices are
  saved per map and used every time you convert it for the Xbox.
  Measurements are kept between runs of the app until the map file
  changes (or the portals option); "Measure again" starts over.

Fog (Xbox)
  The engine draws nothing past a level's fog, but only when the fog is
  fully opaque there (its sky's maximum density exactly 1): then it moves
  the view's far end to the fog's opaque distance, and the level, objects
  and effects past it aren't drawn. At 92% it still draws everything.
  Deathtrap uses Precipice's sky: 92% at 110 units, so the airbase 200-300
  units out and many of its trees were drawn behind the fog. By default a
  conversion makes fog that's nearly opaque (90% or more) fully so, at its
  own distance: what was 8% visible past it is gone, distant hills
  included (the sky shows there instead). The log's [fog] line says what
  changed. Per map, the "Level..." window can keep the fog as the map has
  it, make it fully opaque nearer (three quarters or half its distance:
  less drawn, foggier), or fully opaque at a distance you type (nearer or
  farther than the map's; not nearer than where its fog starts). The
  map's own skies only; Bungie's levels keep theirs.

Far scenery (Xbox, "Level..." window)
  Halo 1 has no draw distance for objects, but it draws nothing of a model
  smaller on screen than its smallest switch distance ("super low", in
  pixels across). With "Far scenery" set to a distance, each scenery and
  device model the map places gets that set to its size on screen at the
  distance (from the smallest object using it), so it vanishes about that
  far away. Vehicles, characters, weapons and items are left alone. Big
  ones pop out of view, so pick a distance past where they matter, or
  behind the fog. The log's [scenery] line lists them. Off by default.

Dynamic lights (Xbox)
  A dynamic light (a muzzle flash, an explosion, a map's lightning) lights
  the level by drawing part of it again, twice (diffuse and specular), out
  of a fixed amount for everything in a frame (32,768 triangles). The part
  isn't just what the light reaches: the engine takes whole pieces of the
  level (subclusters, up to about 100 triangles each, some spanning much of
  it) whose box touches the box around the light, and draws again the ones
  in view, up to 4,096 a pass. Deathtrap's lightning (radius 25, three at
  once, high above its vehicle) takes 10,600 to 12,500 of the level's
  12,749 triangles and stops the Xbox with an out-of-memory error. Made
  smaller, it no longer reaches anything it could be seen on, but still
  takes thousands.
  So a light of the map's own that would take more than one pass can,
  where the map places it, is turned off ("dynamic" off: the engine makes
  no light for it; a lens flare stays). The engine can't light only
  players and vehicles: every light it makes lights the level too. What
  made the light still plays (Deathtrap's thunder). A light the map doesn't
  place (a custom weapon's explosion: where it goes off isn't known) is
  kept to Bungie's biggest, radius 15 (the Scorpion shell's explosion).
  The log's [lights] line lists them. There's no option: those lights
  don't run on the Xbox as they are.

Content nobody can get (Xbox)
  Xbox maps have only 22 MB for level geometry and other tag data,
  so multiplayer maps leave out what no player can get in a match. The log
  lists each one as a [trim] line:
  - palette entries the scenario never places (weapons, equipment, vehicles);
  - the single-player character and the weapons it carries (the stock
    assault rifle and pistol). Matches spawn players as the multiplayer
    character, so that one takes its place;
  - the Warthog, Ghost and Scorpion slots when the map places none of them
    and has no race vehicle spawns (Race is the only game type that spawns
    them by itself);
  - the cheat power-ups (the list only the debug cheats and the script
    command cheat_all_powerups use). A map whose scripts call
    cheat_all_powerups keeps them, and so does one whose scripts can't be
    read back to check. Power-ups a map gives out its own way stay anyway:
    Orange Box's potions, for one, spawn their own power-up copies.
  Everything a player can get stays: weapons and items at spawn points,
  starting equipment, the multiplayer character's spawn weapons, grenades,
  the Oddball and CTF flag, and the parts the map's own weapons are built
  from (a mod's custom weapons often reuse stock effects, sounds and models,
  so folders like weapons\sniper rifle can still show up).

Custom weapons, vehicles and player model
  Maps made with custom tagsets often carry their own (high resolution)
  copies of the stock weapons, vehicles and player model. Every Xbox
  multiplayer map already contains the originals, and on a stock XBE
  custom vehicles can't spawn anyway (only the three slot vehicles do). With
  "Use the Xbox's own weapons, vehicles and player model" ticked (the
  default), the map uses the Xbox originals instead, matched by name
  (".../sniper_rifle.item_collection" -> sniper rifle, ".../ghost_mp.vehicle"
  -> ghost, ...). This saves a lot of space and memory. The log lists
  every swap as [xbox-mp] lines.
  "Other custom ones" covers custom weapons with no Xbox original (for
  example a beam rifle): keep them, or replace them with an Xbox weapon.
  Their models, effects, projectiles and animations take some of the Xbox's
  22 MB of memory (on the Xbox a model's geometry is kept with the tag
  data), so replacing them can be what makes a big map fit.
  Unticked, the map also keeps its own globals settings where they're the
  map's content: its grenades (e.g. custom grenade types), its weapon list
  (what the gametype weapon options use) and its multiplayer player model,
  whose weapons are the ones players spawn with, plus its own fall damage
  and player tuning. The rest of the globals (menus, fonts, multiplayer
  sounds) stay the Xbox's. Either way, if the map has longer weapon and
  vehicle name lists than the Xbox (its custom weapons' pickup names, its
  vehicles' names), the Xbox uses the map's lists.
  Pickup icons: if the map's icon sheet has more icons than the Xbox's,
  the map gets a sheet with both: the Xbox's own icons and controller
  button glyphs first, unchanged, then the map's extra icons (its custom
  weapons'), shrunk to the size the Xbox draws them.

Sounds
  The Xbox only plays Xbox ADPCM sounds: mono 22 kHz, stereo 22 kHz and
  stereo 44 kHz. MCC and Halo PC also play Ogg Vorbis and uncompressed
  16-bit PCM, which are silent on the Xbox (MCC mods often use PCM for
  their custom weapons). The app re-encodes those as Xbox ADPCM, turns
  mono 44.1 kHz sounds into 22.05 kHz ones, and splits sounds too long
  for the Xbox to load at once.

HUDs from MCC maps
  MCC draws the HUD on a canvas twice the Xbox's size: its HUD tags place
  everything at double the Xbox's positions, and its HUD bitmaps are drawn
  at half size ("half hud scale"). The Xbox ignores that flag, so MCC HUDs
  used to come out twice as big. The app now halves the map's own HUDs'
  positions and sizes, shrinks HUD bitmaps MCC draws at half size to the
  resolution the Xbox draws them at (a quarter of the memory or less), and
  places elements MCC anchors to the screen centre (scopes) from the HUD's
  corner instead. It tells an MCC HUD from a Custom Edition one by
  comparing the stock assault rifle HUD with the Xbox's.

The output is an NTSC (US) Xbox map.

REPLACING A STOCK XBOX MAP (e.g. longest.map)
Halo only loads a map whose file name matches the name stored inside it,
and the Xbox menu only lists the map names it knows. If you rename
spiral.map to longest.map in Explorer, the inside still says "spiral" and
the game hangs on "loading level". Let the app do the naming:

  - Build from my tags: on the Xbox row, pick the map to replace from the
    "named:" list (e.g. longest). The map is built as longest.map with the
    right name inside.
  - Convert a map: when "To game" is Xbox, pick the map to replace under
    "Xbox name". "Save as" follows it (and starts in the Build tab's Xbox
    folder, if you set one). The note next to Convert says which map it
    replaces, and the app asks before converting under a name the Xbox
    menu doesn't know.

Which names are offered depends on "Map names" on the Xbox tab:
  - Standard (13 stock maps): beavercreek, bloodgulch, boardingaction,
    carousel, chillout, damnation, hangemhigh, longest, prisoner, putput,
    ratrace, sidewinder, wizard.
  - Expanded map pack (27 maps): for Xboxes with a 27-map pack. Listed in
    the pack's menu order; the number is the menu position, not part of
    the name:
       0 chillout     7 decidia      14 zerohour       21 longest
       1 hangemhigh   8 doubletake   15 wizard         22 putput
       2 prisoner     9 redshift     16 atlas          23 madhouse
       3 dammy       10 outbnd       17 bloodgulch     24 calamity
       4 derelict    11 exhibit      18 boardingaction 25 overflow
       5 downrush    12 hotbox       19 imminent       26 tinker
       6 badcreek    13 ratrace      20 sidewinder

Then copy the file over that map on your Xbox. Don't rename it afterwards.
The Convert tab also warns you ("renamed, won't load as ...") if the map
you pick to convert has been renamed.

Tested: spiral built as longest.map loads on original Xbox hardware and in
the Halo 1 decomp (build 64).

MAP FOLDERS AND MAP CARDS (Halo Universal XBE)
With the Halo Universal XBE, each map can live in its own folder inside
maps, and the multiplayer menu lists every folder after the 13 stock maps.
Nothing has to be renamed to a stock map name, and any number of
putput.map test builds can sit side by side.

  1. Xbox tab: set "Map names" to "Map folders (Halo Universal XBE)".
  2. Build tab or Convert tab: click "Map card..." (it replaces the
     "named:" / "Xbox name" list). Fill in:
       - Folder name: the folder on the Xbox (maps\<folder>\). Up to 30
         characters the Xbox drive can store. System link players need
         the same folder name.
       - Map name and Description: what the menu shows.
       - Picture: choose or drop an image, drag it to move it, and use the
         slider or the mouse wheel to zoom. The frame is exactly the part
         Halo shows (140 x 114).
     The right side previews the Xbox map select screen. Halo's menus don't
     wrap text (they cut it off at the edge), so the line breaks are worked
     out with Halo's own letter widths and written into info.txt; just type
     the description as one paragraph. It warns when the name or
     description gets cut off or uses a character Halo's font doesn't
     have. The picture shown is the real compressed result. The 16:9
     button shows how a widescreen TV stretches the menu.
  3. Build or Convert. The Xbox folder gets:
       <folder>\<mapname>.map
       <folder>\info.txt     line 1 = name, the rest = description (with
                             its line breaks)
       <folder>\preview.bin  the picture: 256x128 DXT1, like Halo's own
                             map pictures (only the top-left 140x114 shows)
     Copy the whole folder into maps on the Xbox.

The card is saved per map or scenario (your picture is copied in), so you
only fill it in once; later builds write the same files again.

MCC Steam Workshop maps fill in their own card. Pick the map straight
from the workshop item, for example
  steamapps\workshop\content\976730\2962107814\maps\chiron.map
and the card starts from the mod's own info: the title and description in
its multiplayer\<map>.json, and that .json's thumbnail picture (the one
MCC shows in its map list). The folder name starts as the title. This
works for any MCC mod folder laid out the same way, not just Steam's.
It only fills in a new card: a card you've already edited is kept. Open
"Map card..." to check it, since MCC titles and descriptions can be
longer than the Xbox menu shows.
The Halo Universal XBE shows preview.bin in the map select list, the
lobby and the system link game browser. A folder without one uses Halo's
"unknown map" picture. The XBE also breaks long lines in any info.txt the
same way the card does, so a hand-written info.txt works too.

Menu order: folders are listed alphabetically after the 13 stock maps. To
set your own order (for example in groups), or to hide maps, put an
order.txt in maps on the Xbox. See "Example order.txt" in xbe_universal.

Map list sorting (Halo Universal XBE test build 20 or later): the Xbox can
sort its map list by players, gametypes and genres (X changes the
sort). The card has a field for each, at the bottom of "Map card...":
  - Players: how many players the map suits, like 4-16. Leave it empty
    when the description already says ("Built for 4-16 players.").
  - Gametypes: which of CTF, King (of the Hill), Oddball and Race the map
    has, plus Slayer.
  - Genres: any names, separated by commas. "Add genre..." offers the ones
    the Xbox starts with: Competitive, Casual, Memes, Artistic, Custom
    vehicles, Custom weapons and Total conversions. Each Xbox can rename or
    reorder them in maps\genres.txt.
They go at the end of info.txt as "Players:", "Gametypes:" and "Genres:"
lines, which the Xbox doesn't show in the description. Older XBEs show
them as part of the description.
After each build or conversion the app reads the finished map and fills
in what you haven't set yourself:
  - Gametypes from the map's own markers: both CTF flags, a hill, a ball
    spawn, race checkpoints (Slayer always works).
  - Players: a guess, worked out from Halo's own 19 multiplayer maps (the
    Xbox's 13 and Custom Edition's 6) and the player counts their menus
    give. It looks at how far apart the spawn points are (the box they
    fill), how many vehicles are placed, and how many spawn points there
    are: a map with 4 or more vehicles, or spawn points spread like Hang
    'Em High or wider, gets 4-16; a mid-sized one like Damnation 4-8; a
    small one like Chill Out 2-8; and never more players than spawn points.
    That gives Bungie's numbers for 16 of the 19 (it calls Rat Race and
    Chiron TL34 4-8 instead of 2-6, and Danger Canyon 4-16 instead of
    2-12). The log names the stock maps it's most like. Skipped when the
    description gives a player count.
  - Genres, only while the card has none: Custom vehicles or Custom
    weapons when the map has vehicles or weapons Halo doesn't, Total
    conversions when it has its own multiplayer character or hardly any of
    Halo's weapons.
The log says what it filled in. Anything you type in these fields stays as
you typed it. On the Xbox, the genres are each player's own choice: a map
shared over system link keeps the Genres line that Xbox already had.

Many maps at once: Map packs tab -> "Map list...". Pick an Xbox maps folder
(a copy on your PC, or the Xbox's over FTP) and click Load: every map
folder and pack map is listed, plus Halo's 13 maps, with their players,
gametypes, genres, favorite, hidden and plays.
  - Scan all maps (or, with several selected, Scan selected maps): reads
    each map and fills in what it doesn't have yet, the same way a build
    does: a Gametypes line, a Players guess (not when the description
    already gives a count), and genre suggestions for maps without genres.
    Tick "Replace Players / Gametypes lines they already have" to redo
    those too. It takes a second or two per map; Stop scanning stops it.
    When it's done it lists what each map got. Nothing is written until
    you click Save, so you can look it over (and change genres) first.
  - Genre: select maps (Ctrl or Shift for several, or Select all), pick or
    type a genre, then Add or Remove. Folder maps get it in their
    info.txt; Halo's own maps get it in the folder's genres.txt ("Casual:
    chillout, wizard"), which is made if there isn't one.
  - Favorite..., Hide, Show: these live on the Xbox in
    E:\TDATA\4d530004\hmp_stats.txt. Copy that file to the PC with Halo
    switched off, pick it as "hmp_stats.txt", make the changes, Save, and
    copy it back before you start Halo again (Halo rewrites the file while
    it runs, so an edit made while it's on gets undone).
Save writes only what changed (a * marks those rows) and says which files.

Changing a card later: open "Map card..." again and click Done. If the
map is already in the card's folder, Done rewrites that folder's info.txt
and preview.bin right away, so nothing needs to be rebuilt or converted.
The line next to Done says where the files go.

Maps that are already on Xbox (someone else's port, or one you built
earlier): use the Convert tab, nothing gets converted.
  1. Map to convert: pick the Xbox .map. With "Map folders" on, To game
     switches to Xbox and the Convert button becomes "Save map card".
  2. Click "Map card..." and fill it in. If the map already sits in a map
     folder (its only .map, next to info.txt or preview.bin), the card
     starts from that folder's name, text and picture. Older ports all use
     stock names like bloodgulch.map, so give each one its own folder name.
  3. Click "Save map card". The map is copied as it is into
     <Xbox folder>\<folder>\ (the Build tab's Xbox folder, or the folder
     shown in "Save as") with info.txt and preview.bin. If the map is
     already there, only the card files are rewritten.
Maps you build or convert remember their card, so opening one of your
Xbox outputs here opens the same card the Build or Convert tab uses.


MAP PACKS (Halo Universal XBE, test build 14 or later)
------------------------------------------------------
Turns an Xbox mod pack (a set of maps that replace the 13 stock multiplayer
maps, plus the pack's own ui.map) into Halo Universal map folders, all in
one go, with the names, descriptions and pictures the pack's menu shows.

  1. Map packs tab: Mod pack -> Browse... and pick the pack's folder (or
     the maps folder inside it, the one with ui.map). The app reads the
     pack's ui.map and lists its maps in the pack's menu order (with the
     stock map each one took the place of), then any other .map files in
     the folder. Maps that aren't multiplayer maps (a campaign map, for
     example) start unticked.
     Packs with a longer map list work too: Halo 1 NHE and the packs built
     on it have 27 maps (and room for 3 more). The list comes from the
     pack's own default.xbe, so keep it in the pack's folder next to maps;
     without it the app recognises NHE's 27-map list from the maps and the
     menu. The log's "Map list:" line says which it used.
  2. Pack name: the folder the whole pack goes in, e.g. CEX. Characters
     the Xbox drive can't store are dropped (CEX+ becomes CEX).
  3. Click a map to see its picture as the Xbox will show it, and its menu
     name and description (you can change both). Untick maps you don't
     want. Double-click a folder name to change it.
  4. Optional logo, drawn on every picture in the pack so the pack's
     authors keep their credit. Most packs put their own logo on the main
     menu in place of the HALO logo: the app finds it in the pack's ui.map
     and suggests it when you pick the pack (the Logo box then says
     "ui.map: <picture>"). "Pack's logo" goes back to it, and steps through
     the others if the menu has more than one; Browse... picks a PNG instead
     (transparent edges look best); "No logo" leaves the pictures alone.
     Then a size (24 to 64 pixels) and a corner. Only 140 x 114 of each
     picture shows, so 32 pixels is usually plenty for a solid logo; a thin
     outline logo reads better at 48 or 64. Menus often draw their logo
     see-through and brighten it with effects, so a logo taken from the menu
     has its lines made solid for the stamp.
  5. Save into: an Xbox maps folder, or anywhere you copy from later. Then
     Create pack. It writes:
       <Pack>\<Map>\<file>.map     the pack's map, as it is
       <Pack>\<Map>\info.txt       its menu name and description, and the
                                   map list's Players / Gametypes lines
                                   and genre suggestions (read from each
                                   map, as for map cards)
       <Pack>\<Map>\preview.bin    its picture, straight from the pack
       <Pack>\order.txt            the pack's menu order
       <Pack>\series.bin           the logo (if you picked one)
     Copy the <Pack> folder into maps on the Xbox, next to your other map
     folders.

The logo isn't baked into the pictures: the XBE draws series.bin on each
picture as it loads, so swapping that one file changes the logo for the
whole pack. The preview here is the exact result the Xbox shows.
Packs need a Halo Universal XBE with map packs (test build 14 or later).
The pack's own maps play as they are: nothing is converted. The pack's
other files (its ui.map, XBE, videos) aren't needed.

Menu order: a pack's maps appear together, in the pack's order.txt order.
The maps\order.txt on the Xbox can place a whole pack (a line with the
pack's name, e.g. CEX), one of its maps (CEX\Abyss) or hide them (-CEX).

READING THE LOG AND REPORT
--------------------------
The log lists every automatic change:

  [fix]   A broken reference in a tag was reset so the map can build.
          These are usually old bugs in stock Bungie tags that the
          original tools ignored. Also: a value above or below what
          Custom Edition and the Xbox allow was clamped. MCC's tools
          accept some of these, such as a sound's gain modifiers
          above 1.0 (louder than normal); that sound then plays at
          normal volume. All of the map's tags of that kind are
          checked at once.
  [swap]  A stock Custom Edition tag was corrupt, so your map's copy
          was used instead.
  [keep]  You edited a stock tag, so your version was used.
  [xbox]  A conversion made for Xbox (models, HUD, sounds, scripts).
  [pc]    Xbox models converted back to PC models.
  [slots] An Xbox vehicle slot was changed (Xbox tab).
  [xbox-mp] A vehicle or PC weapon was swapped, removed or kept for Xbox.
  [shrink] Textures were halved to fit the Xbox size limit.
  [textures] (also) Textures were halved to fit the texture memory target.
  [scripts] A script function the target game lacks was replaced.
  [bc7]   MCC-only BC7 textures were re-encoded as DXT.
  [textures] Textures were scaled or compressed like stock Xbox maps.
  [shaders] Layered transparent shaders were flattened for Xbox.
  [effects] Looping prop effects were lightened for Xbox.
  [lod]   Lower-detail versions were made for models, or models got
          switch distances for theirs (see "Levels of detail" above).
  [portals] A level made without portals got them (see "Portals" above).
  [reflections] Level reflections were dropped, as chosen in the Level
          shaders window (see "Level reflections" above).
  [fog]   The level's fog was made fully opaque (see "Fog" above).
  [scenery] Scenery and devices stop being drawn past a distance (see "Far
          scenery" above).
  [lights] Dynamic lights that would have the Xbox draw most of the level
          again were turned off or shrunk (see "Dynamic lights" above).

Warnings (for example "There are 1/16 Slayer spawns") don't stop the
build. They point at things worth checking in your map.

Xbox builds end with a "Performance" section (in the log, and in the
report when the map builds): what the Xbox has to draw.
  - Level: the level's triangles, its clusters and portals, and about how
    many of them the engine draws in a typical view and in a busy one
    (busier than 9 views in 10), standing on its floors around where the
    players spawn (not in trigger volumes named for killing, like
    Deathtrap's plains) and looking around: through the portals, as the
    engine does, and on screen. Next to it, the same for stock Xbox maps (Blood
    Gulch 588 / 2,033, Damnation 419 / 2,134, Sidewinder 784 / 2,115,
    Prisoner 893 / 2,433, the most of any). A level far above
    those is the likeliest reason a map runs slowly; portals only help
    where walls block the view.
  - Level shaders: the triangle passes the Xbox draws of the level in a
    typical and a busy view (every surface twice, reflecting ones three or
    four times), as converted and as made, next to Sidewinder's 3,350 /
    8,260 (see "Level reflections" above).
  - Models with no lower-detail versions, drawn in full at any distance,
    with their triangles and how many are on the map (placed, or at weapon
    and item spawn points), most triangles on the map first.
  - Models whose lower-detail versions are never used (no switch
    distances), when the app didn't fill them in (stock models with the
    Xbox tab's LOD option off).

The full report for each build is saved in:
  %LOCALAPPDATA%\HaloMapPorter\reports\
("Open report" opens the latest one.)


TROUBLESHOOTING
---------------
"Invader wasn't found"
    Settings tab: click Download Invader, or Browse to the folder with
    invader-build.exe.

"Built, but couldn't write ...map"
    The game (or something else) has the map open. Close it and click
    Build/Convert again.

Xbox hangs on "loading level"
    The map file was probably renamed after building. See "Replacing a
    stock Xbox map" above.

"The map is too big for Xbox"
    See "Size limit" above. Make sure "Shrink textures to fit" is ticked
    on the Xbox tab. If even that isn't enough, the app stops early and
    shows where the size goes (textures, sounds, models, level geometry),
    so you know what would need trimming.

"The map needs more memory than Xbox has for it"
    Separate from the file size, the Xbox has 22 MB of memory for the
    map's level geometry (BSP) and tag data, models' geometry included.
    Textures and sounds don't count toward it, so shrinking them doesn't
    help. When a
    map is over, the app first rebuilds it with Invader's optimize option,
    which stores identical data only once (the log says "[optimize]"; the
    build takes longer). If it's still over, the map needs a simpler BSP or
    fewer custom weapons and objects.

Xbox freezes when something comes into view, or as the match starts
    Usually a shader the Xbox can't draw. Build with the latest version
    (it flattens layered shaders). If it still happens, note where you
    were looking; removing objects in groups narrows down which one.

Framerate drops or stutters while turning (Xbox)
    Too many big textures or particles in view. Keep "Max texture size"
    on "512, props 256" and Lightmaps on 256.

"This map is protected"
    Some Custom Edition maps are "protected": the author scrambled the
    tag names so the map can't be taken apart, and converting needs them.
    Use an unprotected copy instead (for example the map's MCC release).

"Xbox output needs Xbox stock tags"
    Settings tab: set the Xbox tags folder (or the Xbox maps folder).

"Your map's scripts call '...', which doesn't exist in ..."
    The app converts scripts automatically (see "Scripts"), so this only
    happens for something it doesn't know. Remove or change that line in
    your .hsc script and build again.

"Your map's scripts name the ... '...', which the ... build of this map doesn't have"
    A script line names something (an object, a nav point, a tag) that
    didn't make it into the converted map. The message shows the line.
    Change or remove it in your .hsc script, or send the error over for
    help.

"Build failed and no automatic fix applies"
    The log shows the error and the tag it came from, plus the path to
    the full build log. Fix that tag in Guerilla, or send the error over
    for help.

Map not showing up in Custom Edition
    Make sure the .map is in the Custom Edition "maps" folder and the
    file name matches the map's name (the app handles this when it
    builds).

Starting fresh
    Delete %LOCALAPPDATA%\HaloMapPorter (cached tags, work files,
    reports) and/or %APPDATA%\HaloMapPorter (settings). The app recreates
    them as needed.


LIMITATIONS
-----------
- Xbox output targets NTSC (US) only.
- Maps built for MCC by this app use Invader, not MCC's official tools.
  Test them in MCC before sharing.
- Anniversary (remastered) graphics aren't carried over. Only the
  classic graphics layer converts.


FILES
-----
HaloMapPorter.exe            The app
HaloMapPorter_source.zip     Source code (C++)
README.txt                   This file

Settings:   %APPDATA%\HaloMapPorter\settings.ini
Cache:      %LOCALAPPDATA%\HaloMapPorter\ (the LOD library is in it too,
            unless you moved it)
Uninstall:  delete this folder and the two folders above.
