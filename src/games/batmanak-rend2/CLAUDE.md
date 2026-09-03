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

Vanilla tonemapper is Hable/Uncharted2

## Rules

- Keep CLAUDE.md minimal
- Hook engine functions at runtime with Detours (`external/Detours`).
- SDR only. Never call `SetUseHDR10` or add a swapchain proxy.
- Change vanilla output only where a feature requires it.
- Read `src/games/batmanak` for shader hashes only. Do not inherit its tonemap, LUT, or UI passes.
