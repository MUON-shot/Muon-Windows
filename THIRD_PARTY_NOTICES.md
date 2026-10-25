# Third-party software

Muon.exe is statically linked with the libraries below. Their license texts are in the `licenses` folder.

| Library | Version | License | Source |
|---|---|---|---|
| [LibRaw](https://www.libraw.org) | 0.22.2 | CDDL 1.0 (chosen from the LibRaw dual license LGPL 2.1 / CDDL 1.0) | https://github.com/LibRaw/LibRaw/tree/0.22.2 |
| [Dear ImGui](https://github.com/ocornut/imgui) | 1.91.9 | MIT | https://github.com/ocornut/imgui/tree/v1.91.9 |

LibRaw contains code derived from dcraw by Dave Coffin and the DCB / FBDD algorithms by Jacek Gozdz (BSD-style license), see `licenses/LibRaw-COPYRIGHT.txt`.
LibRaw is used unmodified; its complete source code for the version above is available at the link in the table.

## Depth module (optional, the `depth` folder)

The depth map for the "Optics & light" tab is computed by a neural network that Muon loads at run time from the `depth` folder.
These files are redistributed unmodified. Without the folder Muon works as before and treats the scene as flat.

| Component | Version | License | Source |
|---|---|---|---|
| [ONNX Runtime](https://onnxruntime.ai) with DirectML (`onnxruntime.dll`, `onnxruntime_providers_shared.dll`) | 1.24.4 | MIT, third-party notices in `licenses/onnxruntime-ThirdPartyNotices.txt` | https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime.DirectML/1.24.4 |
| [DirectML](https://github.com/microsoft/DirectML) (`DirectML.dll`) | 1.15.4 | Microsoft Software License Terms, redistributable with applications built with ML tools (`licenses/DirectML-LICENSE.txt`) | https://www.nuget.org/packages/Microsoft.AI.DirectML/1.15.4 |
| [Depth Anything V2 Small](https://github.com/DepthAnything/Depth-Anything-V2), ONNX fp16 export by onnx-community (`depth_anything_v2_small_fp16.onnx`) | | Apache 2.0 (`licenses/DepthAnythingV2-LICENSE-Apache-2.0.txt`) | https://huggingface.co/onnx-community/depth-anything-v2-small |

Depth Anything V2: L. Yang, B. Kang, Z. Huang, Z. Zhao, X. Xu, J. Feng, H. Zhao, "Depth Anything V2", 2024.
The C API headers of ONNX Runtime in `src/ort` are MIT licensed (Microsoft Corporation).

Muon uses the Segoe UI fonts that come with Windows at run time; no fonts are bundled.
Everything else (Direct3D 11, Windows Imaging Component, the file dialogs) is part of Windows.
