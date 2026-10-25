#pragma once
// Monocular depth estimation (Depth Anything V2 Small) through ONNX Runtime, loaded at run time
// from a "depth" folder next to Muon.exe or in %LOCALAPPDATA%\Muon\depth. Without it Muon works as before.
#include <string>
#include <vector>
#include <cstdint>

std::wstring DepthModuleDir();          // empty when the module is not installed
bool DepthModuleAvailable();

// Runs the network on an sRGB RGBA8 image. Output is nearness in 0..1 (1 = close to the camera).
bool DepthEstimate(const std::vector<uint8_t>& rgba, int w, int h, std::vector<float>& out, int& ow, int& oh,
                   std::string& error, std::string* device = nullptr);
void DepthShutdown();

// Small on-disk cache so a photo's depth is computed once.
bool DepthCacheLoad(const std::wstring& photo, std::vector<float>& d, int& w, int& h);
void DepthCacheSave(const std::wstring& photo, const std::vector<float>& d, int w, int h);
