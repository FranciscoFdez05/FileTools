#pragma once
#include "gfx.h"

#include <oleidl.h>

std::unique_ptr<Gdiplus::Bitmap> ShellThumbnail(const fs::path& path, int sizePx);
std::vector<fs::path> FilesFromDataObject(IDataObject* data);
// Starts a shell drag of `files` (blocks until dropped/cancelled). Returns the performed effect.
DWORD DragOutFiles(HWND hwnd, const std::vector<fs::path>& files);
bool CopyFilesToClipboard(HWND hwnd, const std::vector<fs::path>& files);
bool CopyTextToClipboard(HWND hwnd, const std::wstring& text);
std::vector<fs::path> ExplorerSelection();
bool PickFolder(HWND owner, fs::path& out);
bool MoveFilesTo(HWND owner, const std::vector<fs::path>& files, const fs::path& dest);
void RevealInExplorer(const std::vector<fs::path>& files);
void OpenTerminalAt(const fs::path& dir);
void OpenWithDialog(HWND owner, const fs::path& file);
bool ShareFiles(HWND hwnd, const std::vector<fs::path>& files);
