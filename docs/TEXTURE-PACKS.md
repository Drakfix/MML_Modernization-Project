# HD texture packs

The Modern renderer can swap any of the game's textures for your own PNGs, at any size.

## Making a pack

1. In the launcher, Graphics tab, set **Dump originals** to On (or `[textures] dump=1` in
   `mml.ini`) and play through the areas you want to cover. Every texture the game uses
   is written once to `Modernization\textures\dump\<hash>.png`.
2. Each dump is a whole 256x256 texture page with its palette already applied, so the
   same page shows up once per palette it's drawn with (a sign in two colours is two
   files). The name is a CRC32 of the page's size and pixels.
3. Make your version of a page: same layout, any size (2x = 512x512, 4x = 1024x1024 and
   so on; keep the aspect ratio). Transparent pixels in the original (alpha 0) should
   stay transparent.
4. Save it under the same name anywhere inside `Modernization\textures` except the
   `dump` folder, for example `Modernization\textures\my-pack\1bc08d0c.png`. Subfolders
   are fine, so packs can be kept apart. A folder whose name starts with `_` or ends with
   `.off` is skipped, which switches that pack off without moving it.
5. With **HD pack** On (the default, `[textures] replace=1`) the game picks it up on the
   next start.

Turn dumping off again for normal play: writing PNGs costs a little time whenever a new
texture appears.

## Notes

- If a page changes at all (an animated texture, a page the game reuses for something
  else), it gets a new hash and needs its own replacement.
- Packs made for Beetle PSX HW (the PlayStation version) don't apply directly: their
  names hash the PS1's 4-bit texture uploads, while the PC version stores most world
  textures as 8-bit pages laid out differently. Converting one would need a matching
  tool.
- Replacements are loaded the first time they're needed and kept in memory.
