// Synthesises full-res DLSS motion vectors: camera motion reprojected through the game's own
// previous view-projection, overridden by RT4 where the velocity pass wrote one.

#include "./dlaa_shared.h"

Texture2D<float4> t0 : register(t0);  // depth
Texture2D<float4> t1 : register(t1);  // velocity RT4, dynamic geometry only

cbuffer cb0 : register(b0) { float4 cb0[11]; }  // occlusion CS view constants

// The g-buffer VS shared per-view block, all 10 registers of it. [0..3] takes translated world to
// clip, [4..7] takes last frame's translated world to last frame's clip, [9] is PreViewTranslation.
// Double buffered only for [9], to rebase this frame's translation onto the previous one.
cbuffer view_current : register(b3) { float4 view_constants[10]; }
cbuffer view_previous : register(b4) { float4 view_previous[10]; }

RWTexture2D<float2> u0 : register(u0);  // motion vectors, pixel space
RWTexture2D<float> u1 : register(u1);   // raw depth copy, typed for NGX
RWTexture2D<float4> u2 : register(u2);  // debug visualisation

float4x4 Invert(float4x4 m) {
  float a00 = m[0][0], a01 = m[0][1], a02 = m[0][2], a03 = m[0][3];
  float a10 = m[1][0], a11 = m[1][1], a12 = m[1][2], a13 = m[1][3];
  float a20 = m[2][0], a21 = m[2][1], a22 = m[2][2], a23 = m[2][3];
  float a30 = m[3][0], a31 = m[3][1], a32 = m[3][2], a33 = m[3][3];

  float b00 = a00 * a11 - a01 * a10;
  float b01 = a00 * a12 - a02 * a10;
  float b02 = a00 * a13 - a03 * a10;
  float b03 = a01 * a12 - a02 * a11;
  float b04 = a01 * a13 - a03 * a11;
  float b05 = a02 * a13 - a03 * a12;
  float b06 = a20 * a31 - a21 * a30;
  float b07 = a20 * a32 - a22 * a30;
  float b08 = a20 * a33 - a23 * a30;
  float b09 = a21 * a32 - a22 * a31;
  float b10 = a21 * a33 - a23 * a31;
  float b11 = a22 * a33 - a23 * a32;

  float scale = 1.f / (b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06);

  float4x4 result;
  result[0][0] = (a11 * b11 - a12 * b10 + a13 * b09) * scale;
  result[0][1] = (a02 * b10 - a01 * b11 - a03 * b09) * scale;
  result[0][2] = (a31 * b05 - a32 * b04 + a33 * b03) * scale;
  result[0][3] = (a22 * b04 - a21 * b05 - a23 * b03) * scale;
  result[1][0] = (a12 * b08 - a10 * b11 - a13 * b07) * scale;
  result[1][1] = (a00 * b11 - a02 * b08 + a03 * b07) * scale;
  result[1][2] = (a32 * b02 - a30 * b05 - a33 * b01) * scale;
  result[1][3] = (a20 * b05 - a22 * b02 + a23 * b01) * scale;
  result[2][0] = (a10 * b10 - a11 * b08 + a13 * b06) * scale;
  result[2][1] = (a01 * b08 - a00 * b10 - a03 * b06) * scale;
  result[2][2] = (a30 * b04 - a31 * b02 + a33 * b00) * scale;
  result[2][3] = (a21 * b02 - a20 * b04 - a23 * b00) * scale;
  result[3][0] = (a11 * b07 - a10 * b09 - a12 * b06) * scale;
  result[3][1] = (a00 * b09 - a01 * b07 + a02 * b06) * scale;
  result[3][2] = (a31 * b01 - a30 * b03 - a32 * b00) * scale;
  result[3][3] = (a20 * b03 - a21 * b01 + a22 * b00) * scale;
  return result;
}

[numthreads(8, 8, 1)] void main(uint2 thread_id : SV_DispatchThreadID) {
  float2 dimensions = cb0[10].xy;
  if (any(float2(thread_id) >= dimensions)) return;

  float raw_depth = t0.Load(int3(thread_id, 0)).x;
  u1[thread_id] = raw_depth;

  // Un-jittered pixel centre on purpose. Used consistently here and in the subtraction below the
  // jitter cancels; correcting only one of the two sites would be worse than correcting neither.
  float2 current_ndc = (float2(thread_id) + 0.5f) * (float2(2.f, -2.f) / dimensions)
                       + float2(-1.f, 1.f);

  float4x4 to_clip = float4x4(view_constants[0], view_constants[1], view_constants[2],
                              view_constants[3]);
  float4x4 to_previous_clip = float4x4(view_constants[4], view_constants[5], view_constants[6],
                                       view_constants[7]);

  // Any scale of the clip position inverts to the same point once divided out, so w = 1 works.
  float4 translated_world = mul(float4(current_ndc, raw_depth, 1.f), Invert(to_clip));
  float3 world = translated_world.xyz / translated_world.w;
  world += view_previous[9].xyz - view_constants[9].xyz;
  float4 previous_clip = mul(float4(world, 1.f), to_previous_clip);

  bool behind_camera = previous_clip.w <= 0.f;
  float2 camera_motion = 0.f;
  if (!behind_camera) {
    float2 previous_ndc = previous_clip.xy / previous_clip.w;
    camera_motion = (previous_ndc - current_ndc) * float2(0.5f, -0.5f) * dimensions;
  }

  float2 velocity = t1.Load(int3(thread_id, 0)).xy;
  // Scaled by the resolution, a denormal or stale RT4 value becomes a large vector, so require
  // motion that survives rounding to a hundredth of a pixel before trusting it over the camera.
  bool has_object_motion = any(abs(velocity) > 1e-6f);
  float2 object_motion = velocity * dimensions;

  float2 motion = has_object_motion ? object_motion : camera_motion;
  // NaN and infinity both fail this, and either one makes DLSS drop the geometry for a frame.
  motion = all(abs(motion) < 1e30f) ? clamp(motion, -dimensions, dimensions) : 0.f;
  u0[thread_id] = motion;

  if (dlaa_debug_view == DLAA_DEBUG_MOTION) {
    // Blue marks the behind-the-previous-camera guard, which zeroes motion and would otherwise
    // be indistinguishable from a genuinely still pixel.
    u2[thread_id] = float4(saturate(0.5f + motion * 0.05f), behind_camera ? 1.f : 0.f, 1.f);
  }
}
