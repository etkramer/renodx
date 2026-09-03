#ifndef SRC_GAMES_BATMANAK_REND2_FEATURES_GTAO_H_
#define SRC_GAMES_BATMANAK_REND2_FEATURES_GTAO_H_

#include <embed/shaders.h>

#include "../../../mods/shader.hpp"
#include "../../../utils/settings.hpp"
#include "../shared.h"

// Replaces the stock directional occlusion pass (CS 0x0E7C20A1) with GTAO.
// Disabling the feature declines the replacement, so the stock pass runs untouched.
namespace features::gtao {

inline bool IsEnabled() {
  return shader_injection.gtao_enabled != 0.f;
}

inline renodx::utils::settings::Settings Settings() {
  return {
      new renodx::utils::settings::Setting{
          .key = "GtaoEnabled",
          .binding = &shader_injection.gtao_enabled,
          .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
          .default_value = 1.f,
          .label = "GTAO",
          .section = "Ambient Occlusion",
          .tooltip = "Replaces the stock 6-tap directional occlusion with ground-truth ambient occlusion.",
      },
      new renodx::utils::settings::Setting{
          .key = "GtaoQuality",
          .binding = &shader_injection.gtao_quality,
          .value_type = renodx::utils::settings::SettingValueType::INTEGER,
          .default_value = 1.f,
          .label = "Quality",
          .section = "Ambient Occlusion",
          .tooltip = "Slice and step count. Higher is smoother and more expensive.",
          .labels = {"Low", "Medium", "High", "Ultra"},
          .is_enabled = IsEnabled,
      },
      new renodx::utils::settings::Setting{
          .key = "GtaoIntensity",
          .binding = &shader_injection.gtao_intensity,
          .default_value = 100.f,
          .label = "Intensity",
          .section = "Ambient Occlusion",
          .tooltip = "Scales occlusion strength. 100% is closest to vanilla.",
          .min = 0.f,
          .max = 300.f,
          .format = "%.0f%%",
          .is_enabled = IsEnabled,
          .parse = [](float value) { return value * 0.01f; },
      },
      new renodx::utils::settings::Setting{
          .key = "GtaoRadius",
          .binding = &shader_injection.gtao_radius,
          .default_value = 100.f,
          .label = "Radius",
          .section = "Ambient Occlusion",
          .tooltip = "Scales the world-space search radius, which grows with distance as in vanilla.",
          .min = 25.f,
          .max = 400.f,
          .format = "%.0f%%",
          .is_enabled = IsEnabled,
          .parse = [](float value) { return value * 0.01f; },
      },
  };
}

inline renodx::mods::shader::CustomShaders Shaders() {
  return renodx::mods::shader::DefineCustomShaders({
      CustomShaderEntryCallback(0x0E7C20A1, [](reshade::api::command_list*) { return IsEnabled(); }),
  });
}

}  // namespace features::gtao

#endif  // SRC_GAMES_BATMANAK_REND2_FEATURES_GTAO_H_
