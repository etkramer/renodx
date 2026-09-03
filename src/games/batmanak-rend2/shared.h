#ifndef SRC_GAMES_BATMANAK_REND2_SHARED_H_
#define SRC_GAMES_BATMANAK_REND2_SHARED_H_

// Must be 32bit aligned
// Should be 4x32
struct ShaderInjectData {
  float debug_view;
  float gtao_enabled;
  float gtao_intensity;
  float gtao_radius;

  float gtao_quality;
  float reserved_0;
  float reserved_1;
  float reserved_2;
};

#ifdef __cplusplus

// Single instance shared by the addon and every feature header.
inline ShaderInjectData shader_injection = {};

#else

cbuffer cb13 : register(b13) {
  ShaderInjectData shader_injection : packoffset(c0);
}

#define CUSTOM_DEBUG_VIEW     shader_injection.debug_view
#define CUSTOM_GTAO_INTENSITY shader_injection.gtao_intensity
#define CUSTOM_GTAO_RADIUS    shader_injection.gtao_radius
#define CUSTOM_GTAO_QUALITY   shader_injection.gtao_quality

#include "../../shaders/renodx.hlsl"

#endif

#endif  // SRC_GAMES_BATMANAK_REND2_SHARED_H_
