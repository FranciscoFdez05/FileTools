#include "fileinfo.h"

#include "zip.h"

#include <bcrypt.h>
#include <propkey.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <cwctype>
#include <set>

using Microsoft::WRL::ComPtr;

namespace {

std::wstring Trim(std::wstring s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return {};
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::wstring Thousands(uint64_t v) {
    std::wstring s = std::to_wstring(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, L".");
    return s;
}

std::wstring SizeText(uint64_t bytes) {
    return bytes < 1024 ? Thousands(bytes) + L" bytes" : FormatSize(bytes) + L" (" + Thousands(bytes) + L" bytes)";
}

std::wstring FileTimeText(const FILETIME& ft) {
    SYSTEMTIME utc, st;
    if (!FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &st)) return {};
    wchar_t buf[64];
    swprintf_s(buf, L"%02u/%02u/%04u %02u:%02u:%02u", st.wDay, st.wMonth, st.wYear, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring AttributesText(DWORD a) {
    static const std::pair<DWORD, const wchar_t*> names[] = {
        {FILE_ATTRIBUTE_READONLY, L"Solo lectura"}, {FILE_ATTRIBUTE_HIDDEN, L"Oculto"},
        {FILE_ATTRIBUTE_SYSTEM, L"Sistema"},        {FILE_ATTRIBUTE_ARCHIVE, L"Archivo"},
        {FILE_ATTRIBUTE_COMPRESSED, L"Comprimido"}, {FILE_ATTRIBUTE_ENCRYPTED, L"Cifrado"},
        {FILE_ATTRIBUTE_SPARSE_FILE, L"Disperso"},  {FILE_ATTRIBUTE_REPARSE_POINT, L"Vínculo"},
        {FILE_ATTRIBUTE_OFFLINE, L"Sin conexión"},  {FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS, L"En la nube"},
    };
    std::wstring s;
    for (auto& [bit, name] : names)
        if (a & bit) s += (s.empty() ? L"" : L", ") + std::wstring(name);
    return s.empty() ? L"Ninguno" : s;
}

HANDLE OpenForRead(const fs::path& p) {
    return CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

// --- File system ----------------------------------------------------------------

struct Stream {
    std::wstring name;  // without the leading ':' and the ":$DATA" suffix
    uint64_t size;
};

std::vector<Stream> AlternateStreams(const fs::path& f) {
    std::vector<Stream> out;
    WIN32_FIND_STREAM_DATA d;
    HANDLE h = FindFirstStreamW(f.c_str(), FindStreamInfoStandard, &d, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = d.cStreamName;
        if (name == L"::$DATA") continue;
        if (name.ends_with(L":$DATA")) name.resize(name.size() - 6);
        if (!name.empty() && name[0] == L':') name.erase(0, 1);
        out.push_back({name, (uint64_t)d.StreamSize.QuadPart});
    } while (FindNextStreamW(h, &d));
    FindClose(h);
    return out;
}

std::string ReadStream(const fs::path& f, const std::wstring& stream) {
    std::string data;
    HANDLE h = OpenForRead(f.wstring() + L":" + stream);
    if (h == INVALID_HANDLE_VALUE) return data;
    char buf[4096];
    DWORD n = 0;
    while (data.size() < 65536 && ReadFile(h, buf, sizeof(buf), &n, nullptr) && n) data.append(buf, n);
    CloseHandle(h);
    return data;
}

void AddFileSystem(const fs::path& f, const std::wstring& group, std::vector<InfoRow>& rows) {
    auto add = [&](const wchar_t* name, std::wstring value) {
        if (!value.empty()) rows.push_back({group, name, std::move(value)});
    };
    add(L"Nombre", f.filename().wstring());
    add(L"Carpeta", f.parent_path().wstring());
    SHFILEINFOW sfi{};
    if (SHGetFileInfoW(f.c_str(), 0, &sfi, sizeof(sfi), SHGFI_TYPENAME)) add(L"Tipo", sfi.szTypeName);

    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a)) return;
    bool dir = a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
    if (dir) {
        uint64_t count = 0, bytes = 0;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(f, fs::directory_options::skip_permission_denied, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) {
                ++count;
                bytes += it->file_size(ec);
            }
        }
        add(L"Tamaño", SizeText(bytes));
        add(L"Contenido", Thousands(count) + L" archivos");
    } else {
        add(L"Tamaño", SizeText((uint64_t(a.nFileSizeHigh) << 32) | a.nFileSizeLow));
    }
    add(L"Creado", FileTimeText(a.ftCreationTime));
    add(L"Modificado", FileTimeText(a.ftLastWriteTime));
    add(L"Último acceso", FileTimeText(a.ftLastAccessTime));
    add(L"Atributos", AttributesText(a.dwFileAttributes));

    for (const auto& s : AlternateStreams(f)) {
        if (s.name != L"Zone.Identifier") {
            add((L"Flujo alternativo"), s.name + L" · " + SizeText(s.size));
            continue;
        }
        // "Mark of the Web": Windows remembers where a download came from.
        std::wstring text = Utf8ToWide(ReadStream(f, s.name));
        size_t pos = 0;
        while (pos < text.size()) {
            size_t end = text.find(L'\n', pos);
            std::wstring line = Trim(text.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos));
            pos = end == std::wstring::npos ? text.size() : end + 1;
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = line.substr(0, eq), value = line.substr(eq + 1);
            if (key == L"ZoneId") {
                static const wchar_t* zones[] = {L"Equipo local", L"Intranet", L"Sitios de confianza", L"Internet",
                                                 L"Sitios restringidos"};
                int z = _wtoi(value.c_str());
                add(L"Descargado (zona)", z >= 0 && z < 5 ? std::wstring(zones[z]) : value);
            } else if (key == L"HostUrl") {
                add(L"Descargado de", value);
            } else if (key == L"ReferrerUrl") {
                add(L"Página de origen", value);
            }
        }
    }
}

// --- Windows property system -----------------------------------------------------

void AddShellProperties(const fs::path& f, const std::wstring& group, std::vector<InfoRow>& rows) {
    // Already covered by AddFileSystem, or internal.
    static const std::set<std::wstring> skip = {
        L"System.ItemNameDisplay",       L"System.ItemNameDisplayWithoutExtension",
        L"System.ItemFolderNameDisplay", L"System.ItemFolderPathDisplay",
        L"System.ItemFolderPathDisplayNarrow", L"System.ItemPathDisplay",
        L"System.ItemPathDisplayNarrow", L"System.ParsingName",
        L"System.ParsingPath",           L"System.FileName",
        L"System.ItemTypeText",          L"System.ItemType",
        L"System.Size",                  L"System.DateModified",
        L"System.DateCreated",           L"System.DateAccessed",
        L"System.FileAttributes",        L"System.FileExtension",
        L"System.ItemUrl",               L"System.SFGAOFlags",
        L"System.ItemDate",              L"System.ZoneIdentifier",
        L"System.ItemNameSortOverride",  L"System.FileAttributesDisplay",
    };
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreFromParsingName(f.c_str(), nullptr, GPS_BESTEFFORT, IID_PPV_ARGS(&store)))) return;
    DWORD count = 0;
    store->GetCount(&count);
    std::set<std::wstring> seen;
    for (DWORD i = 0; i < count; ++i) {
        PROPERTYKEY key;
        if (FAILED(store->GetAt(i, &key))) continue;
        PWSTR canonical = nullptr;
        if (SUCCEEDED(PSGetNameFromPropertyKey(key, &canonical))) {
            bool skipped = skip.count(canonical) > 0;
            CoTaskMemFree(canonical);
            if (skipped) continue;
        }
        ComPtr<IPropertyDescription> desc;
        if (FAILED(PSGetPropertyDescription(key, IID_PPV_ARGS(&desc)))) continue;
        PWSTR label = nullptr;
        if (FAILED(desc->GetDisplayName(&label)) || !label) continue;  // internal property
        std::wstring name = label;
        CoTaskMemFree(label);
        if (name.empty() || !seen.insert(name).second) continue;

        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(store->GetValue(key, &v)) && v.vt != VT_EMPTY && v.vt != VT_NULL && v.vt != VT_BLOB &&
            v.vt != VT_STREAM) {
            PWSTR text = nullptr;
            if (FAILED(desc->FormatForDisplay(v, PDFF_DEFAULT, &text))) PropVariantToStringAlloc(v, &text);
            if (text) {
                // Dates and numbers come wrapped in invisible bidi marks, which break copying and searching.
                std::wstring value = text;
                CoTaskMemFree(text);
                std::erase_if(value, [](wchar_t c) { return c == 0x200E || c == 0x200F || (c >= 0x202A && c <= 0x202E); });
                value = Trim(value);
                bool guid = value.size() == 38 && value.front() == L'{' && value.back() == L'}';
                if (!value.empty() && !guid) rows.push_back({group, name, value});
            }
        }
        PropVariantClear(&v);
    }
}

// --- ExifTool ----------------------------------------------------------------------

// Runs ExifTool with `args` passed through a UTF-8 argument file, so any file name works.
ProcessResult RunExifTool(const fs::path& exe, const std::vector<std::wstring>& args) {
    fs::path dir = MakeTempDir();
    fs::path argfile = dir / L"args.txt";
    std::string text = "-charset\nfilename=utf8\n";
    for (const auto& a : args) text += WideToUtf8(a) + "\n";
    WriteFileBytes(argfile, text);
    wchar_t shortPath[MAX_PATH];
    std::wstring argPath =
        GetShortPathNameW(argfile.c_str(), shortPath, MAX_PATH) ? std::wstring(shortPath) : argfile.wstring();
    ProcessResult r;
    try {
        r = RunProcess(exe, L"-@ \"" + argPath + L"\"", {}, size_t(32) << 20);
    } catch (...) {
        std::error_code ec;
        fs::remove_all(dir, ec);
        throw;
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
    return r;
}

// Non-empty lines of ExifTool's output, without the "-- press ENTER --" prompt that
// "exiftool(-k).exe" prints on stderr (it can arrive before the real output).
std::vector<std::wstring> OutputLines(const std::string& output) {
    std::vector<std::wstring> lines;
    std::wstring w = Utf8ToWide(output);
    size_t start = 0;
    while (start < w.size()) {
        size_t end = w.find(L'\n', start);
        std::wstring line = Trim(w.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!line.empty() && !line.starts_with(L"-- press ENTER")) lines.push_back(line);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return lines;
}

std::wstring FirstLine(const std::string& output) {
    auto lines = OutputLines(output);
    return lines.empty() ? std::wstring() : lines[0];
}

int AddExifTool(const fs::path& exe, const fs::path& f, const std::wstring& prefix, std::vector<InfoRow>& rows) {
    // -G1: specific group (EXIF IFD, GPS, XMP-dc, QuickTime, ...); -t: tab separated; -a: keep duplicates.
    // System and ExifTool groups only repeat file system details.
    auto r = RunExifTool(exe, {L"-G1", L"-t", L"-a", L"-api", L"largefilesupport=1", L"--System:all",
                               L"--ExifTool:all", f.wstring()});
    std::wstring out = Utf8ToWide(r.output);
    int tags = 0;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t end = out.find(L'\n', pos);
        std::wstring line = out.substr(pos, end == std::wstring::npos ? std::wstring::npos : end - pos);
        pos = end == std::wstring::npos ? out.size() : end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t t1 = line.find(L'\t');
        if (t1 == std::wstring::npos) continue;  // warnings, "-- press ENTER --", ...
        size_t t2 = line.find(L'\t', t1 + 1);
        std::wstring group = L"ExifTool", name, value;
        if (t2 == std::wstring::npos) {
            name = line.substr(0, t1);
            value = line.substr(t1 + 1);
        } else {
            group = L"ExifTool · " + line.substr(0, t1);
            name = line.substr(t1 + 1, t2 - t1 - 1);
            value = line.substr(t2 + 1);
        }
        value = Trim(value);
        if (Trim(name).empty() || value.empty()) continue;
        rows.push_back({prefix + group, Trim(name), value});
        ++tags;
    }
    if (tags == 0 && r.exitCode != 0) {
        std::wstring err = FirstLine(r.output);
        rows.push_back({prefix + L"ExifTool", L"Error", err.empty() ? L"No se pudo leer el archivo" : err});
    }
    return tags;
}

// --- Office --------------------------------------------------------------------------

constexpr char kOoxmlCore[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
    "<cp:coreProperties xmlns:cp=\"http://schemas.openxmlformats.org/package/2006/metadata/core-properties\" "
    "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns:dcterms=\"http://purl.org/dc/terms/\" "
    "xmlns:dcmitype=\"http://purl.org/dc/dcmitype/\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\"/>";
constexpr char kOoxmlApp[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
    "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/extended-properties\" "
    "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"/>";
constexpr char kOoxmlCustom[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\r\n"
    "<Properties xmlns=\"http://schemas.openxmlformats.org/officeDocument/2006/custom-properties\" "
    "xmlns:vt=\"http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes\"/>";
constexpr char kOdfMeta[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<office:document-meta xmlns:office=\"urn:oasis:names:tc:opendocument:xmlns:office:1.0\" "
    "office:version=\"1.2\"><office:meta/></office:document-meta>";

}  // namespace

std::vector<InfoRow> FileHashes(const fs::path& f, const std::wstring& group) {
    struct Algo {
        const wchar_t* id;
        const wchar_t* name;
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
    };
    Algo algos[] = {{BCRYPT_MD5_ALGORITHM, L"MD5"},
                    {BCRYPT_SHA1_ALGORITHM, L"SHA-1"},
                    {BCRYPT_SHA256_ALGORITHM, L"SHA-256"},
                    {BCRYPT_SHA512_ALGORITHM, L"SHA-512"}};
    auto cleanup = [&] {
        for (auto& a : algos) {
            if (a.hash) BCryptDestroyHash(a.hash);
            if (a.alg) BCryptCloseAlgorithmProvider(a.alg, 0);
            a.hash = nullptr;
            a.alg = nullptr;
        }
    };
    for (auto& a : algos) {
        if (BCryptOpenAlgorithmProvider(&a.alg, a.id, nullptr, 0) < 0 ||
            BCryptCreateHash(a.alg, &a.hash, nullptr, 0, nullptr, 0, 0) < 0) {
            cleanup();
            Fail(std::wstring(a.name) + L" no disponible");
        }
    }
    HANDLE h = OpenForRead(f);
    if (h == INVALID_HANDLE_VALUE) {
        cleanup();
        Fail(L"No se pudo leer " + f.filename().wstring());
    }
    std::vector<uint8_t> buf(1 << 20);
    DWORD n = 0;
    uint64_t total = 0;
    while (ReadFile(h, buf.data(), (DWORD)buf.size(), &n, nullptr) && n) {
        total += n;
        for (auto& a : algos) BCryptHashData(a.hash, buf.data(), n, 0);
    }
    CloseHandle(h);

    std::vector<InfoRow> rows;
    for (auto& a : algos) {
        DWORD len = 0, got = 0;
        BCryptGetProperty(a.alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&len), sizeof(len), &got, 0);
        std::vector<uint8_t> digest(len);
        BCryptFinishHash(a.hash, digest.data(), len, 0);
        std::wstring hex;
        wchar_t two[3];
        for (uint8_t b : digest) {
            swprintf_s(two, L"%02x", b);
            hex += two;
        }
        rows.push_back({group, a.name, hex});
    }
    cleanup();
    rows.push_back({group, L"Tamaño", SizeText(total)});
    return rows;
}

fs::path ExifToolPath() {
    // Looked up every time so installing it while FileTools runs just works.
    std::error_code ec;
    fs::path configured = ReadSettingString(L"ExifToolPath");
    if (!configured.empty() && fs::is_regular_file(configured, ec)) return configured;
    for (const wchar_t* name : {L"exiftool.exe", L"exiftool(-k).exe"}) {
        fs::path p = FindProgram(name);
        if (!p.empty()) return p;
    }
    wchar_t buf[MAX_PATH];
    std::vector<fs::path> candidates;
    if (GetEnvironmentVariableW(L"ProgramFiles", buf, MAX_PATH))
        candidates.push_back(fs::path(buf) / L"ExifTool" / L"ExifTool.exe");
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH))
        candidates.push_back(fs::path(buf) / L"Programs" / L"ExifTool" / L"ExifTool.exe");
    for (auto& c : candidates)
        if (fs::exists(c, ec)) return c;
    return {};
}

void SetConfiguredExifTool(const fs::path& exe) { WriteSettingString(L"ExifToolPath", exe.wstring()); }

std::wstring ExifToolVersion(const fs::path& exe) {
    try {
        auto r = RunProcess(exe, L"-ver");
        if (r.exitCode != 0) return {};
        for (const auto& line : OutputLines(r.output))
            if (iswdigit(line[0]) && line.find_first_not_of(L"0123456789.") == std::wstring::npos) return line;
    } catch (const AppError&) {
    }
    return {};
}

int CollectMetadata(const fs::path& f, const std::wstring& prefix, std::vector<InfoRow>& rows) {
    AddFileSystem(f, prefix + L"Archivo", rows);
    AddShellProperties(f, prefix + L"Windows", rows);
    std::error_code ec;
    fs::path exe = ExifToolPath();
    if (exe.empty() || fs::is_directory(f, ec)) return exe.empty() ? -1 : 0;
    return AddExifTool(exe, f, prefix, rows);
}

bool StripWithExifTool(const fs::path& in, const fs::path& out, bool keepOrientation, std::wstring& error) {
    fs::path exe = ExifToolPath();
    if (exe.empty()) {
        error = L"ExifTool no está instalado";
        return false;
    }
    std::vector<std::wstring> args = {L"-all="};
    // Dropping the orientation would turn photos sideways, and the colour profile is not personal data.
    if (keepOrientation) args.insert(args.end(), {L"-tagsfromfile", L"@", L"-icc_profile", L"-orientation"});
    args.insert(args.end(), {L"-api", L"largefilesupport=1", L"-o", out.wstring(), in.wstring()});
    auto r = RunExifTool(exe, args);
    std::error_code ec;
    if (r.exitCode == 0 && fs::exists(out, ec)) return true;
    fs::remove(out, ec);
    error = FirstLine(r.output);
    if (error.starts_with(L"Error: ")) error.erase(0, 7);
    if (error.empty()) error = L"ExifTool no pudo quitar los metadatos";
    return false;
}

bool IsOfficeExt(const std::wstring& ext) {
    static const std::set<std::wstring> exts = {L"docx", L"docm", L"dotx", L"xlsx", L"xlsm", L"xltx", L"pptx",
                                                L"pptm", L"potx", L"odt",  L"ods",  L"odp",  L"odg"};
    return exts.count(ext) > 0;
}

void StripOfficeMetadata(const fs::path& in, const fs::path& out) {
    ZipReader zip(in);
    ZipWriter w;
    bool changed = false;
    for (const auto& name : zip.Names()) {
        if (name == "docProps/core.xml") w.Add(name, kOoxmlCore);
        else if (name == "docProps/app.xml") w.Add(name, kOoxmlApp);
        else if (name == "docProps/custom.xml") w.Add(name, kOoxmlCustom);
        else if (name == "meta.xml") w.Add(name, kOdfMeta);
        else {
            auto data = zip.Read(name);
            w.Add(name, data.data(), data.size());
            continue;
        }
        changed = true;
    }
    if (!changed) Fail(L"El documento no tiene propiedades que quitar");
    WriteFileBytes(out, w.Finish());
}

bool HasAlternateStreams(const fs::path& f) { return !AlternateStreams(f).empty(); }
