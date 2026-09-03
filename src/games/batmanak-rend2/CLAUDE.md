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

Verified by DevKit snapshot. Resource handles are per-run; formats are stable.
Draw indices drift between captures. Anchor on shader hashes, never on indices.

Shader hashes are per-permutation. Graphics settings swap whole shaders, so a hash
found in one capture may not be bound in another. See "Setting permutations".

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
| **SMAA 1x** | PS `0x8BE4180B` -> PS `0x2F55F98E` -> CS `0x42C0137E` | edges -> weights -> blend. Purely spatial |
| DOF/bloom prefilter | CS `0xB2A276FD` | -> 3x half-res |
| Bloom pyramid | CS `0x0E6435F4` x8, then `0x2B46745A` alternating | |
| Lens flare | CS `0xB42A7F40` | matches `batmanak`, so hashes are stable across this build |
| Motion blur setup | CS `0xF98BC308`, `0x98C5C8CA` | motion blur on only |
| DOF composite | CS `0xB07D74F7` / `0x8A3FA02D` | chromatic aberration + far blur |
| **Tonemap** | CS `0xB6B56605` / `0x978BFB09` | see below |
| UI | many PS, mostly `0x8C27F313` | |

SMAA blend weights read the canonical lookup textures: AreaTex `r8g8b8a8_unorm` 160x560
crc `0x1D752069`, SearchTex `r8g8b8a8_unorm` 64x16 crc `0x7EEB940B`.

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
(producer uses 1/6), A is scalar occlusion (1/18). Any GTAO replacement must write
all four channels or the directional ambient is lost.

Buffer is full res but written as a checkerboard: thread `(x, y)` owns pixel
`(2x + (y & 1), y)`, so only `(x + y) even` is written and the consumer's gather fills
the rest. Sky (linear z >= 14000) writes zero. Replacements must keep this mapping.

RGB is a view-space open direction scaled by ~3x the occlusion, so it goes to zero when
unoccluded. Reconstruction constants: `cb0[6].x`/`cb0[7].y` projection scale,
`cb0[10].xy` resolution, `cb1[11..13].xyz` world-to-view rotation, `cb1[20].zw` depth
linearization. `shaders/game.hlsli` wraps all of these.

### Velocity contract (G-buffer RT4)

PS writes `o4.xy = (curNDC - prevNDC) * float2(-0.5, 0.5)`, i.e. **`uvPrev - uvCur`** —
a standard backward motion vector in UV space. VS feeds it `SPRAY_VELOCIY_WS_POS` and
`SPRAY_VELOCIY_PREV_WS_POS` (their typo).

Only dynamic geometry writes it. Static pixels keep the clear value, so the buffer is
not full-screen. DLSS/DLAA needs camera motion synthesised from depth + previous
view-projection and merged with RT4.

Motion blur setup CS `0xF98BC308` reads depth t0 + velocity t1 and writes
`u0 = float4(velocity * -0.25, linearDepth, 0)` full res, clamped to +-16 px, plus a
16x16 TileMax in `u1` (228x128). CS `0x98C5C8CA` does NeighborMax 3x3 over that and
puts a 0-4 sample count in `.z`. The gather itself is fused into the tonemap CS.

### Tonemap (CS `0xB6B56605` off / `0x978BFB09` motion blur on)

t0 scene colour, LUT (16-slice 2D strip, 256x16), bloom, 1x1 average luminance.
The motion-blur variant additionally takes packed velocity/depth and the NeighborMax
tiles, and gathers 0-2 samples along velocity before anything else.

Order: motion blur -> bloom add -> divide by average luminance -> vignette (0.3) ->
Hable -> 1/2.2 gamma -> LUT -> sincos dither. Hable constants
`0.22 / 0.03 / 0.002 / 0.30 / 0.06 / -0.033333` then `* 1.662899`, i.e.
A=0.22 B=0.30 C=0.10 D=0.20 E=0.01 F=0.30, linear white 2.2.
AgX replaces the Hable + gamma steps only.

### Setting permutations

Graphics settings recompile passes into different shaders. Known so far:

| Pass | Motion blur off | Motion blur on |
|---|---|---|
| DOF composite | `0xB07D74F7` | `0x8A3FA02D` |
| Tonemap | `0xB6B56605` | `0x978BFB09` |

Assume more permutations exist. Any shader replacement must cover every variant of a
pass, or the mod silently does nothing when a setting changes.

## Rules

- Keep CLAUDE.md minimal
- Build with `cmake --preset clang-x64` (needs VS LLVM `clang-cl.exe` on PATH). Never bare-configure, it drops `/EHsc`.
- The game symlinks `renodx-batmanak-rend2.addon64` to `build/Debug`, so linking fails while it runs. Hand the build off instead of asking to close it.
- Hook engine functions at runtime with Detours (`external/Detours`).
- SDR only. Never call `SetUseHDR10` or add a swapchain proxy.
- Change vanilla output only where a feature requires it.
- Read `src/games/batmanak` for shader hashes only. Do not inherit its tonemap, LUT, or UI passes.
