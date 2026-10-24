cmake_minimum_required(VERSION 3.20)
project(Muon CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the exe runs on a clean Windows without the VC++ redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------- LibRaw (static) ----------------
file(GLOB_RECURSE LIBRAW_SRC ${CMAKE_SOURCE_DIR}/third_party/LibRaw/src/*.cpp)
list(FILTER LIBRAW_SRC EXCLUDE REGEX ".*_ph\\.cpp$")
add_library(libraw STATIC ${LIBRAW_SRC})
target_include_directories(libraw PUBLIC ${CMAKE_SOURCE_DIR}/third_party/LibRaw)
target_compile_definitions(libraw PUBLIC LIBRAW_NODLL PRIVATE LIBRAW_BUILDLIB WIN32 _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_compile_options(libraw PRIVATE /W0 /O2 /MP /EHsc)

# ---------------- Dear ImGui ----------------
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/third_party/imgui)
add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_compile_options(imgui PRIVATE /O2 /MP)

# ---------------- Muon ----------------
add_executable(Muon WIN32
    src/main.cpp src/gpu.cpp src/pipeline.cpp src/params.cpp src/imageio.cpp
    src/ui.cpp src/library.cpp src/preset.cpp src/widgets.cpp src/effects.cpp src/selftest.cpp src/muon.manifest res/muon.rc)
target_link_libraries(Muon PRIVATE libraw imgui d3d11 dxgi d3dcompiler windowscodecs ole32 shell32 shlwapi dwmapi gdi32 uuid ws2_32 dbghelp)
target_compile_definitions(Muon PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
target_compile_options(Muon PRIVATE /utf-8 /O2 /MP /W3 /EHsc /wd4244 /wd4305 /wd4267)
# Debug symbols in a separate .pdb (not shipped) so crash dumps can be read; code is unchanged.
target_compile_options(Muon PRIVATE /Zi)
target_link_options(Muon PRIVATE /DEBUG /OPT:REF /OPT:ICF /MAP)
set_target_properties(Muon PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin)
