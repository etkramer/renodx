// Replaces the stock 6-tap directional occlusion pass with GTAO.
//
// Output contract is the stock one (see CLAUDE.md): the deferred lighting CS reads
// occlusion as 1 - saturate(a) and the directional ambient as cb1[15]*r + cb1[16]*g +
// cb1[17]*b, so rgb must stay a view-space open direction scaled by ~3x the occlusion.
//
// Like the stock pass this writes a checkerboard: thread (x, y) owns pixel
// (2x + (y & 1), y), and the consumer's 4-tap bilateral gather fills the rest.

#include "../shared.h"
#include "./game.hlsli"

Texture2D<float4> t0 : register(t0);  // depth
Texture2D<float4> t1 : register(t1);  // world normals

cbuffer cb0 : register(b0) { float4 cb0[11]; }
cbuffer cb1 : register(b1) { float4 cb1[21]; }

RWTexture2D<float4> u0 : register(u0);

static const float PI = 3.14159265f;
static const float HALF_PI = 1.57079633f;

// Depth at or past this is sky; the stock pass writes zero there.
static const float FAR_DISTANCE = 14000.f;

float SpatialNoise(float2 pixel) {
  return frac(52.9829189f * frac(dot(pixel, float2(0.06711056f, 0.00583715f))));
}

// Largest cosine between the view vector and any sample along slice_dir.
float SearchHorizon(
    float2 pixel,
    float3 view_position,
    float3 view_dir,
    float2 slice_dir,
    float pixel_radius,
    float world_radius,
    float2 dimensions,
    float2 proj,
    float2 depth_params,
    float noise,
    uint step_count) {
  float best = -1.f;
  for (uint i = 0; i < step_count; i++) {
    // Quadratic spacing keeps taps dense near the centre where the horizon usually is.
    float t = (i + noise + 1.f) / (step_count + 1.f);
    float2 offset = slice_dir * (t * t * pixel_radius);
    int2 texel = int2(clamp(pixel + offset, 0.f, dimensions - 1.f));

    float sample_z = LinearizeDepth(t0.Load(int3(texel, 0)).x, depth_params);
    float3 delta = ViewPositionFromPixel(texel, sample_z, dimensions, proj) - view_position;
    float distance = length(delta);
    float cos_h = dot(delta, view_dir) / max(distance, 1e-6f);

    // Fade a tap out between one and two radii so distant geometry stops occluding.
    best = max(best, lerp(cos_h, -1.f, saturate(distance / world_radius - 1.f)));
  }
  return best;
}

[numthreads(16, 16, 1)] void main(uint2 thread_id : SV_DispatchThreadID) {
  float2 dimensions = cb0[10].xy;
  float2 pixel = float2(thread_id.x * 2u + (thread_id.y & 1u), thread_id.y);
  if (any(pixel >= dimensions)) return;

  uint2 target = uint2(pixel);
  float2 proj = float2(cb0[6].x, cb0[7].y);
  float2 depth_params = cb1[20].zw;

  float linear_z = LinearizeDepth(t0.Load(int3(target, 0)).x, depth_params);
  if (linear_z >= FAR_DISTANCE) {
    u0[target] = 0.f;
    return;
  }

  float3 view_position = ViewPositionFromPixel(pixel, linear_z, dimensions, proj);
  float3 world_normal = DecodeWorldNormal(t1.Load(int3(target, 0)).xyz);
  float3 normal = WorldToViewDirection(world_normal, cb1[11].xyz, cb1[12].xyz, cb1[13].xyz);
  float3 view_dir = normalize(-view_position);

  // Stock radius grows with distance; the slider scales it.
  float world_radius = (linear_z * 0.01f + 4.f) * CUSTOM_GTAO_RADIUS;
  float pixel_radius = clamp(world_radius * proj.x * dimensions.x * 0.5f / linear_z, 2.f, 192.f);

  uint quality = uint(CUSTOM_GTAO_QUALITY);
  uint slice_count = 2u + quality;
  uint step_count = 3u + quality;

  float noise_slice = SpatialNoise(pixel);
  float noise_step = SpatialNoise(pixel + 91.f);

  float2 view_per_pixel = ViewPerPixel(dimensions, proj);
  float visibility = 0.f;
  float3 bent = 0.f;

  for (uint slice = 0; slice < slice_count; slice++) {
    float phi = (slice + noise_slice) * (PI / slice_count);
    float2 slice_dir = float2(cos(phi), sin(phi));

    float3 direction = normalize(float3(slice_dir * view_per_pixel, 0.f));
    float3 axis = normalize(cross(direction, view_dir));
    float3 ortho = normalize(direction - view_dir * dot(direction, view_dir));

    float3 projected = normal - axis * dot(normal, axis);
    float projected_length = length(projected);
    if (projected_length < 1e-4f) continue;

    // Signed tilt of the projected normal away from the view vector, within the slice plane.
    float3 projected_dir = projected / projected_length;
    float n = atan2(dot(projected_dir, ortho), dot(projected_dir, view_dir));

    float cos_back = SearchHorizon(pixel, view_position, view_dir, -slice_dir, pixel_radius,
                                   world_radius, dimensions, proj, depth_params, noise_step, step_count);
    float cos_front = SearchHorizon(pixel, view_position, view_dir, slice_dir, pixel_radius,
                                    world_radius, dimensions, proj, depth_params, noise_step, step_count);

    float h0 = n + max(-acos(clamp(cos_back, -1.f, 1.f)) - n, -HALF_PI);
    float h1 = n + min(acos(clamp(cos_front, -1.f, 1.f)) - n, HALF_PI);

    float cos_n = cos(n);
    float sin_n = sin(n);
    float arc0 = -cos(2.f * h0 - n) + cos_n + 2.f * h0 * sin_n;
    float arc1 = -cos(2.f * h1 - n) + cos_n + 2.f * h1 * sin_n;
    visibility += projected_length * 0.25f * (arc0 + arc1);

    float bent_angle = (h0 + h1) * 0.5f;
    bent += projected_length * (cos(bent_angle) * view_dir + sin(bent_angle) * ortho);
  }

  visibility = saturate(visibility / slice_count);

  float occlusion = saturate((1.f - visibility) * CUSTOM_GTAO_INTENSITY);
  float3 open_dir = dot(bent, bent) > 1e-8f ? normalize(bent) : normal;

  u0[target] = float4(open_dir * (3.f * occlusion), occlusion);
}
