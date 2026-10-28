#pragma once
#include "gfx.h"

#include <oleidl.h>

struct Image;

std::unique_ptr<Gdiplus::Bitmap> ShellThumbnail(const fs::path& path, int sizePx);
std::vector<fs::path> FilesFromDataObject(IDataObject* data);
// Starts a shell drag of `files` (blocks until dropped/cancelled). Returns the performed effect.
DWORD DragOutFiles(HWND hwnd, const std::vector<fs::path>& files);
bool CopyFilesToClipboard(HWND hwnd, const std::vector<fs::path>& files);
bool CopyTextToClipboard(HWND hwnd, const std::wstring& text);
// Puts an opaque image on the clipboard as a bitmap (CF_DIB) and as PNG, which some apps prefer.
bool CopyImageToClipboard(HWND hwnd, const Image& img);
std::vector<fs::path> ExplorerSelection();
// True while an app runs an OLE drag (files, text...), started at `downPt`. A mouse moved with the
// button held (games, drawing, selecting) isn't one.
bool OleDragInProgress(POINT downPt);
// True when the foreground window is a game, video or presentation covering its whole monitor.
bool FullscreenAppActive();
bool PickFolder(HWND owner, fs::path& out);
// Asks for a program (.exe or shortcut), starting in the Start menu's program folder.
bool PickProgram(HWND owner, fs::path& out);
bool MoveFilesTo(HWND owner, const std::vector<fs::path>& files, const fs::path& dest);
void RevealInExplorer(const std::vector<fs::path>& files);
void OpenTerminalAt(const fs::path& dir);
void OpenWithDialog(HWND owner, const fs::path& file);
bool ShareFiles(HWND hwnd, const std::vector<fs::path>& files);
// Opens the Bluetooth file transfer wizard with `files` (regular files only) already chosen.
bool SendViaBluetooth(const std::vector<fs::path>& files);
