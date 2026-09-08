# F5 Model Display-List Grammar (Phase A extraction)

Ground truth for how the game's CPU compiles models into the F3DFACTOR5 display-list
stream, extracted from the recompiled functions (`/e/Projects/N64Recomp/RecompiledFuncs`).
Every claim carries a confidence tag:

- **VERIFIED** — read directly from the recomp source at the cited line (orchestrator-checked).
- **EXTRACTED** — agent-extracted with line citations, not independently re-checked.
- **UNCERTAIN** — flagged by the extractor; treat as hypothesis.

This spec exists so the RT64 GBI module (`lib/rt64/src/gbi/rt64_gbi_f3dfactor5.cpp`) can be
audited against what the game actually emits (Phase B), instead of being grown from runtime
observations alone.

## 1. Pipeline architecture

The game does NOT hand models to the RSP. The CPU walks the scene graph and compiles every
visible model into DL commands each frame:

```
traverseSceneGraphRecursive (funcs_4.c:16459)
  → processSceneNode (funcs_4.c:15315)             node matrix + visibility
    → processMeshdef1ForLod (funcs_4.c:14963)      LOD selection
      → submitSceneNodeRender (funcs_4.c:1820)     per-node prolog: lights, RDP state, materials
        → renderLitMeshFaceGroup (funcs_4.c:9893)    lit geometry → DL
        → renderFlatMeshFaceGroup (funcs_4.c:4555)   flat geometry → DL
        → renderUnlitMeshFaces (funcs_4.c:6488)      unlit geometry → DL
          → buildOrientedFaceGeometry (funcs_4.c:7715)  oriented/billboard quads → DL
          → resetVertexCacheSlot (funcs_4.c:7143)       RSP vertex-cache slot management
          → emitMaterialTexturedDL / Alt (funcs_3.c:18359 / 19547)  per-material texture DL
          → appendRdpStateDl (funcs_3.c:17896)          RDP state block
          → emitAllocatedDLCommand (funcs_3.c:2557) / reserveAndEmitDLEntry (funcs_3.c:14608)
          → findAndUnlinkSmallestEntry (funcs_6.c:13573) chunk allocator
```

The renderers iterate STAGED per-material face lists (linked records built during scene-graph
processing), not raw HOB faces; raw HOB data is upstream. **EXTRACTED**

## 2. Emit primitives + chunk discipline

- `emitAllocatedDLCommand(a0=cursor, a1)` / `reserveAndEmitDLEntry(a0=cursor, a1)`
  (funcs_3.c:2557 / 14608): bounds-check the cursor against the chunk ceiling
  (`0x80116x` globals: head `+0x63B0`, cursor `+0x63FC`, ceiling `+0x63D4`); on overflow they
  write `w0=0xB5000000 w1=0` (chunk-link placeholder), pull a new chunk via
  `findAndUnlinkSmallestEntry`, and back-patch the link. **VERIFIED** (funcs_3.c:2588-2603)
- Command words are written AT THE CALL SITES via `MEM_W(0x0,cursor)=w0; MEM_W(0x4,cursor)=w1`.
- Stream framing: `0xE700` pipe-syncs, `0xB500` chunk links, `0xB800` end. **EXTRACTED**

## 3. Command grammar

### 0x04 — vertex batch load (CONFIRMED, highest confidence)
```
w0 = 0x04<<24 | (n << 10) | (((n*8 + 0xF) & 0xFFF0) - 1)     n = vertex count
w1 = RDRAM pointer to packed 8-byte vertices
```
Classic G_VTX shape: count in bits 10+, (DMA length-1) low, with 8-byte vertices.
Emission sites: renderLitMeshFaceGroup funcs_4.c:10870-10883; buildOrientedFaceGeometry
funcs_4.c:8300-8323. **VERIFIED** (matches runtime prior `vertCount=(w0>>10)&0x3F`).
RSP cache slot stride 0x50; slot counter global `0x8012xxxx-0x5BB0`. **EXTRACTED**

### 0x01 — matrix load
- renderLitMeshFaceGroup emits `w0=0x01020040, w1=0x80037780` (funcs_4.c:11787-11800,
  ROM 0x80013B08): b1=0x02 (LOAD param), w1 = STATIC data-segment block at 0x80037780.
  **VERIFIED**. Content of 0x80037780 not yet dumped (identity? lighting basis?).
- The runtime camera matrices seen by the GBI (w1=0x80700000 proj / 0x80710000 modelview)
  are NOT emitted by submitSceneNodeRender or the facegroup renderers — their emitter
  (likely setupCameraMatrices funcs_4.c:20538 or the per-frame header builder) is still
  unpinned. **UNCERTAIN / next round**

### 0xBF — triangle, 0x13 — quad
- Selection: staged-face `flags & 0x3 == 0` → 0xBF triangle, else 0x13 quad
  (funcs_4.c:10829, 11175-11207). **EXTRACTED**
- Index encoding (runtime prior, consistent): w1 packs vertex-cache byte offsets
  (slot×0x50): fields `((w1>>13)&0x7F8) ((w1>>5)&0x7F8) ((w1<<3)&0x7F8)`.
- `w0 & 0x2` → 16-byte inline texcoord block follows. (Runtime-proven; emission site for
  the inline block not isolated this round.) **UNCERTAIN at source level**

### 0xB4 — oriented quad (CURRENTLY DROPPED BY HLE — high-priority gap)
```
w0 = 0xB4000006
then: 4× u16 vertex-cache slot values (from sp+0x5E/0x58/0x5A/0x5C), constant 0x10001000,
      and additional words — total command length ≥16B, possibly 32B.
```
funcs_4.c:9782-9854 (ROM 0x80012F08), in buildOrientedFaceGeometry (billboard/oriented
faces; called from renderLitMeshFaceGroup:12099 for regular models too). **VERIFIED emission;
length UNCERTAIN.** The HLE maps 0xB4 → op_consume16 (16 bytes): if the real length is 32B
the interpreter desyncs after every 0xB4 — and the quad geometry is dropped either way.

### 0xBD — per-face color + lighting (HLE DANGER: inherited as F3DEX popMatrix)
```
w0 = 0xBD00xxxx  (agent: | ((count*5+idx)<<8))
w1 = RGBA — material color × CPU-computed lighting (lit) or raw material color (unlit)
```
renderLitMeshFaceGroup funcs_4.c:11174-11230 (lit), 11339-11361 (unlit). Lighting is
CPU-side: per-channel material×f22 scale, f20 threshold clamp (funcs_4.c:10828-10948);
light direction bytes accompany the color. **EXTRACTED, not orchestrator-verified.**
The F5 GBI module does NOT override 0xBD → falls through to F3DEX `popMatrix`
(rt64_gbi_f3dex.cpp:80). Under native matrix loading, spurious pops corrupt RT64's
matrix stack. AUDIT FIRST.

### 0xBE — merged state word (HLE DANGER: inherited as F3DEX cullDl)
Emitted unconditionally at the end of appendRdpStateDl: `w0=0xBE000000, w1=t0` where t0
accumulates mode bits from tables at 0x8003775C / 0x8003776C indexed by node mode bits
(funcs_3.c:18200-18208). **EXTRACTED.** Not overridden by the F5 module → F3DEX `cullDl`
(rt64_gbi_f3dex.cpp:79) interprets w1 as vertex indices and may cull/terminate real DLs.
AUDIT FIRST.

### 0xFA — G_SETPRIMCOLOR (standard, HLE OK)
`w0=0xFA008000; w1 = face RGBA (staged-face flags&0x200) else 0xFFFFFFFF`
buildOrientedFaceGeometry funcs_4.c:9229-9277. **EXTRACTED**

### appendRdpStateDl block (funcs_3.c:17896-18310)
Args: a0=cursor, a1=node mode flags (node+0x2C), a2/a3=out bytes. Sequence:
1. `0xE700` pipesync (17904-17906) **EXTRACTED**
2. conditional `0xB900 0201`-style othermode_L w/ float-derived fog/alpha value
   (17964-18034, on a1&0x8000000)
3. conditional `0xB700` (set geometry mode, w1=t1) / `0xB600` (clear geometry mode, w1=t2)
   (18162-18193)
4. always `0xBE00` w1=t0 merged state (18200-18208) — see 0xBD/0xBE warnings above.
Mode-bit → state tables: 0x8003775C (idx=(a1>>20)&7), 0x8003776C (idx=(a1>>23)&7).

### Material / texture bind — emitMaterialTexturedDL (funcs_3.c:18359) + Alt (19547)
- Args: a0=cursor-ish, a1=texture index, a2=flags (Alt forces |0x1), a3=material ptr.
  Early-outs: table entry flags bit0 clear, or a2&0x8. **EXTRACTED**
- Index remap: reads u16 at entry+0x2 of the table at 0x8011A444 (lui 0x8012, lw -0x5BBC)
  — the HMT→global remap table (matches registerHmtTextureInTable, arch doc §18) — and
  passes it to `findOrCreateMaterial`. **EXTRACTED**
- W/H come from the GLOBAL texture table D_80128F08 via getTextureLUTFieldAt8 / AtA
  (funcs_6.c:14542-14593): base = 0x80130000-0x70F8 = 0x80128F08, offset = idx*0x24
  (computed ((idx<<3)+idx)<<2), reads +0x08 (W) and +0x0A (H). **VERIFIED math** — confirms
  the D_80128F08 entry layout used by the host resolver.
- The actual SETTIMG/SETTILE/LOADBLOCK/LOADTLUT emission lives in the NOT-YET-EXTRACTED
  helpers `findOrCreateMaterial` and `emitMaterialRenderModeDL(Alt)`. The top-level emits
  bracket commands only (0xE7 sync, 0xFC combine 0xFCFFFFFF/0xFFFE793C, 0xBB texture,
  Alt ends 0xB9000002). Render-mode cache: skip if mode byte equals cached
  (0x8012xxxx-0x56F8). **EXTRACTED**
- DL vs Alt: Alt forces flag bit0, calls emitMaterialRenderModeDLAlt, ends with 0xB9
  instead of 0xBB. **EXTRACTED**

### Material binds resolved: cached per-material sub-DLs via G_DL (VERIFIED)
findOrCreateMaterial (funcs_6.c:14274-14541; a0=DL cursor, a1=texture index u16, a2=mode bits):
- Material cache: bucket table **D_80128F00[texIdx]** → linked material nodes (the 0x18-stride
  matpool at D_80128EF4 — the tickTextureMaterialExpiry system). Node fields: +0x0 next,
  +0x8 dirty u16, +0xC mode byte (cached in 0x80128908-area byte -0x56F8), +0xD aux byte,
  +0xE compare byte, +0x10 mode word, **+0x14 = the node's cached material DL address**.
- Cache hit → emits **w0=0x06000000, w1=node->0x14 - 8** (funcs_6.c:14501-14510): texture
  binds enter the stream as **G_DL sub-DL calls into cached per-material DLs**. w0 passes the
  F5 GBI's strict-G_DL filter ((w0&0x00FEFFFF)==0) → the HLE already follows them.
- Cache miss → allocOrEvictMaterialNode (funcs_6.c, 0x8002236C) builds the cached DL —
  callee scan shows parseImageFile + decodeRdpFormatFlags + packTlutEntriesRgba5551, i.e. the
  cached DL is built from the game's own decoded format data (no format guessing needed
  anywhere). NOT yet line-extracted: the exact SETTIMG/SETTILE/LOADBLOCK sequence it writes —
  next target along with emitMaterialRenderModeDL (funcs_7.c:26) / Alt (funcs_7.c:339).
- Chunk overflow handled inline (0xB500 link + allocateDisplayListBuffer 0x80007D74).
Implication for Phase D: this is the data path that makes the host-side D_80128F08 resolver
(f5_build_global_mats) redundant — the stream already carries correct, game-decoded binds.

### Cached material-DL contents (EXTRACTED 2026-06-10)
Chain: allocOrEvictMaterialNode (funcs_6.c:13845) → allocateDisplayListBuffer →
**emitMaterialRenderStateDL (funcs_6.c:~12172)** builds the snippet → node+0x14 = its addr;
ends with 0xB800 + 0xB500 framing (funcs_6.c:14114-14140). Snippet contents:
- **loadTextureTile() called TWICE** (funcs_6.c:12232, 12264) — the actual
  SETTIMG/SETTILE/LOADBLOCK emission; NOT yet line-extracted (next target).
- othermode-H **0xBA00/w1=0x1001 or 0x1402** + othermode-L **0xB900/w1=0x031D or 0x0002**
  (funcs_6.c:12293-12460; funcs_7.c:211/255/293) — standard F3D ops our GBI passes through.
- combiner/mode words from data-segment tables (emitMaterialRenderModeDL funcs_7.c:26-338:
  tables 0x8004-0x7720/-0x771C indexed by a mode bitfield s0 built from arg flags
  funcs_7.c:74-160; Alt variant funcs_7.c:339-615 uses tables -0x7520/-0x751C).
- Node fields: +0xA matID, +0xC mode byte, +0x10 = a2&7, +0x14 cached-DL ptr.
- Format decode: **decodeRdpFormatFlags (funcs_6.c:4030) maps a 6-value format index
  (bits 0-5) → RDP fmt/siz** — the Factor 5 image subtype enum (0..5, = rerogue's) made
  flesh; the cached DLs carry game-correct formats, no host inference required.

### loadTextureTile (funcs_6.c:7827) — cached-DL texture loads are PURE STANDARD RDP
Per-format switch on (entry flags & 0x3F)-1 bounded 0..5 (the subtype enum, funcs_6.c:8024-8055):
each case emits the classic libultra block-load sequence — 0xE6/0xE8/0xE7 syncs, 0xFD SETTIMG
(w1=data@entry+0x10 or TLUT D_80128EFC[entry+0x0E]), 0xF5 SETTILE (fmt/siz in w0 low bytes),
0xF3 LOADBLOCK, 0xF0 LOADTLUT (CI only), 0xF2 SETTILESIZE; W/H from entry+0x08/+0x0A (mip-shifted
by a2). ⇒ **cached material DLs contain ONLY standard RDP ops the GBI already interprets** —
Phase A material grammar COMPLETE; Phase B audit unblocked end-to-end.

### 0x14 — secondary vertex batch? (UNCERTAIN)
renderLitMeshFaceGroup's secondary pass (flag 0x21 variant, funcs_4.c:11598-12174) appears
to emit `0x14` batches (~12052) where the primary used 0x04, and an `0x01020040`-prefixed
end-of-material marker (11793). Unmapped in the F5 GBI module. Verify before acting.

### 0x02 — per-vertex color staging (runtime-known; emitter NOT found this round)
Runtime shows op_02 (w0 low byte=count, w1=RGBA buffer). None of the five extracted
functions emit it — likely the particle/explosion path (out of scope) or another emitter.
**OPEN**

## 4. submitSceneNodeRender prolog (funcs_4.c:1820-4554)
- Args: a0=scene node, a1=render-pass index, a2=DL cursor, a3=render-pass array. **EXTRACTED**
- Sequence: transformSceneLights(node, pass, cursor, &flags) (:1976) → appendRdpStateDl
  (:2207) → chunk reservations (:2223, :2239) → per-material emitMaterialTexturedDL (:2320,
  on material-ID change; lit-vs-flat selected by material flag bit 2, :3714-3716) →
  facegroup renderers. Node mode word at node+0x2C (bit 0x8 lighting; 0x4000/0xC000
  transform/cull). Pass lists hang off 0x80138D18-area globals (0x63B0/0x63FC). **EXTRACTED**
- transformSceneLights' OUTPUT consumption (DL command vs DMEM staging) not isolated. **OPEN**

## 5. Gap analysis vs rt64_gbi_f3dfactor5.cpp (Phase B seed)

| Gap | Severity | Action |
|---|---|---|
| 0xBD inherited as F3DEX popMatrix | HIGH — corrupts native matrix stack | Verify emission at runtime, then override (no-op first, then implement color) |
| 0xBE inherited as F3DEX cullDl | HIGH — may cull/terminate real DLs | Verify, then override |
| 0xB4 dropped (op_consume16) + length possibly 32B | HIGH — dropped quads + possible DL desync | Dump a 0xB4 at runtime, fix length, implement quad emit |
| 0x14 unmapped (secondary lit pass) | MED | Verify emission, map |
| op_01 static block 0x80037780 | MED | Dump the block; ensure rsp->matrix handles it sanely |
| Camera-matrix emitter unpinned | MED | Find who emits op_01 w1=0x807xxxxx (setupCameraMatrices?) |
| Material bind internals (findOrCreateMaterial / emitMaterialRenderModeDL) unextracted | MED | Next extraction round — settles SETTIMG/SETTILE grammar |
| 0x04 encoding | NONE — native handler matches | — |
| 0xFA prim color | NONE — standard RDP handler | — |

## 6. Cheapest verification next step
A one-shot runtime DL dump during the cinematic (`ROGUESQ_DUMP_FRAME_DL=N`) showing 0xBD /
0xBE / 0xB4 / 0x14 occurrence counts + the words around each settles every UNCERTAIN above
without further static reading.

## 7. Ucode truth (2026-09-07, from the IMEM listing; supersedes the runtime priors above)

Source: `dumps/f5_ucode.imem.bin` (byte 0 = ucode image byte 0 = IMEM `0x1080`; the ucode's own
constants and the DMEM dispatch table use real IMEM addresses, so listing address = real - 0x80).
Overlays live in the RDRAM ucode image (`0x80024A10`), table at DMEM `0x76..0xAA`.

**Fetch model (real `0x1088..0x12F8`).** One 0x108-byte chunk is DMA'd into DMEM `0x170` and executed
from `+8` (the header's next/prev pointers are never executed; op `0x80` dispatches to IMEM 0). When the
cursor reaches `+0x108`, or on `B5`/`0x12`, the ucode fetches the chunk named by the current chunk's
FIRST WORD and continues at `+8` (`B5`'s own w1 is never read). `06` pushes `{chunk, cursor}` and
fetches w1; `07` fetches w1 without pushing; `B8` pops, or ends the task when the stack is empty.
The allocated chunk list is doubly linked: a valid next chunk's second word points back to the current
chunk (the HLE and `tools/validate/f5_dl_walk.py` require this; a stale call into a recycled chunk
would otherwise walk the free list).

| op | handler (real) | meaning | length |
|---|---|---|---|
| `01` | `0x1484` | DMA 64 B matrix from w1: byte1 bit0 = 0 modelview (DMEM `0x5D0`, then MVP = MV x P into `0x610`), 1 = projection (`0x590`) | 8 |
| `02` | `0x14F0` | DMA `(w0&0xFFFF)+1` bytes from w1 into `0xB70` = per-vertex RGBA colors | 8 |
| `03` | `0x14D4` | byte1 selects a DMEM slot (table `0x46`); the next 16 B are stored inline. `03 80` = viewport (vscale x,y,z,pad / vtrans x,y,z,pad, 2-bit fixed); `03 82` = lookat/light block | 24 |
| `04` | `0x15B4` | DMA `(w0&0x3FF)+1` bytes from w1 into `0x280`; `(w0>>10)&0x3F` 8-byte vertices (x,y,z int16, pad) | 8 |
| `05` | `0x15AC` | load overlay 0xC: `05 05 02 ..` = flat terrain tile (below); `05 05 00 ..` = heightfield tile (overlays 0x14/0x18, not yet in the HLE) | 40 |
| `BF` / `08` | `0x146C` | triangle: w1 bytes 1..3 = vertex slot (byte/5 = index), word2 bytes = per-vertex color offsets into `0xB70`, word3 = flags; `w0&2` = textured, 16 B of (s,t) 8.8 texels follow | 32 / 16 |
| `B4` / `13` | `0x1484`-0x80 | quad: as `BF` plus the 4th vertex from w1 byte0; emits two triangles | 32 / 16 |
| `14`, `BD`, `BE` | `0x12EC`, `0x15A4`, `0x12E4` | 16-byte state commands (`BD` also loads overlay 0x2C) | 16 |
| `06`, `07`, `B5`, `B8` | `0x12A0`, `0x12B8`, `0x10E0`, `0x12C4` | call, branch, next-chunk, return | 8 |
| `E4`/`E5` | top2 = 3 path | texrect copied to the RDP buffer | 16 |
| `08..12` | shared table | aliases of `BF..B5` | as above |

**Flat tile (`05 05 02 xx`, overlays 0xC then 0x24).** Word pairs after the command: w1 = (h0,h1),
word2 = (h2,h3) corner heights; word3..word6 = corner RGBA; word7.lo = texcoord size (8.8);
word8 = (x, y>>4); word9 = (z, size). Corners v0=(x,y+h0,z) v1=(x+size,y+h1,z) v2=(x,y+h2,z+size)
v3=(x+size,y+h3,z+size), UVs v0 (0,s) v1 (s,s) v2 (0,0) v3 (s,0), transformed by the current MVP.

**Heightfield tile (`05 05 00 xx`).** DMAs `(word4.hi+0x16)&0xFF0` height bytes from word2 into DMEM
`0x4E0` and `(word4.lo+0x13)&0xFF0` color bytes from word3 into `0x380`; byte1 = samples per row,
byte3 = LOD shift, byte2>>4 = second shift. Overlay 0x14 subdivides/averages rows, overlay 0x18 emits
the grid. Not yet implemented.

**Conventions.** NDC is y-down (the ucode adds `ndc*vscale` with no negation); the HLE hands RT64 a
negative `vscale.y`. Matrices are standard N64 split int/frac, row-vector convention.
