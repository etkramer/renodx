#ifndef SRC_GAMES_BATMANAK_REND2_FEATURES_DLAA_H_
#define SRC_GAMES_BATMANAK_REND2_FEATURES_DLAA_H_

#include <embed/shaders.h>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "../../../mods/shader.hpp"
#include "../../../utils/data.hpp"
#include "../../../utils/settings.hpp"
#include "../../../utils/shader.hpp"
#include "../runtime/d3d11.h"
#include "../shaders/dlaa_shared.h"

// Replaces the SMAA 1x chain with DLSS in DLAA mode. The game has no temporal AA, so
// motion vectors are synthesised and sub-pixel jitter is added via the viewport.
namespace features::dlaa {

inline float enabled = 0.f;
inline float jitter_enabled = 0.f;
inline float debug_view = 0.f;

namespace internal {

constexpr uint32_t SMAA_EDGES = 0x8BE4180B;
constexpr uint32_t SMAA_WEIGHTS = 0x2F55F98E;
constexpr uint32_t SMAA_BLEND = 0x42C0137E;
constexpr uint32_t OCCLUSION = 0x0E7C20A1;

struct Constants {
  uint32_t debug_view;
  uint32_t pad[3];
};

struct Jitter {
  float x = 0.f;
  float y = 0.f;
};

// The game records on five deferred contexts from five worker threads, so viewport and pass state
// is per command list. Sharing it globally lets one thread bind another's viewport.
struct __declspec(uuid("6d1f0a54-3b92-4c77-9a10-58e2c4d7b3f1")) CommandListData {
  reshade::api::viewport base_viewport = {};
  bool has_base_viewport = false;
  bool velocity_pass_active = false;
  // Compared against the current frame, so the phase resets without touching every command list.
  uint32_t geometry_ended_frame = UINT32_MAX;
  // Latched at the resolve so the eval reports the offset the g-buffer was actually rasterised with.
  Jitter eval_jitter = {};
};

inline CommandListData* GetData(reshade::api::command_list* cmd_list) {
  CommandListData* data = nullptr;
  renodx::utils::data::CreateOrGet<CommandListData>(cmd_list, data);
  return data;
}

struct State {
  HMODULE addon_module = nullptr;

  bool ngx_attempted = false;
  bool ngx_ready = false;
  NVSDK_NGX_Parameter* ngx_parameters = nullptr;
  NVSDK_NGX_Handle* dlss_handle = nullptr;
  uint64_t dlss_signature = 0;

  runtime::d3d11::ComPtr<ID3D11ComputeShader> motion_vector_shader;
  runtime::d3d11::ComPtr<ID3D11Buffer> constants;
  runtime::d3d11::ComPtr<ID3D11Buffer> cb0_snapshot;
  runtime::d3d11::ComPtr<ID3D11Buffer> cb1_snapshot;
  runtime::d3d11::ComPtr<ID3D11Buffer> view_cb[2];
  runtime::d3d11::Texture motion_vectors;
  runtime::d3d11::Texture depth_copy;

  uint32_t width = 0;
  uint32_t height = 0;
  std::atomic<uint32_t> frame_index = 0;
  bool snapshots_valid = false;
  std::atomic<bool> view_snapshot_taken = false;
  uint32_t view_snapshot_index = 0;
  uint32_t view_snapshot_count = 0;
  std::atomic<bool> velocity_captured = false;
  bool needs_reset = true;
  // Sticky: once DLAA cannot run we stop bypassing SMAA so the game keeps its own AA.
  bool failed = false;

  // Packed, so a viewport bind cannot pair one frame's x with the next frame's y. Changes only at
  // the resolve, which is ordered against the geometry recording that reads it; Present is not.
  std::atomic<Jitter> jitter = Jitter{};
  std::atomic<uint32_t> jitter_phase = 0;
  std::atomic<bool> jitter_applied = false;

  runtime::d3d11::ComPtr<ID3D11ShaderResourceView> depth_srv;
  ID3D11Resource* velocity_resource = nullptr;
  ID3D11Resource* velocity_srv_resource = nullptr;
  runtime::d3d11::ComPtr<ID3D11ShaderResourceView> velocity_srv;
};

inline State state;

inline void Log(reshade::log::level level, const std::string& message) {
  reshade::log::message(level, ("dlaa: " + message).c_str());
}

inline float Halton(uint32_t index, uint32_t base) {
  float result = 0.f;
  float fraction = 1.f;
  while (index > 0u) {
    fraction /= static_cast<float>(base);
    result += fraction * static_cast<float>(index % base);
    index /= base;
  }
  return result;
}

inline Jitter NextJitter(uint32_t phase) {
  uint32_t index = (phase % 8u) + 1u;
  return {Halton(index, 2u) - 0.5f, Halton(index, 3u) - 0.5f};
}

inline std::wstring AddonDirectory() {
  wchar_t path[MAX_PATH] = {};
  if (GetModuleFileNameW(state.addon_module, path, MAX_PATH) == 0) return L".";
  std::wstring full = path;
  auto separator = full.find_last_of(L"\\/");
  return separator == std::wstring::npos ? L"." : full.substr(0, separator);
}

inline void InitializeNgx(ID3D11Device* device) {
  if (state.ngx_attempted) return;
  state.ngx_attempted = true;

  static std::wstring directory = AddonDirectory();
  const wchar_t* search_paths[] = {directory.c_str()};

  NVSDK_NGX_FeatureCommonInfo common_info = {};
  common_info.PathListInfo.Path = search_paths;
  common_info.PathListInfo.Length = 1;

  auto result = NVSDK_NGX_D3D11_Init_with_ProjectID(
      "a4f0d2c6-91b7-4c1a-9d3e-5f8a2b6c7e10", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
      directory.c_str(), device, &common_info, NVSDK_NGX_Version_API);
  if (NVSDK_NGX_FAILED(result)) {
    Log(reshade::log::level::warning, "NGX init failed, DLAA unavailable");
    return;
  }

  if (NVSDK_NGX_FAILED(NVSDK_NGX_D3D11_GetCapabilityParameters(&state.ngx_parameters))) {
    Log(reshade::log::level::warning, "NGX capability parameters unavailable");
    return;
  }

  int available = 0;
  state.ngx_parameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
  if (available == 0) {
    Log(reshade::log::level::warning, "DLSS not available on this device");
    return;
  }

  state.ngx_ready = true;
  Log(reshade::log::level::info, "NGX initialised");
}

inline void ReleaseDlssFeature() {
  if (state.dlss_handle != nullptr) {
    NVSDK_NGX_D3D11_ReleaseFeature(state.dlss_handle);
    state.dlss_handle = nullptr;
  }
  state.dlss_signature = 0;
}

inline bool EnsureDlssFeature(ID3D11DeviceContext* context) {
  uint64_t signature = (static_cast<uint64_t>(state.width) << 32) | state.height;
  if (state.dlss_handle != nullptr && state.dlss_signature == signature) return true;

  ReleaseDlssFeature();

  // UE3 predates reversed-Z, so depth runs 0 near to 1 far and DepthInverted stays off.
  int flags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
              | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;

  NVSDK_NGX_DLSS_Create_Params create = {};
  create.Feature.InWidth = state.width;
  create.Feature.InHeight = state.height;
  create.Feature.InTargetWidth = state.width;
  create.Feature.InTargetHeight = state.height;
  create.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
  create.InFeatureCreateFlags = flags;

  auto result = NGX_D3D11_CREATE_DLSS_EXT(context, &state.dlss_handle, state.ngx_parameters,
                                          &create);
  if (NVSDK_NGX_FAILED(result)) {
    Log(reshade::log::level::error, "DLSS feature creation failed");
    state.dlss_handle = nullptr;
    return false;
  }

  state.dlss_signature = signature;
  state.needs_reset = true;
  Log(reshade::log::level::info, "DLSS feature created");
  return true;
}

inline bool EnsureResources(ID3D11Device* device, uint32_t width, uint32_t height) {
  if (state.motion_vector_shader == nullptr) {
    if (!runtime::d3d11::CreateComputeShader(device, __motion_vectors,
                                             &state.motion_vector_shader)) {
      Log(reshade::log::level::error, "motion vector shader creation failed");
      return false;
    }
    if (!runtime::d3d11::CreateConstantBuffer(device, sizeof(Constants), &state.constants)) {
      return false;
    }
    if (!runtime::d3d11::SupportsTypedUnorderedAccessStore(device, DXGI_FORMAT_R16G16_FLOAT)) {
      Log(reshade::log::level::warning, "R16G16_FLOAT typed UAV store unsupported");
    }
  }

  if (state.width == width && state.height == height && state.motion_vectors.texture != nullptr) {
    return true;
  }

  if (!runtime::d3d11::CreateTexture(device, width, height, DXGI_FORMAT_R16G16_FLOAT,
                                     &state.motion_vectors)) {
    Log(reshade::log::level::error, "motion vector texture creation failed");
    return false;
  }
  if (!runtime::d3d11::CreateTexture(device, width, height, DXGI_FORMAT_R32_FLOAT,
                                     &state.depth_copy)) {
    Log(reshade::log::level::error, "depth copy texture creation failed");
    return false;
  }

  state.width = width;
  state.height = height;
  state.needs_reset = true;
  return true;
}

// Caches an SRV for a game resource we only ever see as a render target.
inline ID3D11ShaderResourceView* EnsureSrv(ID3D11Device* device, ID3D11Resource* resource,
                                           ID3D11Resource** cached_resource,
                                           runtime::d3d11::ComPtr<ID3D11ShaderResourceView>* cached) {
  if (resource == nullptr) return nullptr;
  if (*cached_resource == resource && *cached != nullptr) return cached->Get();
  cached->Reset();
  if (FAILED(device->CreateShaderResourceView(resource, nullptr, cached->GetAddressOf()))) {
    *cached_resource = nullptr;
    return nullptr;
  }
  *cached_resource = resource;
  return cached->Get();
}

inline bool IsSceneColour(ID3D11ShaderResourceView* srv, uint32_t width, uint32_t height) {
  if (srv == nullptr) return false;
  runtime::d3d11::ComPtr<ID3D11Resource> resource;
  srv->GetResource(&resource);
  runtime::d3d11::ComPtr<ID3D11Texture2D> texture;
  if (FAILED(resource.As(&texture))) return false;

  D3D11_TEXTURE2D_DESC desc = {};
  texture->GetDesc(&desc);
  if (desc.Width != width || desc.Height != height) return false;
  return desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT
         || desc.Format == DXGI_FORMAT_R11G11B10_FLOAT;
}

inline void SnapshotConstantBuffers(reshade::api::command_list* cmd_list) {
  auto* device = runtime::d3d11::GetDevice(cmd_list->get_device());
  auto* context = runtime::d3d11::GetContext(cmd_list);
  if (device == nullptr || context == nullptr) return;

  ID3D11Buffer* buffers[2] = {};
  context->CSGetConstantBuffers(0, 2, buffers);
  if (buffers[0] == nullptr || buffers[1] == nullptr) {
    for (auto* buffer : buffers) {
      if (buffer != nullptr) buffer->Release();
    }
    return;
  }

  if (state.cb0_snapshot == nullptr) {
    runtime::d3d11::CreateConstantBufferSnapshot(device, buffers[0], &state.cb0_snapshot);
    runtime::d3d11::CreateConstantBufferSnapshot(device, buffers[1], &state.cb1_snapshot);
  }
  if (state.cb0_snapshot != nullptr && state.cb1_snapshot != nullptr) {
    context->CopyResource(state.cb0_snapshot.Get(), buffers[0]);
    context->CopyResource(state.cb1_snapshot.Get(), buffers[1]);
    state.snapshots_valid = true;
  }

  ID3D11ShaderResourceView* srvs[1] = {};
  context->CSGetShaderResources(0, 1, srvs);
  if (srvs[0] != nullptr) {
    state.depth_srv = srvs[0];
    srvs[0]->Release();
  }

  for (auto* buffer : buffers) buffer->Release();
}

// cb1 is the shared per-view block: every g-buffer VS declares it as CB1[10] with the same layout,
// including 0x03BEFD54, which has no cb0 at all. cb0 is per-permutation and holds LocalToWorld, so
// nothing in it is addressable from here.
inline bool SnapshotViewConstants(reshade::api::command_list* cmd_list) {
  auto* device = runtime::d3d11::GetDevice(cmd_list->get_device());
  auto* context = runtime::d3d11::GetContext(cmd_list);
  if (device == nullptr || context == nullptr) return false;

  ID3D11Buffer* buffer = nullptr;
  context->VSGetConstantBuffers(1, 1, &buffer);
  if (buffer == nullptr) return false;

  auto release = [&]() { buffer->Release(); };

  D3D11_BUFFER_DESC desc = {};
  buffer->GetDesc(&desc);
  if (desc.ByteWidth < 160u) {
    release();
    return false;
  }

  if (state.view_cb[0] == nullptr) {
    for (auto& slot : state.view_cb) runtime::d3d11::CreateGpuConstantBuffer(device, 160u, &slot);
    if (state.view_cb[1] == nullptr) {
      release();
      return false;
    }
  }

  D3D11_BOX box = {0u, 0u, 0u, 160u, 1u, 1u};
  context->CopySubresourceRegion(state.view_cb[state.view_snapshot_index].Get(), 0, 0, 0, 0, buffer,
                                 0, &box);

  if (state.view_snapshot_count < 2u) state.view_snapshot_count++;
  release();
  return true;
}

inline bool Fail(const std::string& reason) {
  if (!state.failed) {
    state.failed = true;
    Log(reshade::log::level::error, reason + "; falling back to SMAA");
  }
  return true;
}

inline bool RunDlaa(reshade::api::command_list* cmd_list) {
  auto* device = runtime::d3d11::GetDevice(cmd_list->get_device());
  auto* context = runtime::d3d11::GetContext(cmd_list);
  if (device == nullptr || context == nullptr) return Fail("no D3D11 device");
  // Transient on the first frame: the occlusion and g-buffer passes have not been seen yet.
  if (!state.snapshots_valid || state.view_snapshot_count < 2u || state.depth_srv == nullptr) {
    return true;
  }

  InitializeNgx(device);
  if (!state.ngx_ready) return Fail("NGX unavailable");

  ID3D11ShaderResourceView* bound_srvs[4] = {};
  context->CSGetShaderResources(0, 4, bound_srvs);
  ID3D11UnorderedAccessView* bound_uav = nullptr;
  context->CSGetUnorderedAccessViews(0, 1, &bound_uav);

  auto release_bindings = [&]() {
    for (auto* srv : bound_srvs) {
      if (srv != nullptr) srv->Release();
    }
    if (bound_uav != nullptr) bound_uav->Release();
  };

  if (bound_uav == nullptr) {
    release_bindings();
    return Fail("no output UAV bound to the SMAA blend pass");
  }

  runtime::d3d11::ComPtr<ID3D11Resource> output_resource;
  bound_uav->GetResource(&output_resource);
  runtime::d3d11::ComPtr<ID3D11Texture2D> output_texture;
  if (FAILED(output_resource.As(&output_texture))) {
    release_bindings();
    return Fail("SMAA output is not a 2D texture");
  }

  D3D11_TEXTURE2D_DESC output_desc = {};
  output_texture->GetDesc(&output_desc);

  if (!EnsureResources(device, output_desc.Width, output_desc.Height)) {
    release_bindings();
    return Fail("resource creation failed");
  }

  // t0 is the scene colour; the later slots still hold deferred lighting's bindings, one of
  // which is the blend pass's own output, so never fall back onto them blindly.
  ID3D11ShaderResourceView* colour_srv = nullptr;
  runtime::d3d11::ComPtr<ID3D11Resource> colour_resource;
  if (IsSceneColour(bound_srvs[0], state.width, state.height)) {
    colour_srv = bound_srvs[0];
    colour_srv->GetResource(&colour_resource);
  }
  if (colour_srv == nullptr) {
    release_bindings();
    return Fail("could not identify the scene colour SRV");
  }

  auto* velocity_srv = EnsureSrv(device, state.velocity_resource, &state.velocity_srv_resource,
                                 &state.velocity_srv);
  bool evaluate_failed = false;

  {
    runtime::d3d11::StateBackup backup(context);

    Constants constants = {};
    constants.debug_view = static_cast<uint32_t>(debug_view);
    runtime::d3d11::UpdateConstantBuffer(context, state.constants.Get(), constants);

    ID3D11ShaderResourceView* srvs[2] = {state.depth_srv.Get(), velocity_srv};
    uint32_t previous = state.view_snapshot_index ^ 1u;
    ID3D11Buffer* constant_buffers[5] = {state.cb0_snapshot.Get(), state.cb1_snapshot.Get(),
                                         state.constants.Get(),
                                         state.view_cb[state.view_snapshot_index].Get(),
                                         state.view_cb[previous].Get()};
    ID3D11UnorderedAccessView* uavs[3] = {
        state.motion_vectors.uav.Get(), state.depth_copy.uav.Get(),
        constants.debug_view != DLAA_DEBUG_OFF ? bound_uav : nullptr};
    static const UINT NO_COUNTS[3] = {};

    context->CSSetShader(state.motion_vector_shader.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 2, srvs);
    context->CSSetConstantBuffers(0, 5, constant_buffers);
    context->CSSetUnorderedAccessViews(0, 3, uavs, NO_COUNTS);
    context->Dispatch((state.width + 7u) / 8u, (state.height + 7u) / 8u, 1u);

    ID3D11UnorderedAccessView* null_uavs[3] = {};
    ID3D11ShaderResourceView* null_srvs[2] = {};
    context->CSSetUnorderedAccessViews(0, 3, null_uavs, NO_COUNTS);
    context->CSSetShaderResources(0, 2, null_srvs);

    if (constants.debug_view != DLAA_DEBUG_OFF) {
      // The debug pass wrote straight into the output, so there is nothing left to do.
    } else if (!EnsureDlssFeature(context)) {
      evaluate_failed = true;
    } else {
      NVSDK_NGX_D3D11_DLSS_Eval_Params eval = {};
      eval.Feature.pInColor = colour_resource.Get();
      eval.Feature.pInOutput = output_resource.Get();
      eval.pInDepth = state.depth_copy.texture.Get();
      eval.pInMotionVectors = state.motion_vectors.texture.Get();
      Jitter jitter = GetData(cmd_list)->eval_jitter;
      eval.InJitterOffsetX = jitter.x;
      eval.InJitterOffsetY = jitter.y;
      eval.InRenderSubrectDimensions = {state.width, state.height};
      eval.InMVScaleX = 1.f;
      eval.InMVScaleY = 1.f;
      eval.InReset = state.needs_reset ? 1 : 0;

      if (NVSDK_NGX_FAILED(NGX_D3D11_EVALUATE_DLSS_EXT(context, state.dlss_handle,
                                                       state.ngx_parameters, &eval))) {
        evaluate_failed = true;
      } else {
        state.needs_reset = false;
      }
    }
  }

  release_bindings();
  if (evaluate_failed) return Fail("DLSS evaluate failed");
  return false;
}

inline thread_local bool rebinding_viewport = false;

inline bool JitterActive(const CommandListData* data) {
  return enabled != 0.f && !state.failed && jitter_enabled != 0.f && state.width != 0
         && data->geometry_ended_frame != state.frame_index.load(std::memory_order_relaxed)
         && data->has_base_viewport
         && static_cast<uint32_t>(data->base_viewport.width) == state.width
         && static_cast<uint32_t>(data->base_viewport.height) == state.height;
}

// D3D11 viewport state persists, so the game need not re-set it every frame. Drive the jitter
// off this list's stored base instead of whatever is currently bound, or it would accumulate.
inline void SetViewport(reshade::api::command_list* cmd_list, const CommandListData* data,
                        bool jittered) {
  if (!data->has_base_viewport) return;
  reshade::api::viewport viewport = data->base_viewport;
  if (jittered) {
    Jitter jitter = state.jitter.load(std::memory_order_relaxed);
    viewport.x += jitter.x;
    viewport.y += jitter.y;
    state.jitter_applied.store(true, std::memory_order_relaxed);
  }
  rebinding_viewport = true;
  cmd_list->bind_viewports(0, 1, &viewport);
  rebinding_viewport = false;
}

// Translucency and decals draw after deferred lighting and still land in the image DLSS
// reconstructs, so the jitter must hold until the AA resolve. Only the UI comes after.
// This is also where the jitter phase advances: it is the one point per frame ordered against the
// geometry that consumed it, so the eval can never report an offset the g-buffer never used.
inline void EndGeometryPhase(reshade::api::command_list* cmd_list) {
  auto* data = GetData(cmd_list);
  bool was_jittered = JitterActive(data);
  data->geometry_ended_frame = state.frame_index.load(std::memory_order_relaxed);
  if (was_jittered) SetViewport(cmd_list, data, false);

  bool applied = state.jitter_applied.exchange(false, std::memory_order_relaxed);
  data->eval_jitter = applied ? state.jitter.load(std::memory_order_relaxed) : Jitter{};

  uint32_t phase = state.jitter_phase.fetch_add(1u, std::memory_order_relaxed) + 1u;
  state.jitter.store(jitter_enabled != 0.f ? NextJitter(phase) : Jitter{},
                     std::memory_order_relaxed);
}

inline bool OnDispatch(reshade::api::command_list* cmd_list, uint32_t, uint32_t, uint32_t) {
  auto* shader_state = renodx::utils::shader::GetCurrentState(cmd_list);
  if (shader_state == nullptr) return false;
  auto* compute_state = renodx::utils::shader::GetCurrentComputeState(shader_state);
  auto hash = renodx::utils::shader::GetCurrentComputeShaderHash(compute_state);

  if (hash == OCCLUSION) SnapshotConstantBuffers(cmd_list);
  return false;
}

inline void OnBindRenderTargets(reshade::api::command_list* cmd_list, uint32_t count,
                                const reshade::api::resource_view* rtvs,
                                reshade::api::resource_view) {
  auto* data = GetData(cmd_list);
  data->velocity_pass_active = count >= 6;
  // First pass of the frame only, so RT4 and the view constants always come from the same camera.
  if (data->velocity_pass_active && rtvs != nullptr && rtvs[4] != 0
      && !state.velocity_captured.exchange(true)) {
    auto* device = cmd_list->get_device();
    state.velocity_resource = runtime::d3d11::GetResource(device->get_resource_from_view(rtvs[4]));
  }
  if (count >= 4 && JitterActive(data)) SetViewport(cmd_list, data, true);
}

inline bool OnDrawIndexed(reshade::api::command_list* cmd_list, uint32_t, uint32_t, uint32_t,
                          int32_t, uint32_t) {
  auto* data = GetData(cmd_list);
  // Not an exchange: a snapshot can decline, and the next draw should then still get a chance.
  if (data->velocity_pass_active && !state.view_snapshot_taken.load()
      && SnapshotViewConstants(cmd_list)) {
    state.view_snapshot_taken.store(true);
  }
  return false;
}

inline void OnBindViewports(reshade::api::command_list* cmd_list, uint32_t first, uint32_t count,
                            const reshade::api::viewport* viewports) {
  if (rebinding_viewport) return;
  if (first != 0 || count != 1 || viewports == nullptr) return;
  auto* data = GetData(cmd_list);
  data->base_viewport = viewports[0];
  data->has_base_viewport = true;
  if (JitterActive(data)) SetViewport(cmd_list, data, true);
}

inline void OnInitCommandList(reshade::api::command_list* cmd_list) {
  renodx::utils::data::Create<CommandListData>(cmd_list);
}

inline void OnDestroyCommandList(reshade::api::command_list* cmd_list) {
  renodx::utils::data::Delete<CommandListData>(cmd_list);
}

inline void OnPresent(reshade::api::command_queue*, reshade::api::swapchain*,
                      const reshade::api::rect*, const reshade::api::rect*, uint32_t,
                      const reshade::api::rect*) {
  state.frame_index.fetch_add(1u);
  // Only advance on frames that saw the g-buffer, or both slots would hold the same camera.
  if (state.view_snapshot_taken.exchange(false)) state.view_snapshot_index ^= 1u;
  state.velocity_captured.store(false);
  if (enabled == 0.f) state.failed = false;
}

}  // namespace internal

inline bool IsEnabled() {
  return enabled != 0.f;
}

// Enabled and not fallen back to SMAA after a failure.
inline bool IsActive() {
  return enabled != 0.f && !internal::state.failed;
}

inline renodx::utils::settings::Settings Settings() {
  return {
      new renodx::utils::settings::Setting{
          .key = "DlaaEnabled",
          .binding = &enabled,
          .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
          .default_value = 0.f,
          .label = "DLAA",
          .section = "Anti-Aliasing",
          .tooltip = "Replaces SMAA 1x with DLSS in DLAA mode. Requires an NVIDIA RTX GPU.",
      },
      new renodx::utils::settings::Setting{
          .key = "DlaaJitter",
          .binding = &jitter_enabled,
          .value_type = renodx::utils::settings::SettingValueType::BOOLEAN,
          .default_value = 0.f,
          .label = "Temporal Jitter",
          .section = "Anti-Aliasing",
          .tooltip = "Adds sub-pixel jitter to the g-buffer viewport, letting DLAA resolve detail finer than a pixel.",
          .is_enabled = IsEnabled,
      },
      new renodx::utils::settings::Setting{
          .key = "DlaaDebugView",
          .binding = &debug_view,
          .value_type = renodx::utils::settings::SettingValueType::INTEGER,
          .default_value = 0.f,
          .label = "Debug View",
          .section = "DLAA Debug",
          .tooltip = "Shows the synthesised flow field. Grey is still, blue is behind the previous camera.",
          .labels = {"Off", "Motion Vectors"},
          .is_enabled = IsEnabled,
      },
  };
}

inline renodx::mods::shader::CustomShaders Shaders() {
  // These carry no replacement code, so on_replace always declines and the stock pass runs
  // whenever on_draw lets it through.
  auto never_replace = [](reshade::api::command_list*) { return false; };
  auto bypass_when_enabled = [](reshade::api::command_list*) { return !IsActive(); };
  return renodx::mods::shader::DefineCustomShaders({
      {internal::SMAA_EDGES,
       {.crc32 = internal::SMAA_EDGES, .on_replace = never_replace, .on_draw = bypass_when_enabled}},
      {internal::SMAA_WEIGHTS,
       {.crc32 = internal::SMAA_WEIGHTS, .on_replace = never_replace, .on_draw = bypass_when_enabled}},
      {internal::SMAA_BLEND,
       {.crc32 = internal::SMAA_BLEND,
        .on_replace = never_replace,
        .on_draw = [](reshade::api::command_list* cmd_list) {
          internal::EndGeometryPhase(cmd_list);
          if (!IsActive()) return true;
          return internal::RunDlaa(cmd_list);
        }}},
  });
}

inline void Register() {
  reshade::register_event<reshade::addon_event::init_command_list>(internal::OnInitCommandList);
  reshade::register_event<reshade::addon_event::destroy_command_list>(
      internal::OnDestroyCommandList);
  reshade::register_event<reshade::addon_event::dispatch>(internal::OnDispatch);
  reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
      internal::OnBindRenderTargets);
  reshade::register_event<reshade::addon_event::draw_indexed>(internal::OnDrawIndexed);
  reshade::register_event<reshade::addon_event::bind_viewports>(internal::OnBindViewports);
  reshade::register_event<reshade::addon_event::present>(internal::OnPresent);
}

inline void Unregister() {
  reshade::unregister_event<reshade::addon_event::init_command_list>(internal::OnInitCommandList);
  reshade::unregister_event<reshade::addon_event::destroy_command_list>(
      internal::OnDestroyCommandList);
  reshade::unregister_event<reshade::addon_event::dispatch>(internal::OnDispatch);
  reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(
      internal::OnBindRenderTargets);
  reshade::unregister_event<reshade::addon_event::draw_indexed>(internal::OnDrawIndexed);
  reshade::unregister_event<reshade::addon_event::bind_viewports>(internal::OnBindViewports);
  reshade::unregister_event<reshade::addon_event::present>(internal::OnPresent);

  internal::ReleaseDlssFeature();
  if (internal::state.ngx_ready) {
    NVSDK_NGX_D3D11_DestroyParameters(internal::state.ngx_parameters);
    NVSDK_NGX_D3D11_Shutdown1(nullptr);
    internal::state.ngx_ready = false;
  }
}

inline void SetModule(HMODULE module) {
  internal::state.addon_module = module;
}

}  // namespace features::dlaa

#endif  // SRC_GAMES_BATMANAK_REND2_FEATURES_DLAA_H_
