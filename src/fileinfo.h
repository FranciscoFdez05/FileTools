#pragma once
#include "actions.h"

// MD5, SHA-1, SHA-256 and SHA-512 of `file` (one pass), plus its size.
std::vector<InfoRow> FileHashes(const fs::path& file, const std::wstring& group);

fs::path ExifToolPath();  // the path chosen by the user, else a detected install; empty if none
void SetConfiguredExifTool(const fs::path& exe);  // remembered across sessions
std::wstring ExifToolVersion(const fs::path& exe);  // empty if `exe` is not a working ExifTool
// File system details, Windows properties and, when installed, every ExifTool tag.
// `groupPrefix` is prepended to each group name. Returns the number of ExifTool tags (-1 = no ExifTool).
int CollectMetadata(const fs::path& file, const std::wstring& groupPrefix, std::vector<InfoRow>& rows);

// Writes a copy of `in` without metadata through ExifTool. On failure returns false and sets `error`.
bool StripWithExifTool(const fs::path& in, const fs::path& out, bool keepOrientation, std::wstring& error);
// Office Open XML / OpenDocument: blanks the document properties (author, company, dates, ...).
bool IsOfficeExt(const std::wstring& ext);
void StripOfficeMetadata(const fs::path& in, const fs::path& out);
// NTFS alternate data streams, e.g. Zone.Identifier (where a download came from).
bool HasAlternateStreams(const fs::path& file);
