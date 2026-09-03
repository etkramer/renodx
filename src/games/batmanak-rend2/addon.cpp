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
#include "./features/gtao.h"
#include "./shared.h"

namespace {

// Each feature owns its settings and shader replacements; this concatenates them.
renodx::utils::settings::Settings BuildSettings() {
  renodx::utils::settings::Settings settings;
  for (const auto& feature : {features::gtao::Settings()}) {
    settings.insert(settings.end(), feature.begin(), feature.end());
  }
  return settings;
}

renodx::mods::shader::CustomShaders BuildShaders() {
  renodx::mods::shader::CustomShaders shaders;
  for (const auto& feature : {features::gtao::Shaders()}) {
    shaders.insert(feature.begin(), feature.end());
  }
  return shaders;
}

renodx::utils::settings::Settings settings = BuildSettings();
renodx::mods::shader::CustomShaders custom_shaders = BuildShaders();

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
