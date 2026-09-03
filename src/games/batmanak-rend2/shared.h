#ifndef SRC_GAMES_BATMANAK_REND2_SHARED_H_
#define SRC_GAMES_BATMANAK_REND2_SHARED_H_

// Must be 32bit aligned
// Should be 4x32
struct ShaderInjectData {
  float debug_view;
  float reserved_0;
  float reserved_1;
  float reserved_2;
};

#ifndef __cplusplus
cbuffer cb13 : register(b13) {
  ShaderInjectData shader_injection : packoffset(c0);
}

#define CUSTOM_DEBUG_VIEW shader_injection.debug_view

#include "../../shaders/renodx.hlsl"

#endif

#endif  // SRC_GAMES_BATMANAK_REND2_SHARED_H_
