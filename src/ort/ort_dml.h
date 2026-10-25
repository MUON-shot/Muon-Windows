// The part of onnxruntime's dml_provider_factory.h that Muon needs (selecting the GPU by preference),
// without pulling in DirectML.h. Copyright (c) Microsoft Corporation, MIT License.
#pragma once
#include "onnxruntime_c_api.h"

struct IDMLDevice;
struct ID3D12CommandQueue;
struct ID3D12Resource;

extern "C" {
enum OrtDmlDeviceFilter : uint32_t { Gpu = 1 << 0 };
typedef enum OrtDmlPerformancePreference { Default = 0, HighPerformance = 1, MinimumPower = 2 } OrtDmlPerformancePreference;
struct OrtDmlDeviceOptions {
    OrtDmlPerformancePreference Preference;
    OrtDmlDeviceFilter Filter;
};
struct OrtDmlApi {
    ORT_API2_STATUS(SessionOptionsAppendExecutionProvider_DML, _In_ OrtSessionOptions* options, int device_id);
    ORT_API2_STATUS(SessionOptionsAppendExecutionProvider_DML1, _In_ OrtSessionOptions* options, _In_ IDMLDevice* dml_device, _In_ ID3D12CommandQueue* cmd_queue);
    ORT_API2_STATUS(CreateGPUAllocationFromD3DResource, _In_ ID3D12Resource* d3d_resource, _Out_ void** dml_resource);
    ORT_API2_STATUS(FreeGPUAllocation, _In_ void* dml_resource);
    ORT_API2_STATUS(GetD3D12ResourceFromAllocation, _In_ OrtAllocator* provider, _In_ void* dml_resource, _Out_ ID3D12Resource** d3d_resource);
    ORT_API2_STATUS(SessionOptionsAppendExecutionProvider_DML2, _In_ OrtSessionOptions* options, OrtDmlDeviceOptions* device_opts);
};
}
