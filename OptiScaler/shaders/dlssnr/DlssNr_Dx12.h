#pragma once

#include "DlssNr_Common.h"

#include <d3d12.h>
#include <d3dx/d3dx12.h>
#include <memory>
#include <shaders/Shader_Dx12.h>
#include <shaders/Shader_Dx12Utils.h>

// Meter, encode, optional downsample and resolve each need their own descriptors and constants.
#define DLSSNR_NUM_OF_HEAPS 32

// Each upscaler owns its NR model, composition resources, history and output buffer.
class DlssNr_Dx12 : public Shader_Dx12, public DlssNr_Common
{
  private:
    struct State;
    std::unique_ptr<State> _state;
    FrameDescriptorHeap _frameHeaps[DLSSNR_NUM_OF_HEAPS];
    ID3D12Resource* _constantBuffers[DLSSNR_NUM_OF_HEAPS] = {};
    uint32_t _heapIndex = 0;
    static constexpr uint32_t kSrvCount = 5;
    static constexpr uint32_t kUavCount = 2;
    uint32_t _numThreadsX = 8;
    uint32_t _numThreadsY = 8;

    bool DispatchPass(ID3D12GraphicsCommandList* cmdList, const DlssNrConstants& constants, ID3D12Resource* source,
                      ID3D12Resource* model, ID3D12Resource* original, ID3D12Resource* motion,
                      ID3D12Resource* previousEdit, ID3D12Resource* target, ID3D12Resource* keep);

  public:
    DlssNr_Dx12(std::string name, ID3D12Device* device);
    ~DlssNr_Dx12();

    bool CreateBufferResource(ID3D12Device* device, ID3D12Resource* source, D3D12_RESOURCE_STATES state);
    void SetBufferState(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES state);
    ID3D12Resource* Buffer();
    bool CanRender() const;

    // Inputs are compute-readable and output is a UAV. Aliasing colour/output is supported with
    // that resource in UAV state. All resources retain these states on return. False leaves the
    // caller responsible for forwarding the original frame. The pipeline owns command-state restore.
    bool Dispatch(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* colour, ID3D12Resource* depth,
                  ID3D12Resource* motion, ID3D12Resource* output, const DlssNrFrameInfo& frame,
                  ID3D12CommandQueue* timingQueue = nullptr);
};
