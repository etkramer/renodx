#ifndef SRC_GAMES_BATMANAK_REND2_SHADERS_GAME_HLSLI_
#define SRC_GAMES_BATMANAK_REND2_SHADERS_GAME_HLSLI_

// Engine-space helpers shared by every replaced pass.
//
// View space is right-handed with +z pointing away from the camera, and linear z is in world
// units. The passes that reconstruct position all bind the same constants:
//   proj         = float2(cb0[6].x, cb0[7].y)   projection scale terms
//   dimensions   = cb0[10].xy                   target resolution in pixels
//   depth_params = cb1[20].zw                   depth linearization
//   view rows    = cb1[11..13].xyz              rotation part of world-to-view

float LinearizeDepth(float raw_depth, float2 depth_params) {
  return 1.f / (min(raw_depth, 1.f) * depth_params.x - depth_params.y);
}

float3 ViewPositionFromPixel(float2 pixel, float linear_z, float2 dimensions, float2 proj) {
  float2 ndc = (pixel + 0.5f) * (float2(2.f, -2.f) / dimensions) + float2(-1.f, 1.f);
  return float3(linear_z * ndc / proj, linear_z);
}

float2 PixelFromViewPosition(float3 view_position, float2 dimensions, float2 proj) {
  float2 ndc = (view_position.xy / view_position.z) * proj;
  return (ndc + float2(1.f, -1.f)) / (float2(2.f, -2.f) / dimensions) - 0.5f;
}

// Rate of change of view-space xy per pixel, per unit of linear z.
float2 ViewPerPixel(float2 dimensions, float2 proj) {
  return float2(2.f, -2.f) / (dimensions * proj);
}

float3 DecodeWorldNormal(float3 encoded) {
  return normalize(encoded * 2.f - 1.f);
}

float3 WorldToViewDirection(float3 direction, float3 row0, float3 row1, float3 row2) {
  return direction.x * row0 + direction.y * row1 + direction.z * row2;
}

#endif  // SRC_GAMES_BATMANAK_REND2_SHADERS_GAME_HLSLI_
