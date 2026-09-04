#ifndef SRC_GAMES_BATMANAK_REND2_RUNTIME_D3D11_H_
#define SRC_GAMES_BATMANAK_REND2_RUNTIME_D3D11_H_

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <span>

#include <include/reshade.hpp>

// Native D3D11 helpers for features that run their own passes instead of replacing shaders.
namespace runtime::d3d11 {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

inline ID3D11Device* GetDevice(reshade::api::device* device) {
  if (device == nullptr || device->get_api() != reshade::api::device_api::d3d11) return nullptr;
  return reinterpret_cast<ID3D11Device*>(device->get_native());
}

inline ID3D11DeviceContext* GetContext(reshade::api::command_list* cmd_list) {
  if (cmd_list == nullptr) return nullptr;
  return reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
}

inline ID3D11Resource* GetResource(reshade::api::resource resource) {
  return reinterpret_cast<ID3D11Resource*>(static_cast<uintptr_t>(resource.handle));
}

// Saves the state our dispatches and NGX are known to clobber, and puts it back afterwards.
class StateBackup {
 public:
  static constexpr uint32_t SRV_COUNT = 8;
  static constexpr uint32_t UAV_COUNT = 8;
  static constexpr uint32_t CB_COUNT = 8;
  static constexpr uint32_t SAMPLER_COUNT = 4;

  explicit StateBackup(ID3D11DeviceContext* context) : context_(context) {
    context_->CSGetShader(&shader_, nullptr, nullptr);
    context_->CSGetShaderResources(0, SRV_COUNT, srvs_);
    context_->CSGetUnorderedAccessViews(0, UAV_COUNT, uavs_);
    context_->CSGetConstantBuffers(0, CB_COUNT, constant_buffers_);
    context_->CSGetSamplers(0, SAMPLER_COUNT, samplers_);
    context_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, &dsv_);
    viewport_count_ = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context_->RSGetViewports(&viewport_count_, viewports_);
  }

  ~StateBackup() {
    static const UINT NO_COUNTS[UAV_COUNT] = {};
    context_->CSSetShader(shader_, nullptr, 0);
    context_->CSSetShaderResources(0, SRV_COUNT, srvs_);
    context_->CSSetUnorderedAccessViews(0, UAV_COUNT, uavs_, NO_COUNTS);
    context_->CSSetConstantBuffers(0, CB_COUNT, constant_buffers_);
    context_->CSSetSamplers(0, SAMPLER_COUNT, samplers_);
    context_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs_, dsv_);
    context_->RSSetViewports(viewport_count_, viewports_);

    Release(shader_);
    for (auto*& srv : srvs_) Release(srv);
    for (auto*& uav : uavs_) Release(uav);
    for (auto*& buffer : constant_buffers_) Release(buffer);
    for (auto*& sampler : samplers_) Release(sampler);
    for (auto*& rtv : rtvs_) Release(rtv);
    Release(dsv_);
  }

  StateBackup(const StateBackup&) = delete;
  StateBackup& operator=(const StateBackup&) = delete;

 private:
  template <typename T>
  static void Release(T*& object) {
    if (object != nullptr) {
      object->Release();
      object = nullptr;
    }
  }

  ID3D11DeviceContext* context_;
  ID3D11ComputeShader* shader_ = nullptr;
  ID3D11ShaderResourceView* srvs_[SRV_COUNT] = {};
  ID3D11UnorderedAccessView* uavs_[UAV_COUNT] = {};
  ID3D11Buffer* constant_buffers_[CB_COUNT] = {};
  ID3D11SamplerState* samplers_[SAMPLER_COUNT] = {};
  ID3D11RenderTargetView* rtvs_[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
  ID3D11DepthStencilView* dsv_ = nullptr;
  D3D11_VIEWPORT viewports_[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
  UINT viewport_count_ = 0;
};

// A full-res compute target we own, readable by NGX and writable by our passes.
struct Texture {
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> srv;
  ComPtr<ID3D11UnorderedAccessView> uav;

  void Reset() {
    texture.Reset();
    srv.Reset();
    uav.Reset();
  }
};

inline bool CreateTexture(ID3D11Device* device, uint32_t width, uint32_t height,
                          DXGI_FORMAT format, Texture* out) {
  D3D11_TEXTURE2D_DESC desc = {};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

  out->Reset();
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &out->texture))) return false;
  if (FAILED(device->CreateShaderResourceView(out->texture.Get(), nullptr, &out->srv))) return false;
  if (FAILED(device->CreateUnorderedAccessView(out->texture.Get(), nullptr, &out->uav))) return false;
  return true;
}

struct StructuredBuffer {
  ComPtr<ID3D11Buffer> buffer;
  ComPtr<ID3D11ShaderResourceView> srv;
  ComPtr<ID3D11UnorderedAccessView> uav;

  void Reset() {
    buffer.Reset();
    srv.Reset();
    uav.Reset();
  }
};

inline bool CreateStructuredBuffer(ID3D11Device* device, uint32_t stride, uint32_t count,
                                   StructuredBuffer* out) {
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = stride * count;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
  desc.StructureByteStride = stride;

  out->Reset();
  if (FAILED(device->CreateBuffer(&desc, nullptr, &out->buffer))) return false;

  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Format = DXGI_FORMAT_UNKNOWN;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
  srv_desc.Buffer.NumElements = count;
  if (FAILED(device->CreateShaderResourceView(out->buffer.Get(), &srv_desc, &out->srv))) return false;

  D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
  uav_desc.Format = DXGI_FORMAT_UNKNOWN;
  uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  uav_desc.Buffer.NumElements = count;
  if (FAILED(device->CreateUnorderedAccessView(out->buffer.Get(), &uav_desc, &out->uav))) return false;
  return true;
}

inline bool CreateConstantBuffer(ID3D11Device* device, uint32_t byte_width,
                                 ComPtr<ID3D11Buffer>* out) {
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = (byte_width + 15u) & ~15u;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  out->Reset();
  return SUCCEEDED(device->CreateBuffer(&desc, nullptr, out->GetAddressOf()));
}

// GPU-only constant buffer, so CopySubresourceRegion can slice registers out of a game buffer.
inline bool CreateGpuConstantBuffer(ID3D11Device* device, uint32_t byte_width,
                                    ComPtr<ID3D11Buffer>* out) {
  D3D11_BUFFER_DESC desc = {};
  desc.ByteWidth = (byte_width + 15u) & ~15u;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  out->Reset();
  return SUCCEEDED(device->CreateBuffer(&desc, nullptr, out->GetAddressOf()));
}

// Default-usage clone of a game constant buffer, so CopyResource can snapshot it mid-frame.
inline bool CreateConstantBufferSnapshot(ID3D11Device* device, ID3D11Buffer* source,
                                         ComPtr<ID3D11Buffer>* out) {
  D3D11_BUFFER_DESC desc = {};
  source->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.CPUAccessFlags = 0;
  desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  desc.MiscFlags = 0;
  out->Reset();
  return SUCCEEDED(device->CreateBuffer(&desc, nullptr, out->GetAddressOf()));
}

template <typename T>
inline void UpdateConstantBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer,
                                 const T& value) {
  D3D11_MAPPED_SUBRESOURCE mapped = {};
  if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
  memcpy(mapped.pData, &value, sizeof(T));
  context->Unmap(buffer, 0);
}

inline bool CreateComputeShader(ID3D11Device* device, std::span<const uint8_t> blob,
                                ComPtr<ID3D11ComputeShader>* out) {
  out->Reset();
  return SUCCEEDED(
      device->CreateComputeShader(blob.data(), blob.size(), nullptr, out->GetAddressOf()));
}

inline bool SupportsTypedUnorderedAccessStore(ID3D11Device* device, DXGI_FORMAT format) {
  D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support = {};
  support.InFormat = format;
  if (FAILED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support,
                                         sizeof(support)))) {
    return false;
  }
  return (support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
}

}  // namespace runtime::d3d11

#endif  // SRC_GAMES_BATMANAK_REND2_RUNTIME_D3D11_H_
