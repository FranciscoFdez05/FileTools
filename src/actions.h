#pragma once
#include "common.h"

#include <memory>

enum class Mode { Convert = 0, Tools = 1, Actions = 2 };
enum class Kind { Image, Pdf, Audio, Video, Document, Archive, Folder, Other };

struct ActionDef {
    std::string id;      // "to:png", "tool:crop", "act:copy", ...
    std::wstring label;  // shown on the wheel
    wchar_t glyph = 0;   // Segoe Fluent Icons code point, 0 = text only
    bool ui = false;     // runs on the UI thread (dialogs, clipboard, shell)
};

// Name/value table shown in the info popup (hashes, metadata).
struct InfoRow {
    std::wstring group, name, value;
};

struct InfoSheet {
    enum Type { Hash, Metadata } type = Metadata;
    std::wstring title;    // window title
    std::wstring summary;  // line under the title
    std::wstring note;     // default status line
    std::vector<fs::path> files;
    std::vector<InfoRow> rows;
    bool canStrip = false;  // offer "Quitar metadatos"
};

struct JobResult {
    bool ok = true;
    std::vector<fs::path> outputs;
    std::wstring message;  // short status line ("1,1 MB → 340 KB", error text, ...)
    bool hide = false;     // close the wheel right away
    std::shared_ptr<const InfoSheet> sheet;  // open the info popup instead of showing a result
};

using Progress = std::function<void(int done, int total)>;

Kind KindOf(const fs::path& p);
const wchar_t* ModeName(Mode m);
std::vector<ActionDef> ActionsFor(Mode mode, const std::vector<fs::path>& files);

// Worker-thread actions (conversions, tools). Never throws.
JobResult RunAction(const std::string& id, const std::vector<fs::path>& files, const Progress& progress);
// UI-thread actions (ActionDef::ui). Never throws.
JobResult RunUiAction(const std::string& id, const std::vector<fs::path>& files, HWND owner, POINT anchor);
