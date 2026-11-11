# Status and roadmap

**Not yet tested on hardware or with a real `data.win`.** All the code below compiles, and the desktop build passes the test suite (`make test`) under AddressSanitizer. The tests use synthetic `data.win` files built by `tools/mkdatawin.py`. Because of that, the parts most likely to need fixes first are the **version-dependent `data.win` fields** and the **rarely used 2.3+ bytecode instructions** (marked ⚠ below).

## Done

### data.win reader (`src/core/datawin.c`)
- Chunks: GEN8, STRG, TPAG, SPRT, BGND (GMS2 tilesets), FONT, SOND, AGRP, OBJT, ROOM (views, instances, layers: background / instances / tiles / assets), CODE, VARI, FUNC, SCPT, GLOB, AUDO (offsets only), TXTR (count only).
- Variable and function reference chains are patched in place, so the VM runs straight from the file buffer with no extra copy of the bytecode.
- Only the chunks before TXTR/AUDO are kept in RAM. Textures come from the converter and audio is streamed from the file.
- ⚠ Detection of version-dependent fields: the OBJT "managed" flag, the room instance-creation-order list (2024.13), layer effect data (2022.1+), compressed tile data (2024.2+), font header extras and tileset separation fields. Each one uses a layout check instead of a version number. These checks are the first things to verify against a real file.

### Interpreter (`src/core/vm.c`)
- All main opcodes: arithmetic, comparison, conversion, branches, `with` (`pushenv`/`popenv`, including the early-exit form), calls, `callv`, return/exit, `dup`, the pop-swap form.
- Variable scopes: self, other, global, local, static, builtin, instance ids, object indices (writes go to all instances), structs, stack-top.
- `argument0..15`, `argument[n]`, `argument_count`.
- Arrays: 1-D, nested `[i][j]` (`pushaf`, `popaf`, `pushac`), copy-on-write via `setowner`.
- Structs and constructors (`@@NewGMLObject@@`), array literals, methods, `method()`, `script_execute`.
- ⚠ Need checking on real code: `savearef`/`restorearef`, `dup` with a swap size, `chknullish`, `pushref` for non-function assets.
- Unknown functions return `undefined`, and each is logged once by name.

### Runtime (`src/core/runtime.c`)
- Event order: begin step → alarms → keyboard events → step → motion (speed, friction, gravity) → collision events → end step → animation (Animation End) → draw.
- Draw passes: Pre Draw, then per view Draw Begin / Draw / Draw End, then Post Draw, Draw GUI Begin, Draw GUI, Draw GUI End.
- Rooms: room instances keep their ids, pre-create and creation code, Game Start, room creation code, Room Start / Room End, persistent instances, `game_restart`, `game_end`, `game_change` (chapter switching).
- Views and cameras with follow and borders.

### Built-in functions (about 600)
Math, random, strings (UTF-8 aware), arrays, structs and variables, instances, collisions (bounding boxes), motion, layers and backgrounds, cameras and views (`__view_get`/`__view_set` too), sprites, text and fonts, primitives, blend modes, audio (sounds, streams, fades, pitch, audio groups), `ds_map`/`ds_list`/`ds_grid`/`ds_stack`/`ds_queue`, JSON (`json_decode`, `json_encode`, `json_parse`, `json_stringify`), INI files, text files, file system queries, keyboard input, colours. Desktop and Steam-only functions are stubbed with harmless return values.

### 3DS platform (`src/platform/platform_3ds.c`, `src/main_3ds.c`)
- citro2d renderer: 640×480 game window scaled to 320×240 with letterboxing, texture tinting (`C2D_TintMult`), mirroring, rotation, blend modes.
- Texture atlas cache with LRU eviction and a per-console memory budget. The Old 3DS build packs textures to RGBA4444 when it loads them.
- ndsp audio: WAV (8/16-bit PCM) and Ogg Vorbis (tremor). Short sounds are decoded into memory, music is streamed on a background thread (core 1 on Old 3DS, core 2 on New 3DS). Supports gain, pitch, pause and seek.
- A game-list launcher on the bottom screen, log file, saves on the SD card.
- ⚠ Not compiled locally: devkitPro is not available in the development container. The GitHub Actions workflow compiles both builds. Expect small API fixes on the first CI run.

### Tools
- `tools/d3ds_convert.py`: reads PNG, QOI and BZip2+QOI texture pages, repacks every texture item into 1024×1024 atlases, and writes them in the 3DS tiled format. Can also copy the needed game files into an SD-card layout. ⚠ The QOI/BZ2 decoders have not been tested on real GameMaker files.
- `tools/mkdatawin.py`: a GameMaker bytecode assembler and `data.win` writer, used to make the test games.

## Missing / next steps

In rough priority order for getting DELTARUNE to its first room:

1. **Run against the real `data.win` on the desktop** (`build_host/delta3ds_host data.win 600`) and fix whatever parsing or opcode problems show up.
2. **Implement the functions the log reports** as "unimplemented function" during the first rooms.
3. **First CI build of the 3DS code**, then fix any libctru/citro2d API mismatches.
4. **Surfaces** (`surface_create` / `surface_set_target` / `draw_surface`) with render-to-texture. Right now drawing to a surface is discarded. DELTARUNE uses surfaces for some effects and transitions.
5. **Precise collision masks.** Collisions currently use bounding boxes only (rotation-aware).
6. **Shaders.** Not possible on the PICA200 in the same way; effects need hand-written replacements.
7. **Persistent rooms**, paths (`path_start`), timelines, sequences, particles, buffers.
8. **Performance work:** cache instance lists per object, cache resolved event handlers, add frame-skip on Old 3DS.
9. **Memory on Old 3DS:** check that a chapter's `data.win` (without textures) plus the runtime fits, and stream the CODE chunk if it doesn't.
10. Touch-screen extras: on-screen buttons and a settings menu.
