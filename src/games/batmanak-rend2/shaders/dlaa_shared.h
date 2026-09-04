#ifndef SRC_GAMES_BATMANAK_REND2_SHADERS_DLAA_SHARED_H_
#define SRC_GAMES_BATMANAK_REND2_SHADERS_DLAA_SHARED_H_

// Layout shared by the motion vector pass and features/dlaa.h.

#define DLAA_DEBUG_OFF    0
#define DLAA_DEBUG_MOTION 1

#ifndef __cplusplus

cbuffer dlaa_cb : register(b2) {
  uint dlaa_debug_view;
  uint3 dlaa_pad;
};

#endif

#endif  // SRC_GAMES_BATMANAK_REND2_SHADERS_DLAA_SHARED_H_
