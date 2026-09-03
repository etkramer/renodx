/*
 * Copyright (C) 2026 Eli Kramer
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#include <deps/imgui/imgui.h>
#include <embed/shaders.h>
#include <include/reshade.hpp>

#include "../../mods/shader.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

ShaderInjectData shader_injection;

// Populated once DevKit identifies stable target hashes.
renodx::mods::shader::CustomShaders custom_shaders = {};

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .key = "DebugView",
        .binding = &shader_injection.debug_view,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Debug View",
        .section = "Debug",
        .tooltip = "Displays an intermediate buffer instead of the final image.",
        .labels = {"Off"},
    },
};

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX Rend2";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "Rendering features for Batman: Arkham Knight";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;
      // Keep settings out of the stock batmanak mod's [renodx-preset*] namespace.
      renodx::utils::settings::global_name = "batmanak-rend2";
      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
