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

   "With each Xbox build, write a camera tour..." (off by default)
       For testing with Halo 1 Unlimited's perf log (see "Camera tours"
       below).

3. Your settings are saved automatically.


CONVERTING A FINISHED MAP  (tab: "Convert a map")
-------------------------------------------------
Use this for a map you already have as a .map file.

1. Map to convert: click Browse and pick the .map file.
   The app reads the file and sets "From game" for you
   (it shows "Detected: MCC map ..." or similar).

2. To game: Xbox, unless you pick another (MCC or Halo Custom Edition).
   An Xbox map starts at Halo Custom Edition (or at Xbox with "Map
   folders" on, to give someone else's port its map card).

3. Save as: filled in with the target game's maps folder. Change it with
   Browse if you want it somewhere else.

4. Leave "Use Custom Edition's shared bitmaps / sounds / loc maps" ticked.
   The map then reads stock textures and sounds from Custom Edition's
   bitmaps.map, sounds.map and loc.map instead of carrying its own copies.
   (Spiral: about 21 MB instead of 66 MB.) Untick it only if you want a
   map that doesn't depend on those files.

5. Click Convert. Progress shows in the log box. When it's done, use
   "Open output folder" or "Open report".

Unpack (Xbox): for a map you'll fine-tune on the Xbox tab, click Unpack
first. It extracts the map's tags (or reuses the ones extracted before:
extracting only ever happens once per map file) and prepares them for the
Xbox with the options as they are, the way a conversion does, but doesn't
build the map. While it's at it, it measures the map for the Xbox tab's
windows from those same tags (Weapons & vehicles, when no picks are set;
Level; LODs), so they open on it straight away instead of preparing the
map themselves. Then go through the Xbox tab and its windows, and click
Convert:
- When the map file and every option the tags were prepared with are as
  they were (the Xbox tab's options, your weapons & vehicles picks, the
  Level window's choices, the models' choices in the LODs window), Convert
  builds from the unpacked tags: no preparing again. The map is the same
  one a conversion from scratch makes; the log says it's building from
  them, and the report keeps what preparing them changed.
- When any of those changed, Convert prepares the map again with them (it
  can't know what else a change touches), as it always has.
The windows use the last unpacked (or converted) tags even after you
change options: the LODs window does the models' levels of detail and
trims again on them with the options now (seconds) when those changed;
the Weapons & vehicles window measures them with the models done again
the same way. Only options that change which tags the map has (the Xbox's
own weapons and vehicles, vehicle slots, custom weapon choices, keeping
every vehicle, animation compression) make them prepare the map again.
What was unpacked is remembered when the app closes (the tags stay in
the cache folder's work\convert until the next conversion or Unpack).
Precipice, on a 2-core PC: a conversion 57 s; Unpack 25 s (17 s prepared,
8 s measured), then Convert 23 s.

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
warns. Textures seen up close (vehicles, characters, weapons and their
first-person models, the first-person arms) are halved only after
everything else, and only when that gets the map to the target (a map
far over it keeps them sharp rather than blurring them for nothing).
HUD art is never halved (it's fitted to the screen, see HUD textures).
"Update the map" keeps the textures as the conversion left them. The log and the report show the figures, e.g.:
  Xbox memory: tag memory 10.1 MB of 22 (tag data 7.6 MB, BSP 2.5 MB);
  textures 22.2 MB; target 20 MB + 11.9 MB of free tag memory (OpenCE
  lends it) = 31.9 MB
Lower targets leave more room for what you see at once (effects, other
players' weapons), at the cost of softer textures on maps that are over.
On Halo Universal (no lending), "18 MB - Safe" is the closer match.

Texture sizes (Xbox tab): the Xbox loads textures as things come into
view. A map that needs much more texture memory than stock maps stutters
(or briefly loses HUD graphics) wherever you can see most of it at once.
"Max texture size" handles the map's own textures (HUD art has its own
pass, see HUD textures; menu art is left alone):
- "Stock-like budget (smoothest)", the default: level and sky textures at
  most 512 px, and the level's textures together kept to about 6 MB, like
  stock outdoor maps (Blood Gulch 3.3 MB, Sidewinder 5.3 MB). If the level
  needs more, the map's own bump maps are first packed into less memory
  at their size (see "Bump maps" below), then bump and detail maps are
  halved (not below 128 px), then the biggest color textures, and only
  then bump maps a shader cuts holes with (see "Cut-out bump maps"
  below). Objects
  get the largest Bungie's own of their kind have: a vehicle's,
  character's, weapon's (first-person model too), the first-person arms'
  and scenery's textures at most 512 px (their bump, detail and cube maps
  256), ammo and other pickups 256; effects, decals and anything else
  256. An effect's bitmap (a particle's, a particle system's, a
  contrail's, weather's) is also held to Bungie's biggest as a whole,
  336 KB (the pistol's muzzle flash, two 256x256 frames; all of Bungie's
  effect textures together come to 4.6 MB): one bigger has every frame
  halved until it fits (CMT's beam cannon's plasma arcs, 30 frames of
  256x256, 1.2 MB, become 128x128 frames, 0.3 MB). Small on screen, where
  they're seen, they look about the same. (It used to cap every object's textures at 256, half what Bungie's
  vehicles and weapons have: a vehicle made with 1024 or 2048 textures
  came out a quarter or an eighth of its size, and looked it.)
- "512 max (sharper)": everything at most 512 px, no budget.
- "1024" / "No limit": for testing.
How a texture is halved: usually its own next smaller version (its next
mip level, made with it) becomes the new full size. Two kinds are made
again instead:
- See-through cut-outs (leaves, fences, grates, chain link): a shader
  that alpha-tests a texture draws a pixel only where its alpha is over
  half. Averaging blurs the cut-out edges into partly transparent pixels,
  most of them under half, so leaves and grates thin out and a couple of
  halvings down all but vanish. For a texture a shader alpha-tests (a
  model shader's base map unless "not alpha tested", an environment
  shader's bump map when "alpha tested", a transparent shader's first map
  when "alpha tested"), if the usual way would leave less than 90% as
  much of it solid, each smaller level is made from the one above and its
  alpha scaled so as much of it stays solid as at full size.
- A texture whose own next version looks unlike it (faded out, as detail
  maps are by design, or its alpha thinned or broken): the new full size
  is a 2x2 average of the old one; its smaller versions stay as they were.
DXT1 textures with see-through pixels stay DXT1 (its 1-bit transparency)
rather than doubling in size as DXT5.
Uncompressed textures are also compressed to DXT (a quarter of the memory
or less).
Bump maps: Bungie's Xbox bump maps are palettized (P8): a byte per pixel,
each one of 256 directions from a palette the Xbox keeps (their PC copies
are 32-bit, four times the memory). A PC or MCC map's own bump maps are
32-bit (or DXT5 from MCC's BC7), so when the level is over its budget,
each of them, biggest first, is packed before anything is halved: into
P8 or DXT1 (an eighth of 32-bit, half of DXT5), whichever turns its
pixels least from their own directions. The palette only reaches 53
degrees from flat, so the steep slopes of Halo 2-style bump maps (curbs,
grooves) keep closer as DXT1; gentle ones are closer as P8. Either turns
pixels by a few degrees on average, where halving turned them several
times as far: New Mombasa Classic's 23 custom bump maps had needed 29
halvings to fit, now 16 are packed and 6 halvings do. Bump maps a shader
reads as a specular mask, or that are also used another way, are left
as they are; a shader that alpha-tests through one keeps its see-through
pixels. "Lightmaps" scales the map's lightmaps (its baked lighting);
"Max 256" is like stock maps and the default. 512 looks sharper but costs
about three times the memory, which big outdoor maps may not have to spare.

Cut-out bump maps: an environment shader with "alpha tested" cuts its
holes with its bump map's alpha, not its base map's (the Xbox, Halo PC and
MCC alike: its first pass draws with the bump map and tests its alpha),
so that bump map is the surface's shape: a grate's slots, a railing's
mesh. Those are capped by area rather than by side (as many pixels as a
512x512 texture, so a long strip keeps its detail along its length:
Deathtrap's "alpha test bump upscaled", 2048x512, becomes 1024x256, the
size of Bungie's own a10 "alpha test bump" on the Xbox), packed into P8
or DXT1 straight away when the level is over its budget (both keep every
see-through pixel), halved only after every other level texture is as
small as it goes, and kept as solid as at full size when they are. (They
used to be halved first with the other bump maps: Deathtrap's went down
to 128x32, which blurred its slots and mesh away, so its railings
vanished, its covers came out solid and its floors lost some of their
holes.)

Other Xbox fixes (automatic):
- Layered transparent shaders: shaders that stack extra layers on top of
  each other (MCC-era holograms, for example) hang the Xbox as soon as
  they come into view, so they're flattened to their base layer. They
  look simpler but work.
- Effects busier than Bungie's busiest make fewer particles (see
  "Effects" below).
- Shine and team colours on models: a model shader's multipurpose map
  packs its masks in the colour channels, and the PC and the Xbox read
  them from different ones. PC: auxiliary mask in red, self-illumination
  in green, reflection in blue, colour change in alpha. Xbox: reflection
  in red, self-illumination in green, colour change in blue, auxiliary in
  alpha. So a PC map's own vehicles, weapons and scenery showed their
  reflections and team colours in the wrong places on the Xbox, or not at
  all (Precipice's custom warthog, banshee and focus beam had no shine).
  Their multipurpose maps are rewritten in the Xbox's order; Bungie's PC
  textures rewritten this way match their Xbox originals channel by
  channel. A texture also used another way (a level's base map) gets a
  rewritten copy for the model shaders. MCC shaders flagged "use xbox
  multipurpose channel order" are already in it and stay as they are.
  Converting an Xbox map for Custom Edition does the reverse; for MCC its
  model shaders get that flag instead.
- MCC's compatibility flags: MCC added shader flags that make it draw a
  shader the way Custom Edition did, for maps made around Custom
  Edition's look. The Xbox doesn't read them, so a PC or MCC map's own
  shaders that set them are changed to look that way on the Xbox where
  they can be:
  - "Custom edition blending" on a multiply or double multiply
    transparent shader (dirt, scorch marks, tinted glass): Custom Edition
    faded these toward their own colour, the Xbox fades them out. Fully
    shown, Custom Edition's multiply leaves what's behind as it was and
    its double multiply draws the average of the shader's colour and
    white. The shader gets one more map stage that draws exactly that (a
    tiny white texture), so up close they look as in MCC; in fog they
    still fade the Xbox's way. A shader already using all four maps has
    no stage to spare and stays as it is.
  - "Use alternate normal type blending" on a level shader of the normal
    type: Custom Edition blended its two detail maps by the base map's
    alpha, the Xbox by the second detail map's. The shader becomes the
    Xbox's "blended" type, which blends them the same way (or "blended
    base specular" when both detail maps are opaque, which also keeps its
    shine the same).
  - "Use alternate bump attenuation": MCC lights bump maps so their flat
    parts are as bright as the lightmap; the Xbox's bump lighting darkens
    surfaces lit from a low angle. The Xbox has no way to do MCC's, so
    these are only counted in the log.

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
  - MCC scripts with parameters are rewritten to pass them through globals:
    (get_weapon index) becomes (set <its global> index) (get_weapon). The
    sets go just before the statement the call is in, so the call itself
    is only (get_weapon), wherever nothing between could change them (a
    call of the same script with something else, a script that calls it,
    a set of the variable passed, a sleep); else they go in a (begin ...)
    around the call. Each (begin ...) is a call more, and a level more of
    the script's stack (see "How deep scripts go" below): Inferno's maps
    pass parameters three calls deep, and with a begin at each level their
    main script went past the end of its stack.
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
  - A short (a whole number up to 32,767) given the game's time,
    (game_time), goes negative 32,768 ticks (18 min 12 s) into a game and
    stays wrong until 36 minutes: Deathtrap's (modulo (game_time) 1800),
    whose parameters are shorts, stopped its lightning and threw its
    warthogs about from then on. Every short global and short script
    parameter set from the game's time, or from another one that holds
    it, becomes a long (good for half a year of game time). Shorts and
    longs convert to each other wherever a function wants one.
  - Anything else becomes a stand-in that does nothing: calls with no
    result (like sv_say, which posts a chat message on PC servers) are
    removed, true/false checks return false, numbers return 0, and
    distance checks return "far away".
  - Xbox: a continuous script that only tests players against trigger
    volumes, each one every tick (Deathtrap's kill volume: 16 players'
    tests, 30 times a second, a good part of its 0.6 ms of script time a
    frame), first waits until any player is in one of its volumes, with
    (sleep_until (volume_test_objects <volume> (players)) 5): one test
    every 5 ticks while nobody is in it, the player-by-player tests only
    once someone is. Halo's scripts have no loops (no for or while; a
    continuous script is the loop, run once a tick), so this is the cheap
    way: the most a player in the volume waits is a sixth of a second.
  - Xbox: lookups. With no arrays, scripts keep 16 players' things in 16
    globals and pick one with an if for each:
    (if (= index 0) (set r ref_player0)) (if (= index 1) ...) ... Every
    test runs every time (Inferno's maps do this hundreds of times a tick:
    most of their script time). Written as else-ifs instead,
    (if (= index 0) a (if (= index 1) b ...)), each if sits inside the
    last, a level of the stack each: Digsite's 30-minute announcer is 30
    deep, past the end of its stack from minute 14. Either way, six or
    more such tests on one variable (four, for else-ifs) become a tree that
    tests halves first: (if (< index 8) <0 to 7> <8 to 15>), and the same
    again inside, with the tests for each value at the end. A 16-way
    lookup makes 6 tests instead of 16; 30 else-ifs go 7 levels deep
    instead of 30. Ifs one after another are only changed where nothing
    they run changes the variable or sleeps (else-ifs run one branch
    anyway), and each shape is kept only if no script then goes deeper
    than 480 of its 512 bytes (else a shallower shape, or as it was):
    lookups called from deep inside other calls stay as they are.
The log lists every change as [scripts] lines with how many times it
happened, so you can check that nothing important was lost.

How deep scripts go: Halo (the Xbox and PC alike) runs each script on a
stack of 512 bytes. Each call in progress takes 16 to 34 of them (an if 28,
a begin 24, a call of a static script 20, an ordinary function 22 and 4 more
for each argument), and the calls in its arguments go on top: a script
calling a script calling a script, each in an if, adds up. A script that
would go past the end is stopped right there, for good (OpenCE's debug log
says "stack overflow"), so whatever it does stops working. Bungie's own
levels go at most 410 deep. The log warns about any script that can still
go past 512
after the changes above ("[scripts] ... can go 530 bytes deep in calls"),
with the scripts its deepest calls go through: shorten that chain of calls
in the scripts window.

What the scripts cost a tick (Xbox, in the log and the scripts window):
"[scripts] script work each tick on the Xbox: about 2.3 ms of a tick's 33
(3.1 ms as the map made them)", then the heaviest continuous scripts with
what each does a tick (the scripts they call and the heavy functions, with
how many times). It's an estimate from the scripts' text: a call counts 1
(about 0.0036 ms on the Xbox, measured with Deathtrap's), heavier ones more
(placing AI, creating objects, volume tests, sounds); a continuous script's
statements count once a round of its sleeps, a sleep_until's test once a
period; an if's branches count half each when it has an else (as a lookup's
tree does) and not at all when it hasn't (things usually aren't happening).
"Up to" is with everything they test for happening at once. Scripts that
stop at once unless an object is there (a network trick's) are counted
apart. A map's script time adds to the Xbox's frame time: past a few ms it's
worth a look in the scripts window.

The scripts window ("Scripts...", top right of the Xbox tab): the map's
scripts to read and edit, for the map "Map" shows (the Convert tab's map
or the Build tab's scenario).
  - "Show" picks what the big box shows: your scripts (edit them there),
    "As the Xbox gets them" (the same, converted the way the Xbox build
    converts them, with the list of changes below it, and what they cost a
    tick and any that go too deep for the stack; read-only), or "The
    map's own" (as Invader gets them back out of the map; read-only). A
    map's own scripts are read in a second or two (only its scenario tag is
    taken out of the map); a Build tab scenario's are its data folder's
    .hsc files, or its tag's when it has none.
  - Save keeps your version for this map, in the app's folder
    (%LOCALAPPDATA%\HaloMapPorter\scripts, "Open folder" shows it). From
    then on every conversion or build of the map, for any game, compiles
    your version instead of the map's own, converted for that game as the
    map's are. Saving text that's the same as the map's keeps nothing.
    "Back to the map's" throws your version away.
  - Convert after editing doesn't prepare the map again: the tags Unpack
    (or a conversion) prepared get your scripts as they are now, and the
    map is built from them (the same map a conversion from scratch makes).
    The LODs window's "Build the map" does the same.
  - A map without scripts can be given some: write them in the box (it
    starts as an empty scripts.hsc) and Save.
  - Check (and Save) say what stops the scripts being read at all, with
    the line: a ( never closed, a ) too many, a " not closed on its line,
    something outside any (script ...) or (global ...). The build itself
    checks them against the map: a trigger volume, object, flag or tag
    the map doesn't have, or a wrong argument, stops it with the file,
    line and the line's text in the log.
  - Names in a script (kill_volume, hog1, a cutscene flag, an AI
    encounter) are the names they have in the scenario, given in Sapien
    (trigger volumes: Game data > Trigger volumes) and shown in Guerilla.
  - Keys: Ctrl+F find, F3 find the next, Ctrl+G go to a line, Ctrl+S save,
    Tab indents (4 spaces).

Old network tricks (Xbox, noted in the log and the scripts window): Halo
PC's netcode sends its clients the host's units and items, but not what the
host's scripts do with devices, nor which console is the host, so maps made
for it signal with units placed out of play. A vehicle no client has tells
a script it runs on the host ("does it exist": unit_get_health is -1 for
none); the host teleports a vehicle onto a biped, and every console's
scripts wait for that biped's death, which is sent. The original Xbox's
system link runs the same game on every console, and OpenCE places a map's
objects on every console (each one counts as the host), so on the Xbox
these tricks do nothing. The log says which units are only there for one
and what the scripts do with them ("[scripts] network trick, potential
saving"): the scripts can do without them (rewrite them in the scripts
window) and the units can go. Precipice (Custom Edition and MCC): 3
Warthogs and 2 cyborgs under the map (z -165) sync its purple beam and
pick the host; its beam can run from the game's clock instead (the same
on every console), as Deathtrap's warthogs do. Dogs of War: a Warthog in
a far corner picks the host. A game type trick (bipeds out of play that a
gametype-only spawn kills, so the scripts know the game type: Coldsnap's
ctf, slayer and race) is noted too, but it stays: Halo 1's scripts can't
ask the game type, and it works on every console.

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
  100/50/25/10/0 pixels; super low stays 0, or what the model has, so
  nothing new disappears). A model with only a super low size counts as
  having no switch distances: it still always draws its most detailed
  version (see "Tiny scenery" for what happens to that size). The log
  lists them as [lod] lines. First-person models are left alone: the
  engine asks them for their most detailed version by drawing them as 0
  pixels across, so switch distances would make them use their least
  detailed one.
  "Also make and use LODs for Bungie's models" (Xbox tab, off by default)
  does the same for stock models: 36 of the Xbox's models ship with
  lower-detail versions and no switch distances (also the multiplayer
  Scorpion). It also makes lower-detail versions for Bungie's models that
  have none, as for the map's own (see "Making lower-detail versions"
  below), on edited copies (your Xbox tags stay as they are). Faster on
  maps full of scenery, but those models then switch detail where they
  never did on retail.

  Heavy models ("Heavy models", Xbox tab)
  Some custom models are far denser than anything the Xbox shipped with:
  Precipice's beam cannon has 15,327 triangles in the world and as many
  in first person, where the Xbox's heaviest stock first-person weapon
  has about 2,200 and its warthog about 2,800. Every triangle costs tag
  memory and drawing time, and lower-detail versions don't help up close.
  So before any lower-detail versions are made, the app trims the most
  detailed version of the map's own heavy models: it takes out the
  vertices that change the shape least, keeping every other vertex
  exactly as it was. Texture seams stay exactly where they are (vertices
  can only slide along them), every hard edge left is as hard as made
  (a model of flat facets, like a mech's armor plates, can lose triangles
  too), two-sided surfaces (glass, fins) stay as made, and a change in
  texture coordinates or shading counts like a change in shape, so a
  texture can't slide across a surface.
  Bones: every vertex keeps the bones that move it and how much, and a
  vertex is only ever taken out into one moved exactly the same way. So
  the triangles left either move as one piece or are the model's own
  (where a joint bends), and a character or a vehicle with moving parts
  bends as before in every pose, off by no more than at rest.
  Layers: a part drawn over another with the same triangles (a
  character's shield shimmer is its whole body again with the shield's
  shader; a weapon's hologram over its gauge) gets exactly the triangles
  the part below keeps.
    Leave them as made   Nothing trimmed.
    Trim (they look the same)   The default. The shape moves at most
                         0.25% of the model's size (characters 0.18%:
                         fingers thin out first; first-person weapons
                         0.12%, seen closest) and never more than 0.006
                         world units (about 2 cm).
    Trim harder (details change)   0.6% (characters 0.4%, first person
                         0.25%), at most 0.015 units: much lighter, but
                         small details (a seat, a bumper bar, fingers)
                         can lose their shape up close.
    Trim to 1.5x Bungie's density   The lightest. Each model aims at 1.5
                         times the triangles Bungie's own models of its
                         kind and size have (below), when it's over that
                         by more than a quarter. The shape moves at most
                         1% of the model's size and never more than 0.03
                         world units (about 9 cm), so a model that can't
                         get there without visibly changing stops short.
  The first three trim only models over their kind's budget, and never
  below it: vehicles and scenery 4,000 triangles, characters and
  first-person weapons 3,000, weapons and items 2,000. Stock models and
  the models in skip.txt are left alone, and a model's own choice in the
  LODs window comes first (see "Seeing them" below). A model whose
  triangles all show (a mech made of hundreds of separately textured
  panels) can't lose many, and the log says so. Lower levels a model
  already had that are now as detailed as its trimmed top draw the top
  instead, and versions nothing draws any more are dropped (more memory
  saved). The lower-detail versions below are then made from the trimmed
  version. Trimmed versions are kept in the LOD library (.top files) and
  reused. The log's [heavy] line names each model with its triangles as
  made > now, and how much tag memory that saved; in the LODs window, "As
  the map made it" swaps the original back in, to compare.
  First-person weapons: a first-person model is drawn as the game draws
  it in first person, posed by every frame of its weapon's first-person
  animations (idle counted most, then firing; each overlay, like turning
  and moving, on the idle pose), through a view wider than any field of
  view (110 x 80 degrees), its opaque parts hiding what's behind them.
  The triangles that are never on screen (the back of a rocket tube, the
  side of a stock off the edge of the screen, insides) are taken out,
  even from a model within its budget, and then the ones seen least are
  simplified first (the side facing you is kept closest to its shape).
  In a model within its budget, they only go when they're at least 5%
  of it; a triangle next to one that's seen counts as seen. A
  first-person model that's also its weapon's model in the world (some
  tagsets draw one model for both) is left whole: it's seen from every
  side there. The [heavy] line says how many triangles were never on
  screen. Measurements are kept with the app's other results and reused.
  Precipice's CMT weapons (with "Use the Xbox's own weapons, vehicles and
  player model" off): the rocket launcher's 7,298 > 3,032 (623 never on
  screen), the shotgun's 6,920 > 2,999 (794), the plasma rifle's 8,510 >
  4,359 (522).
  Precipice, as made > trimmed (harder; Bungie's density): beam cannon
  15,327 > 5,156 (2,548; 1,729), its first-person model 15,331 > 5,321
  (3,455; 3,259; 1,414 never on screen), the damaged chaingun warthog
  8,580 > 5,435 (3,994; 1,973), and with "Use the Xbox's own weapons,
  vehicles and player model" off, its player model 7,483 > 6,543 (4,529;
  3,000), which then gets lower-detail versions too. Tag memory saved:
  681 KB (875 KB; 1.0 MB).
  Bungie's density: every stock model's most detailed triangles against
  its size (the diagonal of the box around it). Volume doesn't predict
  them: a sniper rifle is about five times a pistol's length and has
  less than twice its triangles. Bungie gave each kind a budget that
  grows slowly with size, so the targets (1.5x) are:
    first-person weapons   about 2,250 for a pistol, 2,800 for a rifle,
                           3,300 for a sniper rifle (Bungie's: 1,030 to
                           2,259)
    weapons and items      about 1,150, 1,350 and 1,550 (Bungie's: 566 to
                           1,468)
    characters             3,000 (Bungie's main ones about 2,000,
                           whatever their size)
    vehicles               Bungie's vary 3x at one size (the Ghost 1,034,
                           the Warthog 2,798), so the line through the
                           most detailed, the Warthog and the Pelican: a
                           Warthog-sized one about 4,200
    scenery and devices    the line three in four of Bungie's are under:
                           a crate about 750, a big tree about 1,500
  The LODs window shows each model's ("1.5x Bungie").
  First-person arms (the map's own with "Use the Xbox's own weapons,
  vehicles and player model" unticked, see below) are left as made.

  Collision ("Heavy models", Xbox tab)
  Bungie's own first. Most custom vehicles, and the wrecks made from
  them, are iterations of a stock one: the same skeleton, about the same
  size. So each part (node) of a custom collision model whose name a
  stock vehicle's collision also has (the Warthog, Ghost, Scorpion,
  Banshee, Wraith and Shade, from your Xbox tags) takes Bungie's
  collision for it, where that sits on the custom model's drawn
  triangles of that part as well as it sits on Bungie's own model: the
  collision's distance to what's drawn and what's drawn to the
  collision, half and nine tenths of each, no more than a quarter (or
  2 cm) further. Each surface keeps the material of the custom part's
  nearest own surface (so its damage and hit effects stay), and parts
  that don't fit, that the stock vehicle doesn't have, or with breakable
  surfaces keep their own. Only when that's lighter. Coldsnap's
  swarthog: 13 parts, 486 surfaces now 167. Precipice's damaged chaingun
  Warthog (scenery): the hull and three suspensions, 390 surfaces now
  65. Deathtrap's destroyed Banshee is bent out of the Banshee's shape
  (its struts splay), so it keeps its own.
  Then dense collision. A biped's movement gathers every surface, edge
  and corner of the collision within its reach (about 1.5 m), and checks
  each one against all those it has gathered so far, so a dense
  collision model costs it the square of its detail. Bungie's densest
  multiplayer scenery (a rock, the small barricade) keeps that work
  around 115,000 steps; Deathtrap's pilot chair (416 surfaces, all
  within reach at once) took 1.2 million, ten times as much, and each
  biped near it ran several times slower. So the collision of the map's
  own scenery and devices that cost more than Bungie's densest is made
  again, the way Bungie made collision for their vehicles (the Warthog's
  is 202 surfaces for a 2,800-triangle model): a few convex pieces with
  big flat faces on the model's own planes, sitting just outside it;
  hollows, gaps and small details filled over (the Warthog's cockpit is
  a flat floor); nothing thinner than about 4.5 cm, since a thin
  collider lets a vehicle slice into it and jitter. The pieces are found
  by cutting the shape wherever its outline would fill the biggest gap,
  the worst first, and it keeps as many pieces as stay within the
  budget. When the same surfaces in a leaner BSP are enough, only the
  BSP is made again (the shape exactly as made). Every new BSP is
  checked with the engine's own ray test against its surfaces.
    Leave them as made   Collision left as made.
    Trim (they look the same)   The default: made again when it costs
                         more than about 130,000 steps.
    Trim harder (details change)   ... more than 90,000.
  Made again: only scenery and machines (what bipeds walk into and
  vehicles drive into: vehicles meet bipeds and each other as their
  physics spheres, not their collision models, and controls aren't
  walked into), never a collision model a vehicle also uses, and never
  one with two-sided or breakable surfaces. Neither step touches a
  collision model a character, weapon or item uses (their hit boxes),
  the Xbox's own, or the level's own (BSP) collision, which matches what
  you see exactly. The log's [collision] line names each model with its
  surfaces before and after, the work as a multiple of Bungie's densest,
  and how far the new surfaces lie from the old (most of that is hollows
  filled over); "LODs & collision" draws each one as made and as converted
  (see "Seeing them" below).
  Deathtrap: the pilot chair 416 surfaces > 86 in 9 pieces (10.6x >
  1.0x); the destroyed Banshee 629 > 331 (3.9x > 1.1x).

  Animations ("Animations", Xbox tab)
  Animations take tag memory, the Xbox's hard limit (22 MB on every Xbox,
  modded or not). Stored frame by frame, a bone that turns takes 8 bytes
  for every frame of the animation, even where it barely moves. The Xbox
  can also play animations stored as keyframes ("compressed"): each bone
  keeps only the frames where its motion changes, and the engine blends
  in between. Bungie stored the Xbox's cinematics that way. The app
  stores the map's animations that way too, its own and the Xbox's it
  uses (as copies; your Xbox tagset is left alone).
    Leave them as made   Nothing changed.
    Compress (they move the same)   The default. Every bone stays within
                         0.05 degrees and 0.0001 world units (0.3 mm) of
                         each frame as made, about what the format's own
                         rounding moves it anyway (its rotations keep 12
                         bits a number). First-person animations come out
                         about half their size, characters about a
                         quarter smaller.
    Compress harder (fast moves drift)   0.2 degrees / 0.0004 units:
                         another 10-15% smaller; a fast move (a melee
                         swing) can be a few millimetres off.
  Each animation is also checked as a whole: its bones are posed down the
  skeleton in every frame, and if any bone's end would land further from
  where it was made than the rounding alone puts it (plus 0.3 mm; harder:
  2 mm), tighter tolerances are tried, and an animation that still can't
  is left as made. The tags keep their frames as made too, with the
  keyframes after them (as Bungie's tools save them); the map keeps only
  the keyframes. Animations already stored as keyframes (some of an MCC
  tagset's are) are left as they are.
  Overlays are never stored as keyframes: the engine plays some of them by
  a value rather than frame by frame (a vehicle's velocity, steering and
  suspension, a rider's acceleration: the frame comes from how fast it's
  going), and for a compressed one it blends the turns only at whole
  frames, so the pose snapped from one frame to the next as the value
  moved (a hovering Ghost jittered between its leaning poses while it sat
  still). Devices are played the same way, by their position and power
  (Deathtrap's wheels, a 60-frame turn over an 8-second loop, moved a
  frame every 4 of the game's when compressed), and so are an object's
  animations its functions drive: whatever type those animations are,
  they're kept as overlays are. A compressed overlay whose frames are
  still in the tag (an MCC tagset's) gets them back, whatever the
  setting. The log's [anim] line gives the tag
  memory before and after, and the farthest any bone end moved.
  Blood Gulch (the Xbox's own animations): tag memory 8.1 MB > 7.6 MB
  (harder 7.5 MB). Precipice with its own weapons: 1.1 MB of animations >
  674 KB. Posing a bone from keyframes takes the Xbox a little more work
  than reading a stored frame (finding its two keyframes and blending
  them).

  Making lower-detail versions
  Many custom models have only one version, so the Xbox draws all of
  their triangles however far away they are. "Make lower-detail versions
  (LODs) when a model has none" (Xbox tab, on by default) makes them for
  the map's own models with 1,000 triangles or more, or 300 or more and
  5,000 or more on the map in all (a tree placed many times): a medium
  version with about half the triangles (used from the "high" switch
  distance down) and a low one with about a fifth (from "medium" down).
  It takes out the vertices that change the shape least and keeps every
  other vertex exactly as it was (position, texture coordinates, normal,
  bones), the edges where one shader's part meets another (no cracks),
  and the edges between pieces moved by different bones (a wheel stays
  separate from the body). Each version may move the shape about 1.5
  pixels where it's first drawn (the model that many pixels across at its
  switch distance), and never less than 1% (medium) and 2.5% (low) of
  the model's size: scenery 1.7% and 5%, vehicles 1.5% and 3%, weapons
  and items 3% and 6%, characters 1% and 2.5%. A version is only added
  when it has at least a tenth fewer triangles than the one above it,
  and a model with no switch distances gets the stock-style ones above.
  Variants that draw another variant at a distance (Precipice's tall
  pine has four shapes whose lower levels all drew the first) get lower
  levels of their own, so each keeps its shape.
  Characters get them too: as when trimming (see Bones above), every
  vertex keeps its bones, so they bend as before at every distance.
  Cut-out cards (leaves, boughs, grates, fences: parts whose shader cuts
  holes with its texture's alpha) stay as made in every version: a
  card's corners can't move without cutting off what's drawn on it, so a
  tree's leaves would thin out or lose their tops in the distance. The
  rest of the model (a trunk) is simplified as usual, and the model only
  gets lower-detail versions when its lowest is still a tenth lighter
  than as made in all.
  Left alone: first-person models, models whose shape needs most of their
  triangles, models that are mostly cut-out cards, the models listed in
  skip.txt (below), and stock models unless "Also make and use LODs for
  Bungie's models" is ticked. Deathtrap: tree_big 2,479 > 2,200 > 1,953
  (its trunk; 8 on the map), tree_leafycover_mid left as made. Precipice
  with Bungie's: tree_pine 522 > 441 (17 on the map) and tree_pine_tall
  left as made, with Bungie's rocket launcher and monitor.
  A model's own choice in the LODs window comes first: made whatever the
  Xbox tab says and however few triangles it has, aiming at the medium
  and low triangles you give (allowed to move the shape up to 5% and 10%
  of its size to get there), or not made. The [lod] lines in the log name
  each model with its triangles, most detailed > medium > low, and how
  many are on the map.
  The versions cost memory: on the Xbox, a model's vertices and triangles
  are part of the 22 MB of tag memory (and of the map file), and the
  [lod] line says how much they add (about 0.5 MB for three big models).
  If the map then needs more tag memory than the Xbox has, or is over the
  size limit by less than the versions take, the app takes them out again
  and rebuilds, and the log says so. The weapons & vehicles window
  measures maps without them.

  Seeing them: after an Xbox conversion, "LODs & collision" (next to
  "Open report") lists the map's own models, changed or not, Bungie's
  that have no lower-detail versions, and stock models whose switch
  distances or collision it changed: what changed,
  their triangles by level, 1.5x what Bungie's models of their kind and
  size have, how many are on the map and your choice for them. "Show"
  narrows it to the models made without lower-detail versions, or to
  those this conversion changed; click a column's title to sort by it
  (heaviest first to start). Click one to see each level ("Most detailed",
  "Medium", "Low"...: drag to turn it, wheel to zoom; "Show the triangles"
  draws their edges) and roughly how far away the game switches to it, in
  metres and feet (on a 4:3 screen; a model's size on screen decides it).
  For a trimmed model, "As the map made it" swaps its most detailed level
  for the original, to compare. For a model whose collision changed, "Its
  collision" and "Collision as made" draw its collision as converted and
  as the map made it, each part where the model's rest pose puts it,
  coloured by material (the same colours in both), each surface outlined;
  the view stays where you turned it, to flip between them. Versions
  you're happy with need nothing: they're kept in the LOD library and
  reused every time.
  Below the list, what every conversion does with the model shown from
  now on, on this map or any other that uses it (kept in choices.txt in
  the LOD library; saved as you change it):
    Lower-detail versions   As the Xbox tab says; Make them, aiming at a
                         medium and a low number of triangles (empty:
                         about half and a fifth), whatever the Xbox tab
                         says and however few triangles it has; or Don't
                         make them (drawn in full at every distance; a
                         model with lower-detail versions of its own,
                         which the app always keeps, then gets no switch
                         distances either).
    Most detailed version   As "Heavy models" says; Keep it as made; or
                         Trim it to a number of triangles (it starts at
                         1.5x Bungie's; the shape may move up to 5% of
                         the model's size to get there).
  "Make it again" makes the model shown again with its choice (from the
  model as the map made it) and draws its levels in the window, in a
  moment and without building the map: change the triangles until it
  looks right.
  "Update the map" puts every model's choice in the map you have, without
  converting it again: the conversion's prepared tags are kept (in the
  cache folder's work\convert, until the next conversion), so only the
  models' levels of detail and trims are done again (with the Xbox tab's "Heavy models" and
  LOD options as they are now) and the map is built again: the time of
  the build, a few seconds; the list then shows the map as updated, and
  the report gets what changed. The rest of the map stays as converted
  (to change anything else, convert it again). The result is the same
  map a conversion with those choices makes. First-person models only
  have the second choice; stock models only the first. It also works
  after a map that didn't fit: the versions made were taken out again,
  and the list says so; an update that doesn't fit leaves the map you
  have as it was.
  "LODs..." (Xbox tab, beside "Level...") opens the window before any map
  is built, for the map the Xbox tab's "Map" names (see "Which map the
  Xbox tab's windows are for"): on the last conversion or Unpack of it with
  the Xbox tab's weapon and vehicle options as they are (its models done
  again first, in seconds, when the "Heavy models" or LOD options changed),
  or else the map is prepared as far as the window (the tags, the models'
  levels of detail and trims; no build, so it takes about a third of a
  conversion) and the window opens on that. Its button then says "Build
  the map": the first build, from the tags prepared, with every model's
  choice. (Precipice: 28 s prepared, a model made again in under a
  second, built in 60 s, the same map a 2-minute conversion makes.)
  The map goes where the Convert tab's Save as says (or the Build tab's
  Xbox folder); a Convert tab set to another game is switched to Xbox
  first, with the Xbox Save as, and the log says so.

  The LOD library
  Every version the app makes is kept in the LOD library folder (Settings
  tab; %LOCALAPPDATA%\HaloMapPorter\lod_library unless you change it), so
  the same model in another map, or the same map converted again, reuses
  them instead of making them again. In the folder:
    *.lod       The versions of one model's geometry, named after it (an
                edited model gets new ones).
    *.top       A trimmed most detailed version of one geometry (see
                "Heavy models"; each setting has its own).
    index.txt   One line per model added: date, tag path, triangles (most
                detailed > medium > low, or "trimmed" as made > now) and
                its files.
    choices.txt What to do with a model, chosen in the LODs window: one
                line per model, tab separated: its tag path; lower-detail
                versions (auto, none, or make and the medium and low
                triangles, 0 for automatic); its most detailed version
                (auto, as made, or trim and the triangles); a note.
                Delete a line to go back to automatic.
    skip.txt    Models to leave alone (not trimmed, no lower-detail
                versions made, no switch distances given), one per line: a
                tag path (vehicles\jet\jet.model) or a name (jet: the
                model's or its object's). Lines starting with # are
                ignored. Write it yourself; a model's line in choices.txt
                comes first.
  Deleting .lod or .top files (or the whole folder) has them made again
  next time.
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

Which map the Xbox tab's windows are for ("Map", Xbox tab)
  The Scripts, Weapons & vehicles, Level and LODs windows work on one map:
  the Convert tab's map or the Build tab's scenario, whichever "Map" at the
  top of the Xbox tab shows. The rest of the Xbox tab applies to both tabs.
  "Map" follows what you do, not which tab you last looked at (clicking
  through the tabs changes nothing): picking a map on the Convert tab, or
  converting one for the Xbox, makes it the Convert tab's; picking a
  scenario on the Build tab, or building with Xbox ticked, makes it the
  Build tab's. Choose either in the list yourself at any time; the app
  remembers it. When the tab chosen has no map, the other tab's is used.
  What you pick in the windows is saved per map (or scenario) and used
  whenever that one is converted or built for the Xbox, so switching back
  and forth loses nothing.

Weapons & vehicles window (Xbox)
  The "Weapons & vehicles..." button on the Xbox tab lists every weapon and
  vehicle a player can get on the map (the one "Map" shows: the Convert
  tab's map, or the Build tab's scenario), with:
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
  builds a test copy, then reads back what each part takes), unless the map
  was converted or unpacked (or prepared for the LODs window) with the Xbox
  tab's weapon and vehicle options as they are and no picks: then it's
  measured from the tags that prepared (the models done again with the
  "Heavy models" and LOD options now), in seconds (Precipice: 8 s instead
  of a minute). Unpack measures it on the way. A conversion for
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

Level reflections, fog, scenery, weather and more: the "Level..." window (Xbox)
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
  and the level looked noticeably flatter. Measured with Halo 1 Unlimited's
  perf log, a reflection costs the Xbox's processor too, not only its
  graphics chip (the level is sent again): dropping Deathtrap's took about
  1.4 ms off each frame on average and 3.2 ms in its busiest view, of the
  33 ms a frame has, about 0.45 ms per 1,000 triangle passes. The window's
  line under the picture estimates that for the level. So a conversion keeps every
  reflection unless you choose otherwise for that map in this window. When
  a level's reflections cost much more than Sidewinder's, the Performance
  section says so.
  Dropping a reflection sets its brightnesses to 0 and removes its cube map
  (freeing its memory too); the surface keeps its textures, bump map and
  lightmap. The log's [reflections] line lists the ones dropped.
  The "Level..." button on the Xbox tab shows this for the map "Map" shows
  (the Convert tab's map or the Build tab's scenario), with the level's fog, sky, effects, tiny
  scenery and weather (see "Fog", "Sky", "Effects", "Tiny scenery" and
  "Weather" below).
  Measuring prepares the map as for a conversion without building it
  (seconds to a minute), unless a conversion or Unpack (Convert tab) of the
  map measured it already: then the window opens on that.
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
    Alpha-tested surfaces (grates, railings, treelines) are cut out by
    their bump map's alpha, as the game cuts them, so a material whose bump
    map and color map don't line up shows that here too.
  - "Objects" (on by default): the objects in the level when a game
    starts, where they are: the scenery, machines, controls, light
    fixtures, vehicles (ones a multiplayer game type spawns), weapons and
    equipment the map places, and an item on each multiplayer spawn point
    (its item collection's first). Each draws its model's most detailed
    version with its shaders' base maps: alpha-tested ones cut out by the
    base map's alpha, see-through ones blended as their shader says (added,
    multiplied, or by their alpha). They're lit the way the engine lights
    an object: it looks straight down from the object's middle and from
    four points around it to the level below, takes the lightmap's colour
    there, and lights the object with part of it all round, the rest from
    the direction the lightmap's light came from at that spot, and a little
    bounced up off the surface below in its colour. An object with nothing
    lit within 10 units below it gets the level's default lighting, as in
    the game. Not shown: their detail, reflection and self-illumination
    maps, change colours, animations and shadows; markers and effects
    (lights, sparks, smoke) only as their models are.
  - Double-click a surface in the picture to find its shader: one in the
    list is selected there (the view stays where it is); one that doesn't
    reflect is tinted and named in the line above the list, as is a
    see-through one. The line also says when it's alpha-tested.
    Double-click an object to name it, what it is, and the shader of the
    part you clicked (and whether it's alpha-tested: cut out where its base
    map's alpha is under half, which is how a model's shader is drawn
    unless it says it isn't).
  - "Showcase flythrough...": the camera shots for Halo 1 Unlimited's
    showcase screensaver, played and made in the picture (see "Showcase
    flythroughs" below).
  - "Portals": each portal outlined in yellow, glowing orange where
    you can see through it, and each room (cluster) tinted a colour of its
    own. The engine draws the room you're in, then only the rooms it can
    see through portals. The line above the list says how many rooms and
    portals the level has (after the app's own, when it adds them).
  - The line under the picture: the level's triangle passes in a typical /
    busy view as made and with your choices, next to Sidewinder's, and
    about how much of the Xbox's processor time a frame the reflections
    take (or dropping them saves).
  - Fog: as the conversion leaves it (automatic, the default), as the map
    has it, fully opaque at its distance, three quarters of it or half of
    it, or Other: from where it starts to where it's fully opaque, the two
    distances you type in the boxes beside it (in world units; 1 unit = 10
    feet, about 3 m). The boxes show the level's own distances (see "Fog"
    below). The picture shows it, and follows as you type (its sky is a
    plain gradient, not the level's own sky).
  - Hide scenery: never (the default), or below 4, 8, 12 or 16 pixels
    across. The picture draws every object however small.
  - Weather: as made (the default), lighter, half the drops, or off, each
    with the level's own number of drops. The picture doesn't show it.
  - Sky: drawn in fewer layers, looking the same (the default), or as made
    (see "Sky" below).
  - Effects: no busier than Bungie's (the default), lighter (looping ones
    keep a quarter of their particles), or as made (see "Effects" below).
    The picture doesn't show effects.
  - Riders: keep the AI of units the scripts sit on other objects (the
    default), or leave them without AI (see "Riders" below).
  - Props: keep the collision of small props whose collision is very
    detailed (the default), or leave it out (see "Detailed props" below).
  Hover over the choices and checkboxes for what each does. Choices are
  saved per map and used every time you convert it for the Xbox.
  Measurements are kept between runs of the app until the map file
  changes (or the portals option); "Measure again" starts over.

Fog (Xbox)
  Halo's fog (each sky's outdoor fog, and its indoor fog for rooms with no
  sky) thickens evenly with distance from the camera, the way most engines'
  linear fog does: none nearer than its start distance, then thicker and
  thicker up to its opaque distance, where it reaches its maximum density,
  and that density from there on. (With a start of 60 units and an opaque
  distance of 150, something 105 units away is half fogged.)
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
  less drawn, foggier), or fully opaque between two distances you type:
  where it starts (0: at the camera) and where it's fully opaque (nearer or
  farther than the map's). The start stays short of the opaque distance.
  The map's own skies only; Bungie's levels keep theirs.
  Where the end lands matters for what stands round the level: the fog is
  by distance in front of the camera, not distance away, and past its end
  the sky shows, not the fog's color. A treeline, cliffs or mountains that
  are partly nearer than the end and partly farther are cut off along a
  line, and the line moves as you turn (what's straight ahead is cut
  first; the same trees at the picture's sides are nearer in front of the
  camera, so they're drawn). Deathtrap's treeline cards
  (deathtrap_treeline_moving) stand 142 to 346 units from where players
  play: with the end at 140, their tops go when you look straight at them
  and come back as you turn. The Level window's status line says when the
  end runs through something like that, and how far to move it: under
  109 units the treeline is never drawn, past 357 it's always whole. The
  log's [fog] line says it too. (The level's own walls round players are
  cut wherever the fog ends; only what stands past them is named.)

Sky (Xbox, "Level..." window)
  The Xbox draws the sky first in every frame, over the whole view, before
  the level covers most of it: each of the sky model's parts is a layer of
  transparent texture blended over the ones before it, a screenful at a
  time, wherever you look. Bungie's multiplayer skies draw 0.8 to 1.4
  layers per pixel (Sidewinder 0.8, Blood Gulch 1.1, Carousel 1.4; their
  densest campaign sky 2.4). Precipice's draws 2.8: a dome, two cloud
  domes, a horizon haze band, sun rays, a planet, a ring and lightning,
  each its own layer. By default a conversion draws the map's own skies
  in fewer layers, so they look the same:
  - A still layer (none of its textures move) drawn just before another
    on the same dome (the same triangles seen from the same direction) is
    drawn in the other's pass: their textures are combined into new ones
    and the upper layer's moving clouds are kept, so one pass of up to
    four textures blends what the two did.
  - Still layers drawn after a dome, where the dome is in front of them
    (a haze band, sun rays), are drawn in its pass too; what's below the
    dome's rim stays as it was.
  - Layers' clear parts (the sky above a haze band, a planet's square's
    corners) are cut away.
  Precipice's sky draws 1.6 layers per pixel this way (3.3 looking up,
  4.7 before), its cloud domes, haze band and sun rays in one pass of
  four textures, with three new 512 x 512 textures (about 0.5 MB more of
  texture memory). Drawn in software from views all round, the two match
  to within a few levels of 255 (the rounding of the new textures'
  compression). The log's [sky] line says what was drawn together and the
  layers before and after; the Performance section gives the sky's layers
  next to Bungie's. "As made" keeps the sky's layers as they are (for
  comparing on the Xbox). Bungie's skies are left alone.

Effects (Xbox, "Level..." window)
  Every particle alive is moved and drawn each frame, its sprite blended
  over what's behind it, and the engine keeps at most 1,024 of them, every
  effect's together. What a frame pays for is what's alive at once: how
  many particles, and how much sprite (its radius squared, in square world
  units). Bungie's busiest Xbox multiplayer effects, measured from their
  tags:
  - Made once (a muzzle flash, an impact, an explosion), at its busiest
    moment: about 200 particles (a charged plasma bolt hitting stone) and
    188 square units of sprites (the rocket's explosion).
  - Attached to an object, so it loops while the object exists (a fire on
    a prop, a beam, a burning wreck), kept alive on average: about 300
    particles (the teleporters, a beam emitter) and 1,150 square units
    (the plasma flame on Bungie's scenery, the beam).
  An effect's cost includes the effects its parts make and those its
  particles make when they hit something or die. A looping effect only
  runs while its spot can be seen, so what counts is each one's own cost.
  By default a conversion leaves the map's own effects as made unless one
  is busier than those: it then makes just enough fewer particles to fit
  (each count scaled, at least one where there was one; each particle the
  same size as before), and the effects it makes alongside keep no more
  than it does. Precipice's are all within them but one (its green flame
  costs what Bungie's plasma flame does): kotick_laser_large_explosion
  draws 10 smoke sprites of radius 9 to 11 units that last 12 seconds
  (631 square units at once), so it keeps 30% of them (3). "Lighter" also
  has looping effects on objects keep a quarter of their particles (close
  to what earlier versions of the app did to all of them); "As made"
  leaves them all as they are. The log's [effects] line lists the effects
  changed and the share each keeps. Bungie's effects are left alone.

Tiny scenery (Xbox, "Level..." window)
  Halo 1 has no draw distance for objects, but it draws nothing of a model
  smaller on screen than its smallest switch size ("super low", its
  bounding sphere's pixels across). With "Hide scenery" set to a size,
  each scenery and device model the map places gets that raised to it, so
  only specks go: at 8 pixels a 1-unit prop goes about 57 units away, a
  16-unit tree not until about 900. Vehicles, characters, weapons and
  items are left alone. The log's [scenery] line lists them. Off by
  default.
  It's a size rather than a distance on purpose. The engine measures an
  object's size by how far ahead of the camera it is, not how far away:
  things at the edges of the screen count as nearer, so with a distance
  cutoff, big objects show at the screen's edge and vanish when you turn
  to face them. Bungie's own scenery never vanishes (they use cheaper
  versions far away, and fog); past fully opaque fog the engine already
  skips everything. Halo 1 Unlimited counts every model at half its size
  in widescreen (to pay for the wider view), so there things go at twice
  the size chosen.
  Some custom models set a super low size but no switch distances
  (Deathtrap's phone booth: 0/0/0/0/35). They only ever draw their most
  detailed version, so that size does nothing but make them vanish, and
  the Xbox's 480 lines make everything fewer pixels across than a PC's
  screen does: the booth went about 13 units away (6.5 in widescreen).
  On Xbox builds the app lowers such a size on the map's scenery and
  devices to the "Hide scenery" choice, or to 0 (drawn at any distance)
  when that's off, and lists them in the log's [scenery] lines. Models
  with switch distances of their own keep their sizes.

Weather (Xbox, "Level..." window)
  Rain and snow cost the Xbox's processor every frame, wherever you are,
  indoors too: every drop around the camera is moved, clipped against the
  level's rain-free volumes and drawn. Deathtrap's rain took 2.5 ms of
  each 33 ms frame. A weather type keeps its particle count (per cubic
  unit) x its fade-out end distance cubed drops in a box around you, and
  the engine keeps at most 512: Deathtrap's asks for about 3,600, so it
  always runs at 512. Bungie's heaviest rain (343 Guilty Spark) is about
  220 drops, ending 5 units away.
  - Lighter: just as dense around you as the engine draws it now, but
    ending 5 units away (fading out over the same share of the distance),
    so far fewer drops: Deathtrap's 512 become about 125. No change for
    weather that already ends within 5 units and isn't at the cap.
  - Half the drops: thinner everywhere (half of what the engine draws now,
    not of what the tag asks for).
  - Off: no rain or snow; the wind stays.
  Every weather system the level uses is changed (a copy of a stock one
  for this map). The log's [weather] line says what changed. As made by
  default.

Riders (Xbox, "Level..." window)
  Units the scripts give AI (ai_attach) and then attach to another object
  (objects_attach), like Deathtrap's marines sitting in its moving
  warthogs, can't go anywhere, but their AI still thinks every tick:
  Deathtrap's 9 took 1.5 ms of each Xbox tick, a tenth of its game time.
  "No AI" leaves their ai_attach calls out; they're still placed, attached
  and seated as before. Try the map once: a rider meant to shoot at players
  would stop. The log's [scripts] ai_attach line counts them.

Detailed props (Xbox, "Level..." window)
  Each tick a player's movement is tested against every surface of the
  collision models it's near. Small props ported with their whole mesh as
  collision make that expensive: standing by one of Deathtrap's pilot
  chairs (416 surfaces for a 1-unit object; a warthog's whole chassis has
  47) cost an Xbox 5-6 ms a tick. "No collision" leaves out the collision
  of scenery 1 unit or less across with 300 surfaces or more (Bungie's most
  detailed small prop, a10's oxygen tank, has 163): players and shots pass
  through them. The window names them; the log's [props] line too.

Lights (Xbox, "Level..." window)
  Dynamic lights on the objects the map places are always on: Deathtrap's
  three warthogs each carry a headlight (radius 3.5). Each one in view
  has the Xbox draw the level and the objects around it again, twice:
  once for its light (diffuse) and once for its shine (specular, which
  reaches further). Measured with Halo 1 Unlimited's perf log, Deathtrap's
  slowest views had two in view: those passes took about 3.4 ms of each
  frame (the shine 2.4 of it), and setting the lights up 1.2 ms more.
  - Keep them (as made, the default).
  - No specular: the light stays, without its shine: about 70% of its
    passes' cost goes.
  - Off: no light; a lens flare stays (Deathtrap's headlights still glow).
  The window lists the lights and how many of the map's objects carry
  one: attached to them, or made by an effect attached to them. The log's
  [lights] line names them.

Showcase flythroughs (Xbox, "Level..." window)
  Halo 1 Unlimited's showcase screensaver (planned: in place of the attract
  movie, the idle main menu loads a map and flies the camera through it
  while bots play) needs camera shots for each map. Each Xbox conversion
  into a map folder (the folder with the map's info.txt) writes them as
  showcase.txt beside the map, so a map maker can ship theirs with a map.
  Made from the level, unless you make your own, and kept to where
  players play: the places the scenario says players are (where they
  spawn, the item spawn points, every game type's flags, hills,
  teleporters and race points, the vehicles a game type spawns), and the
  floors near them, at most half a unit above one (so not the roofs and
  high ledges no one gets to):
  - circling its CTF flags and power items (rocket launchers, sniper
    rifles, fuel rods, flamethrowers, overshields, camouflage): a slow
    100-degree arc a few units off and above, looking at it;
  - along the routes its game types give players: from one CTF flag to
    the other, between two King of the Hill hills (the two farthest apart
    the camera can fly between), between two oddball spawns, and round
    the race's points in order (its first 35 seconds on a long race). The
    camera flies through open air a couple of units over the ground,
    round the walls in its way (a route found over every floor of the
    area players play in), looking ahead along the way as far as it's in
    view, and over the last few units at where it's going. Routes that
    would only stare at walls are left out, as are places only a
    teleporter reaches (Precipice's flags);
  - the vehicles a game type spawns, where two or more stand together:
    beside them, moving along the line they stand in (Blood Gulch's
    Scorpions), or circling them when they're in a bunch;
  - flying over the level looking straight down, the camera moving
    sideways across where players play: as high as the level's air goes
    (up to 36 units over them), only where nothing is over the camera and
    the whole picture is the level (no sky, nothing past its edge), and
    over ground about as far below as it's high rather than the bottom of
    a pit;
  - looking down on a flag, a power item or the middle of where players
    are, from 5 to 10 units above and 7 to 18 off to the side, moving a
    little way round it (these may be outside where players can go);
  - gliding between two places players stand that see each other;
  - down the longest sight lines players have: from one place players
    are towards another far off that it sees, the camera drifting
    towards it;
  - rising over open ground near the middle, tilting down to take the
    level in.
  Every shot is checked every quarter of a second: the camera stays
  inside the level, clear of every surface (0.6 units; gliding and sight
  lines 0.35) and outside the boxes around the map's placed objects
  (scenery, machines, controls, light fixtures: each model's box, turned
  as placed), never passes through a wall between two checks, and sees
  what it circles at the start, the end and most of the way, through
  nothing (glass included). Apart from looking down, it stays in the box
  around where players are (a few units past it across, from their feet
  to a few units over their heads). On a small map the shots go closer
  together, and gliding and rising shots must look out at least 6 and 8
  units, not into a wall. Deathtrap gets 12 shots (110 seconds),
  Precipice 16 (222), Blood Gulch 16 (212). The log's [showcase] line
  says what was written; a showcase.txt in the map folder that the app
  didn't write (its first line doesn't name Halo Map Porter) is kept.
  "Showcase flythrough..." (top right of the Level window) swaps the list
  for the shots: select one to go to its start and see its path (cyan),
  its keys (yellow) and what it looks at (magenta); grey boxes are the
  ones around the map's placed objects, which the camera keeps clear of. "Play them all" and "Play
  this shot" fly the picture through them (click the picture or Stop to
  stop). To make your own: fly to where a shot starts, "New shot here",
  fly on and "Add key here" at each next spot (keys 3 seconds apart;
  Seconds makes the shot slower or faster, keeping its path); tick "Keep
  looking at what's in the middle of the picture" while aiming at
  something to keep it in view. Name: the shot's name (31 characters).
  - Keys: the arrows pick one of the selected shot's keys (orange in the
    picture; or double-click a yellow key in the picture) and take the
    picture there; "Go to it" goes back to it. Fly somewhere and "Move it
    here" to move it (it keeps its time in the shot), "Add key here" to
    put a new one after it (halfway to the next key, or 3 seconds after
    the last), "Remove it" to take it out (without its first key the shot
    starts at the next one).
  - Orbit: "New orbit here" makes a shot circling what's in the middle of
    the picture (a surface or an object), looking at it, passing your
    side of it halfway through (it starts half the turn before, and the
    picture stays where you were): radius units from it across, height
    units above it, turning that many degrees (360 a whole circle;
    negative: clockwise), in 12 seconds (Seconds changes it). With an
    orbit selected, the boxes change it (and its Seconds) round the same
    middle; moving its keys one by one makes it a shot of keys of its
    own.
  - A shot you make or change is checked as the made ones are, and the
    line above the list says when the camera goes through a surface,
    leaves the level, or loses sight of what it looks at.
  Delete, move up and down, and "Make again" (back to shots made from the
  level). Once you change a shot they're this map's own, kept with its
  other Level choices, and every conversion writes them. "Save to map
  folder" writes them now, without converting: into showcase.txt in the
  converted map's folder (the one with its info.txt, so the map has to
  have been converted into it once), as a conversion would, and keeps
  them for the next conversion too (even if you then Cancel the window). An orbit is
  written as its keys, with a "# orbit" comment after its look line (the
  Level window reads it to change it again; Halo 1 Unlimited skips it).
  The file (what Halo 1 Unlimited reads; plain text, so it can be edited
  by hand too):
    # a comment (anything after # on a line)
    fade 0.5                 seconds of the fade to black between shots
    shot 12 orbit: red flag  a shot: its seconds, then a label
    look 12.3 -4.2 1.1       (optional) a point kept in the middle of the
                             view
    key 0 x y z yaw pitch    a keyframe: seconds into the shot, the
                             camera's position, its direction in degrees
                             (yaw 0 = +x, counterclockwise; pitch up
                             positive, as the camera tour's stops), then
                             the horizontal field of view (optional, 70)
  Shots play in order, then start again. Between keys the camera moves on
  a smooth curve through them in time (Catmull-Rom tangents from the
  neighbouring keys, Hermite between two; yaw the short way round); with a
  look point it turns to keep it in the middle.

Camera tours (Xbox, Settings tab)
  Halo 1 Unlimited's perf log (perf_log = 1 in opence.ini, test build 34
  and later) can fly the view through a list of spots on a map and measure
  each one (a T row each), so every run measures the same views and builds,
  maps and settings compare spot for spot. With the Settings tab's option
  on, each Xbox build also writes perf\tours\<map>.txt beside the map: up
  to 4 of its spawns (spread out), the level's 6 busiest views from where
  players stand (at least 6 units apart), 2 typical ones, and a look around
  from the middle. Copy it to the perf\tours folder beside the XBE. The log
  names a map by how the Xbox lists it (a map in the folder "_Unsorted" is
  _Unsorted_<map>), so rename the file to match the CSV files' name if it
  differs. It's plain text: reorder, delete or add stops (Halo 1 Unlimited
  can add your current view to it too). The tour moves the view, not your
  player: the game's own costs (tick_ms, the obj_* columns) follow where
  your player stands. The log's [tour] line says where it was written.

Perf logs (Settings tab)
  "Read perf logs..." reads Halo 1 Unlimited's perf logs (perf_log = 1 in
  opence.ini; a CSV a map session, in the perf folder beside the XBE) and
  writes in the log, and in <log>_report.txt beside it:
  - the frame rate: how much of the time it held 30 fps, how slow the
    rest was;
  - what limits it, second by second: the processor drawing, the
    processor running the game's ticks, or the graphics chip (a second
    is the chip's when the processor waited for it or the chip was busy
    92% of the time or more);
  - the graphics chip's work a frame, read in the slow seconds (at 30 fps
    its busy share also counts its wait for the screen's refresh:
    Sidewinder, a stock map that runs smoothly, shows 81%);
  - for the slow seconds: the processor's time a frame and how far over a
    30 fps frame it is, drawing's biggest parts and a game tick's, the
    costliest objects, and what's on screen (objects, triangles, lights,
    particles) next to the seconds at 30 fps;
  - what to try: the app's options that go after the biggest costs (the
    Level window's Lights, Effects, Weather, Riders, Hide scenery, the
    LODs window, portals);
  - with a camera tour (T rows), each view: its frame time, what limits
    it and its three biggest costs.
  Pick several logs (Ctrl or Shift) to compare them: side by side, and
  their tours view for view (<first log>_compared.txt).

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
  no light for it; a lens flare stays). A light an effect makes is
  dynamic whatever its tag says (Deathtrap's lightning is three effects'
  lights), so it's taken out of those effects instead. The engine can't
  light only players and vehicles: every light it makes lights the level
  too. What made the light still plays (Deathtrap's thunder). A light the map doesn't
  place (a custom weapon's explosion: where it goes off isn't known) is
  kept to Bungie's biggest, radius 15 (the Scorpion shell's explosion).
  The log's [lights] line lists them. There's no option: those lights
  don't run on the Xbox as they are.

Materials' see-through parts (Xbox)
  A material either draws solid, cuts holes where its texture's alpha is
  low (alpha-tested: leaves, grates, fences), or blends with what's behind
  it (glass, smoke; drawn after everything else, sorted, without hiding
  what's behind it, which costs more). The Xbox cuts a pixel out unless
  its alpha is above half. Which texture's alpha: a model shader's base
  map, an environment (level) shader's bump map. Every Xbox conversion
  checks the map's own materials against the alpha their textures
  actually have:
  - Alpha-tested, but nothing in the texture is see-through (no alpha, or
    all of it above half): the test never cuts anything, so it's turned
    off (one less test each time the surface is drawn, and in its shadow).
    Deathtrap: the tires, turret, barrel, horn, airfield, bulletin board,
    fan and tree trunks.
  - Alpha-tested, but 1% or less of the texture's alpha reaches half, and
    hardly any of it is fully clear: the Xbox would leave the surface out
    entirely, while Halo PC and MCC draw it (MCC BC7 textures whose alpha
    stays between 6% and 55%). They are drawn solid instead.
  Noted only, since they look as made either way: alpha-tested materials
  that hardly draw but are mostly clear, blended materials whose first
  texture has nothing see-through (they could be solid) and blended ones
  whose first texture is a clean cut-out (alpha-testing would be cheaper).
  The log's [materials] lines name them.

Content nobody can get (Xbox)
  Xbox maps have only 22 MB for level geometry and other tag data,
  so multiplayer maps leave out what no player can get in a match. The log
  lists each one as a [trim] line:
  - placements no game type can ever spawn, taken out first, so they're
    not offered in the Weapons & vehicles window or given a slot either:
    a vehicle with none of its gametype flags set (Precipice's Scorpion:
    Halo PC and MCC never spawn it, but the Xbox ignores those flags and
    would spawn it in every game), an item spawn point whose gametypes are
    all "none", and an unnamed vehicle, weapon or equipment marked "not
    placed: automatically". Named ones stay, because a script can make
    them. A map with no gametype flags on any vehicle (tags from an Xbox
    map) keeps its vehicles, since it never had flags to go by. A vehicle
    or weapon with nothing else placing it then drops out of the map. A
    slot vehicle stays when the map has Race vehicle spawns, because Race
    spawns the slot vehicles itself (a default Race game's second vehicle
    is the Scorpion, so Precipice keeps its tank for Race);
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

Machines (Xbox)
  Machines (device_machine: doors, lifts, anything a map animates as a
  device) cost the Xbox more every tick than anything else it places that
  doesn't move by itself: each updates its device, its dynamic lights and
  its bones, and an elevator looks for riders (Deathtrap's 74 machines
  took 6.3 ms of every 33 ms tick in Halo 1 Unlimited's perf log, where
  its 49 scenery took 0.3). Two things change that, and nothing you can
  see:
  - An elevator machine with no collision model (nobody can stand on it)
    keeps the flag but loses its elevator node. Every tick its node moved,
    it searched a sphere its size for bipeds riding it, and only a biped
    standing on its collision can ride. The flag also decides how its
    lighting is sampled, so it stays. Deathtrap's trees, rocks and airbase
    pieces scroll past as elevators like this: 14 kinds, 61 placed.
  - A machine that's only a prop is placed as scenery instead: no
    animation graph (it never changes pose), no device functions, sounds
    or effects, not an elevator, placed without a name (no script can
    reach it) and in no device group (no switch or script changes it).
    Scenery is drawn and lit the same way (shadowless, like a machine) but
    doesn't do a device's work every tick. A scenery tag is made beside
    the machine's; other placements of the same machine stay machines.
    Precipice's 42 holo panels are machines like this.
  The log's [machines] lines list them.

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
  and player tuning, and its first-person arms. (Each weapon's first-person
  animations find the arms' bones by name, and a weapon whose animations
  miss one isn't drawn: the map's arms are used unless fewer of the map's
  weapons would be drawn with them than with the Xbox's. CMT's arms have
  the Xbox's bones; they take about 0.1 MB more of the Xbox's memory.)
  The rest of the globals (menus, fonts, multiplayer sounds) stay the
  Xbox's. Either way, if the map has longer weapon and
  vehicle name lists than the Xbox (its custom weapons' pickup names, its
  vehicles' names), the Xbox uses the map's lists.
  Pickup icons: if the map's icon sheet has more icons than the Xbox's,
  or its own weapons' and grenades' HUDs show other pictures than the
  Xbox's at the icon numbers they use (a sheet laid out its own way, like
  CMT's: its pistol is number 4, the Xbox's sniper rifle), the map gets a
  sheet with both: the Xbox's own icons and controller button glyphs
  first, unchanged, then the map's extra icons (its custom weapons'), then
  the map's own pictures for those numbers, with its HUDs pointed at them.
  The pictures are compared, so a sheet that only adds to the stock one
  (Custom Edition's) keeps using the Xbox's. They're shrunk to the size the
  Xbox draws them (half, for icons the map's HUD globals draw at "high
  res scale").
  Once the map's weapons, items and vehicles are settled (the Xbox's own
  versions, your replacements, vehicles left out), those extras keep only
  what's still used: a weapon's or item's pickup name (and a weapon's
  ammo line after it), a vehicle's and its seats' names, and the icons of
  the weapons and grenades left. Unused names are left empty (every
  number stays as it was) and none come after the last one used; the
  icons kept are cut out of the map's sheet onto a small texture of their
  own (the pixels exactly as they were), and with none kept the Xbox's
  own sheet stays as it is. The Xbox's own names and icons all stay.
  Precipice, with the Xbox's own weapons: 2 of its 92 extra pickup names
  (the beam cannon's) and 2 of its 24 icons, 6.4 KB of tag memory and
  144 KB of textures less; with its own weapons, the icons they use, on
  one 256x256 texture instead of two 512x512 ones. The same goes for the
  pictures of nav point arrows added from the map ("Nav points" above): only
  those its arrows use. Everything else a replaced or removed weapon or
  vehicle brought (its model, animations, sounds, effects) is left out
  of the map by itself, unless something kept still uses it.

Sounds
  The Xbox only plays Xbox ADPCM sounds: mono 22 kHz, stereo 22 kHz and
  stereo 44 kHz. MCC and Halo PC also play Ogg Vorbis and uncompressed
  16-bit PCM, which are silent on the Xbox (MCC mods often use PCM for
  their custom weapons). The app re-encodes those as Xbox ADPCM, turns
  mono 44.1 kHz sounds into 22.05 kHz ones, and splits sounds too long
  for the Xbox to load at once.

HUDs from MCC maps
  Each MCC HUD tag has a canvas size, read from the map itself (extracted
  tags lose it). On "960p" (343's own HUD tags) everything is placed at
  double the Xbox's positions and drawn at half size ("half hud scale" on
  its bitmaps halves it again), so the app halves the positions and sizes.
  On "480p" (most mods' HUDs, CMT's included) the positions are the Xbox's
  already, and as on Halo PC an element with "use high res scale" (or
  drawing a bitmap with "half hud scale") is drawn at half size, so only
  those elements are halved. (Without the map, e.g. from your own tags,
  an MCC HUD is told from a Custom Edition one by comparing the stock
  assault rifle HUD with the Xbox's.)
  MCC also anchors HUDs to the middle of a screen edge (top centre,
  bottom centre, left centre, right centre), which the Xbox doesn't have:
  an anchor it doesn't know draws from the screen's centre with its size
  unset, so the element lands in the middle or not at all (CMT's shield
  and health bar). Those elements are drawn from the corner (or centre)
  beside the anchor, their offsets moved so they land where MCC puts them
  on a full screen: top centre and bottom centre from the left corners
  (still right on a two-player split screen, which is as wide), left and
  right centre from the centre. And MCC lets each element of a weapon HUD
  have its own anchor (CMT's visor: pieces along the top and bottom of the
  screen, in a HUD anchored to the centre), where the Xbox draws a whole
  HUD from one: the elements anchored apart go into copies of the HUD, one
  per anchor (e.g. visor_bottom_left), chained after it as its child HUDs.
  The Xbox draws the whole chain, each from its own corner, so they stay
  at their edges on a split screen too.
  The Xbox's corners aren't the screen's: it draws the HUD inside the
  TV's title-safe frame, 48 pixels (of 640) in from the sides and 36 (of
  480) from the top and bottom, where MCC draws to the screen's edges. So
  an element moved from the middle of the top or bottom edge to the left
  corner is moved back those 48 pixels, which centres it on the screen
  (CMT's health bar sat 48 pixels right of the middle). And the elements
  with an anchor of their own (the visor's pieces) are moved out by the
  whole frame, to the screen's edges where MCC has them; left inside the
  frame, they sat 48 and 36 pixels nearer the middle. On a split screen
  that puts them as far past each view's edge. The rest of the HUD (the
  shield, health, weapon and grenade panels, the motion sensor) stays
  inside the frame, the way the Xbox's own HUD sits.
  Numbers (ammo and grenade counts, a scope's zoom) MCC lays out on its
  960p canvas whatever the HUD's own, so on a "480p" HUD their positions
  are halved too (left as they were, CMT's grenade count sat under its
  panel and the ammo count over the bullets). And with the map's own
  weapons (the Xbox tab's "Use the Xbox's own versions" off), the Xbox
  draws counts with the map's own digits (its globals' "hud digits
  definition"): MCC draws HUD digits at half size and their spacing at
  half, and a digits sheet flagged "half hud scale" at half again, so a
  copy of the sheet is made that size (CMT's: 256x256 to 64x64) and the
  spacing halved. The Xbox's own digits (another font, wider apart) put
  the counts beside their panels. With the Xbox's own weapons, their HUDs
  and digits are the Xbox's.
  One difference is the Xbox's, not the map's: with widescreen on, the
  Xbox draws the HUD on its 640x480 screen and the TV stretches it with
  the picture, so round things come out a third wider (the motion sensor
  an oval), where MCC draws its HUD unstretched. That would be the XBE's
  to change, for every map at once.
  The textures themselves are then fitted (see HUD textures).

HUD textures
  The Xbox draws the HUD on a 640x480 screen, one texel to a pixel at
  scale 1. HUDs made for big screens draw big textures at a fraction of
  that: an MCC tagset like CMT's draws its 512x512 sheets at half, so 3
  of every 4 texels never reach the screen, and with no mipmaps the thin
  lines that do break up into dashes. For every bitmap of the
  map's own that only HUD elements draw, the app halves the texture while
  every element still draws it at no more than a texel to a pixel, and
  doubles those elements' scales: the same size on screen, the lines
  whole again, and a quarter of the memory per halving. (Precipice with
  CMT's weapons: 14 HUD bitmaps, 9.5 MB of textures down to 2.4 MB.)
  Bitmaps a HUD also uses some other way (a multitexture overlay's map),
  or that anything besides HUDs uses, are left alone.
  Scope masks (a weapon HUD's screen effect mask: the dark surround of a
  zoomed view) are drawn a texel to a pixel too, centred, not stretched to
  the screen. One made for a 1080p screen (1024 rows tall) is bigger than
  the whole Xbox screen, so the scope's window covered all of it and the
  surround never showed. Each is halved while it still covers the screen
  (240 rows for a split-screen-only mask), so it frames the view about as
  it does on the screen it was made for (CMT's 4 masks: 6 MB down to
  1.5 MB). MCC's BC7 HUD textures are written as DXT5 (DXT1 when nothing
  in them is see-through) as they're halved.
  Then what never reaches the screen goes. A bitmap every element draws
  at a texel to a pixel or bigger (and a scope mask) loses its smaller
  copies (mipmaps): the Xbox only reads those for something drawn
  smaller, and they're a third on top of the texture. And a crosshair
  overlay drawn centred and bigger than the 640x480 screen (CMT's focus
  rifle scope: 1024x1024 at full size) is cut down to the middle that
  can show (1024x512; a split-screen view is smaller still): 1.3 MB down
  to 0.5 MB, the same on screen.

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
  [pc]    Xbox models converted back to PC models, and their
          multipurpose maps put in PC's channel order (MCC: flagged).
          See "Shine and team colours" above.
  [slots] An Xbox vehicle slot was changed (Xbox tab).
  [xbox-mp] A vehicle or PC weapon was swapped, removed or kept for Xbox.
  [shrink] Textures were halved to fit the Xbox size limit.
  [textures] (also) Textures were halved to fit the texture memory target;
          cut-outs kept as solid, or a faded next version averaged instead.
  [scripts] A script function the target game lacks was replaced, a
          short that holds the game's time was made a long, or a kill
          volume script made to wait until a player is in its volume.
  [materials] Materials whose alpha test never cut anything, or would cut
          out the whole surface on the Xbox, were fixed; blended ones that
          could be cheaper are named (see "Materials' see-through parts").
  [bc7]   MCC-only BC7 textures were re-encoded as DXT.
  [textures] Textures were scaled or compressed like stock Xbox maps, or
          bump maps packed into P8 or DXT1 (see "Bump maps" above).
  [shaders] Layered transparent shaders were flattened for Xbox, model
          shaders' multipurpose maps put in the Xbox's channel order (see
          "Shine and team colours" above), or shaders with MCC's
          compatibility flags drawn the Xbox's way (see "MCC's
          compatibility flags" above).
  [effects] Effects busier than Bungie's busiest were given fewer
          particles (see "Effects" above).
  [heavy] Heavy models' most detailed versions were trimmed (see "Heavy
          models" above).
  [anim]  Animations were stored as keyframes (see "Animations" above).
  [collision] Custom collision took Bungie's own where it fits, or dense
          scenery collision was made again, simpler (see "Collision" above).
  [lod]   Lower-detail versions were made for models, or models got
          switch distances for theirs (see "Levels of detail" above).
  [portals] A level made without portals got them (see "Portals" above).
  [reflections] Level reflections were dropped, as chosen in the Level
          shaders window (see "Level reflections" above).
  [fog]   The level's fog was made fully opaque (see "Fog" above).
  [sky]   The map's sky was drawn in fewer layers (see "Sky" above).
  [scenery] Tiny scenery and devices aren't drawn, or models whose own
          super low size only hid them are drawn further (see "Tiny
          scenery" above).
  [weather] The level's rain or snow was made lighter or turned off (see
          "Weather" above).
  [props] Small props with very detailed collision lost it (see "Detailed
          props" above).
  [showcase] The map folder's showcase.txt was written, or kept (see
          "Showcase flythroughs" above).
  [tour]  A camera tour for Halo 1 Unlimited's perf log was written (see
          "Camera tours" above).
  [lights] Dynamic lights that would have the Xbox draw most of the level
          again were turned off or shrunk (see "Dynamic lights" above), or
          the map's objects' lights lost their shine or were turned off
          (the Level window's "Lights").

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
  - Sky: the layers the Xbox draws per pixel of each of the level's skies,
    over views all round and in the busiest one (looking up), next to
    Bungie's multiplayer skies (see "Sky" above).
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

Converting a map again is quicker than the first time:
- The tags taken out of a map are kept for the 3 maps converted most
  recently (in %LOCALAPPDATA%\HaloMapPorter\extracted), so the Level
  window, the Weapons & vehicles window and the conversion after them
  take them out of the map once ("Reusing the tags extracted from ...
  before" in the log). They're taken out again when the map file, the
  bitmaps.map / sounds.map it reads or Invader changes.
- The slow steps that turn a tag into another the same way every time
  keep what they made (in %LOCALAPPDATA%\HaloMapPorter\results, up to
  768 MB, the least recently used dropped first): sounds re-encoded for
  the Xbox, MCC textures re-encoded as DXT, textures compressed and bump
  maps packed, the sky drawn in fewer layers, and the Level window's
  measurement of the level (the conversion measures the same level
  again). The same tag (in that map or another one) gets the copy
  instead. A new version of the app makes them again.
- Reading and changing values in tags, listing the tags a tag uses and
  renaming references (what invader-edit, invader-dependency and
  invader-refactor do) happen inside the app instead of starting
  Invader's tools hundreds of times per conversion. The tags come out
  byte for byte as those tools save them; anything they'd do differently
  (an error, say) still goes to the tool.
- Several sounds are re-encoded at once, the map's tags are copied on
  several threads, and the Level window's measuring leaves the map's
  sounds alone (they don't change what it measures).


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
    Delete %LOCALAPPDATA%\HaloMapPorter (cached tags, the tags kept from
    recent maps, kept results, work files, reports) and/or
    %APPDATA%\HaloMapPorter (settings). The app recreates them as
    needed.


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
