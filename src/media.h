#pragma once
#include "common.h"

bool IsAudioExt(const std::wstring& ext);
bool IsVideoExt(const std::wstring& ext);
bool IsArchiveExt(const std::wstring& ext);

fs::path FfmpegPath();  // empty if not installed
// Runs: ffmpeg -y -i <in> <args> <out>
void Ffmpeg(const fs::path& in, const std::wstring& args, const fs::path& out);
// Codec arguments for converting into `ext`.
std::wstring FfmpegArgsFor(const std::wstring& targetExt, bool sourceIsVideo);

// Archives through Windows' bundled bsdtar (tar.exe).
void ExtractArchive(const fs::path& archive, const fs::path& destDir);
void CreateArchive(const fs::path& archive, const fs::path& baseDir, const std::vector<std::wstring>& names);
