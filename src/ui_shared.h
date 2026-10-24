#pragma once
// Helpers shared between ui.cpp (editor panels) and library.cpp (library tab, filmstrip, thumbnails).
#include "app.h"
#include "imgui.h"
#include <string>
#include <vector>

void Changed();
void ResetAll();
void ApplyDefaultsForNew(Params& p, bool raw);
inline ImTextureID TexId(Tex* t) { return (ImTextureID)(intptr_t)t->srv; }

// sidecars of photos that are not open
bool ReadSidecarFor(const std::wstring& path, Params& out);
void WriteSidecarFor(const std::wstring& path, const Params& p);
void DeleteSidecarFor(const std::wstring& path);

// settings clipboard
void CopySettingsFrom(const std::wstring& path, uint32_t groups);
void OpenCopyDialog(const std::wstring& path);
void PasteSettingsTo(const std::vector<std::wstring>& paths);
void ResetSettingsOf(const std::vector<std::wstring>& paths);

// export dialog for a set of photos (empty = the open photo)
void OpenExport(const std::vector<std::wstring>& paths);

// library.cpp
void ThumbsStart();
void ThumbsStop();
void ThumbsPoll();
ThumbEntry* ThumbFor(const std::wstring& path, bool request);
void ThumbReplace(const std::wstring& path, const std::vector<uint8_t>& rgba, int w, int h);

int CollectionIndex(const std::wstring& path);
void CollectionAdd(const std::vector<std::wstring>& paths);
void CollectionRemove(const std::vector<std::wstring>& paths);
std::vector<std::wstring> CollectionSelection();           // selected photos, or the open one
void CollectionSelectOnly(int index);
void CollectionLoad();
void CollectionSave();

void LibraryOpenFolder(const std::wstring& folder);
void LibraryGrid(ImVec2 pos, ImVec2 size);
void LibraryPanel();
void Filmstrip(ImVec2 pos, ImVec2 size);
void SelectAllInView();
