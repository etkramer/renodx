# RenoDX: Batman: Arkham Knight (batmanak-rend2)

SDR-only rendering mod: AgX, GTAO, world-space GI, contact shadows, DLAA. Uses RenoDX as a library, not as a look mod.

## Facts

| | |
|---|---|
| Game decomp (C) | `D:\SteamLibrary\steamapps\common\Batman Arkham Knight\Binaries\Win64\BatmanAK.exe.c` |
| Engine / API | UE3, D3D11, x64 |
| ReShade loader | `dxgi.dll`. |
| Build target | `batmanak-rend2` -> `build/Debug/renodx-batmanak-rend2.addon64` |
| CRC32 reference | `src/games/batmanak` (upstream HDR mod) |
| Frame breakdown | http://morad.in/2020/04/03/unmasking-arkham-knight/ |
| Live inspection | `.mcp.json` -> `renodx-devkit` MCP server (needs game running + DevKit) |
| Shader source | `devkit_get_shader` disassembly. Decompilation is broken (`cmd_Decompiler.exe` is x86, needs an x64 `d3dcompiler_46.dll`) |

Game has a real G-buffer with accurate normals. Never reconstruct normals from depth.

## Frame map

Verified by DevKit snapshot. Handles and draw indices are per-run; anchor on shader hashes.

Hashes are per-permutation: graphics settings recompile passes into different shaders, so a
replacement must cover every variant or it silently does nothing when a setting changes. Known
pairs (motion blur off / on): DOF composite `0xB07D74F7` / `0x8A3FA02D`, tonemap `0xB6B56605` /
`0x978BFB09`. Assume more exist.

G-buffer MRT, full res. Static geometry binds RT0-3; dynamic geometry binds all six:

| RT | Format | Contents |
|---|---|---|
| 0 | `r11g11b10_float` | HDR scene colour / emissive accumulation |
| 1 | `r10g10b10a2_unorm` | World normals, alpha unused. Decode `normalize(rgb * 2 - 1)` |
| 2 | `r8g8b8a8_unorm` | Albedo |
| 3 | `r8g8b8a8_unorm` | Material params, alpha is a sparse mask |
| 4 | `r16g16_float` | Velocity, dynamic geometry only |
| 5 | `r16_float` | `length(prevWorldPos - worldPos)` |

Pass order:

| Pass | Shader | Notes |
|---|---|---|
| G-buffer | VS `0x03BEFD54` + many PS | 4-RT MRT, static |
| G-buffer + velocity | many VS/PS, e.g. `0x5C4CCB28` / `0x9C6974F1` | 6-RT MRT, dynamic |
| Depth downsample | PS `0xADD61541` | -> quarter-res `r32_float`, colour writes masked |
| Shadow depth | PS `0x2E6D2DFE` | ~86 draws, 0 RT |
| Directional occlusion | CS `0x0E7C20A1` | depth + normals -> `r16g16b16a16_float` |
| Shadow masks | PS `0xE0096F19` and others | half-res `r8_unorm`, `min` blend |
| Volumetric inject/scatter/integrate | CS `0xA8253F51`, `0x170E1452`, `0x10619B97` | 80x80x48 froxels |
| **Deferred lighting** | CS `0x7A5E1E66` | 18 SRVs -> `r16g16b16a16_float` full res |
| Separable blur | CS `0xECE88095` (128x1), `0x707DD08A` (1x256) | H then V |
| Fog composite | CS `0xFE8D7315`, `0x816CB5D7` | froxel volume + 6 tex |
| **SMAA 1x** | PS `0x8BE4180B` -> PS `0x2F55F98E` -> CS `0x42C0137E` | edges -> weights -> blend. Purely spatial. DLAA replaces all three |
| DOF/bloom prefilter | CS `0xB2A276FD` | -> 3x half-res |
| Bloom pyramid | CS `0x0E6435F4` x8, then `0x2B46745A` alternating | |
| Lens flare | CS `0xB42A7F40` | matches `batmanak`, so hashes are stable across this build |
| Motion blur setup | CS `0xF98BC308`, `0x98C5C8CA` | motion blur on only. Packed velocity/depth + 16x16 TileMax/NeighborMax; the gather is fused into the tonemap |
| DOF composite | CS `0xB07D74F7` / `0x8A3FA02D` | chromatic aberration + far blur |
| **Tonemap** | CS `0xB6B56605` / `0x978BFB09` | see below |
| UI | many PS, mostly `0x8C27F313` | |

SMAA blend weights read the canonical lookup textures: AreaTex `r8g8b8a8_unorm` 160x560
crc `0x1D752069`, SearchTex `r8g8b8a8_unorm` 64x16 crc `0x7EEB940B`. The blend pass binds only
t0 scene colour and t1 weights; slots 2-17 still hold deferred lighting's SRVs, one of which is
its own output, so never scan them for an input.

### Deferred lighting SRV slots (CS `0x7A5E1E66`)

t0 depth, t1 normals, t2 albedo, t3 material, **t4 occlusion**, t5 shadow mask,
t6 BRDF LUT, t7 light buffer, t8 shadow atlas `r16g16_float` 8192x4096, t9 preintegrated,
t10 light list, t11 cookies, t13 reflection probe cube array (2046 layers),
t15-t17 froxel volumes. u0 lit scene.

### Occlusion contract (CS `0x0E7C20A1` -> t4)

Consumer does a 4-tap normal-weighted bilateral gather, then:

    occlusion = 1 - saturate(A)
    ambient   = cb1[15] * R + cb1[16] * G + cb1[17] * B

3-basis directional ambient, not scalar AO. RGB are signed directional weights
(producer uses 1/6), A is scalar occlusion (1/18). Any replacement must write all four
channels or the directional ambient is lost.

Buffer is full res but written as a checkerboard: thread `(x, y)` owns pixel
`(2x + (y & 1), y)`, so only `(x + y) even` is written and the consumer's gather fills
the rest. Sky (linear z >= 14000) writes zero. Replacements must keep this mapping.

RGB is a view-space open direction scaled by ~3x the occlusion, so it goes to zero when
unoccluded. Reconstruction constants: `cb0[6].x`/`cb0[7].y` projection scale,
`cb0[10].xy` resolution, `cb1[11..13].xyz` world-to-view rotation, `cb1[20].zw` depth
linearization. `shaders/game.hlsli` wraps all of these.

### Velocity contract (G-buffer RT4)

PS writes `o4.xy = (curNDC - prevNDC) * float2(-0.5, 0.5)`, i.e. **`uvPrev - uvCur`** — a standard
backward motion vector in UV space. Only dynamic geometry writes it; static pixels keep the clear
value, so the buffer is not full-screen.

### G-buffer VS constants

Row-vector matrices, rows in consecutive registers, matching `mul(v, M)`. The chain is
`local -> mul(cb0[0..3]) -> world -> + cb1[9] -> translated world -> mul(cb1[0..3]) -> clip`.

**cb0 is per draw; cb1 is the shared per-view block.** `cb0[0..3]` is LocalToWorld (the output
register is named `SPRAY_VELOCIY_WS_POS`), so snapshotting it captures one object's model matrix,
not the camera. Previous LocalToWorld sits in a tail whose offset varies per permutation
(`cb0[26..29]` or `cb0[27..30]`) and cannot be located from outside: DevKit reports constant
buffers as handles with `size: null`, and nothing establishes that `ByteWidth` equals `CB0[N]`.
`cb2`/`cb3` are bone matrices, current/previous.

cb1 is `CB1[10]` in every g-buffer VS seen — including static `0x03BEFD54`, which declares no cb0
at all — so a 160-byte copy captures the whole per-view block:

| Registers | Contents |
|---|---|
| `cb1[0..3]` | translated world -> clip, current |
| `cb1[4..7]` | translated world -> clip, **previous frame**. Only `.xyw` is read |
| `cb1[9]` | PreViewTranslation, added to world before `cb1[0..3]` |

`cb1[4..7]` consumes world translated by the **previous** frame's `cb1[9]`, so rebase before
using it. Established by testing, not disassembly.

These are the vertex stage's buffers and share no layout with the compute passes' cb0/cb1.
`cb1[11..13]` being the view rotation is a *compute* fact; the vertex cb1 is unrelated.

### Tonemap (CS `0xB6B56605` off / `0x978BFB09` motion blur on)

t0 scene colour, LUT (16-slice 2D strip, 256x16), bloom, 1x1 average luminance. The motion-blur
variant additionally takes packed velocity/depth and the NeighborMax tiles.

Order: motion blur -> bloom add -> divide by average luminance -> vignette (0.3) ->
Hable -> 1/2.2 gamma -> LUT -> sincos dither. Hable constants
`0.22 / 0.03 / 0.002 / 0.30 / 0.06 / -0.033333` then `* 1.662899`, i.e.
A=0.22 B=0.30 C=0.10 D=0.20 E=0.01 F=0.30, linear white 2.2.
AgX replaces the Hable + gamma steps only.

## Rules

- Keep CLAUDE.md minimal: game facts and hard rules. Implementation rationale goes in code comments.
- Build with `cmake --preset clang-x64` (needs VS LLVM `clang-cl.exe` on PATH). Never bare-configure, it drops `/EHsc`.
- The game symlinks `renodx-batmanak-rend2.addon64` to `build/Debug`, so linking fails while it runs. Hand the build off instead of asking to close it.
- The game records on 5 deferred contexts from 5 worker threads. Per-context state (viewports,
  bindings, pass flags) belongs in command-list private data, never a global.
- A feature is `features/<name>.h` exposing `Settings()`, optionally `Shaders()`, and
  `Register()`/`Unregister()` if it owns runtime state. `addon.cpp` only concatenates.
- Hook engine functions at runtime with Detours (`external/Detours`) only when nothing in the
  render state can be intercepted instead.
- SDR only. Never call `SetUseHDR10` or add a swapchain proxy.
- Change vanilla output only where a feature requires it.
- Read `src/games/batmanak` for shader hashes only. Do not inherit its tonemap, LUT, or UI passes.
- DLSS SDK is vendored at `external/DLSS`; `nvngx_dlss.dll` must sit next to the loaded addon.
