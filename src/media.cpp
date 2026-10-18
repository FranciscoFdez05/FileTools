#include "media.h"

#include <algorithm>

namespace {
bool In(const std::wstring& ext, std::initializer_list<const wchar_t*> list) {
    return std::any_of(list.begin(), list.end(), [&](const wchar_t* e) { return ext == e; });
}

std::wstring LastLines(const std::string& output, size_t lines) {
    std::wstring w = Utf8ToWide(output);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
    size_t pos = w.size();
    for (size_t i = 0; i < lines && pos != std::wstring::npos && pos > 0; ++i) pos = w.rfind(L'\n', pos - 1);
    return pos == std::wstring::npos ? w : w.substr(pos + 1);
}

fs::path TarPath() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    fs::path p = fs::path(sys) / L"tar.exe";
    std::error_code ec;
    if (!fs::exists(p, ec)) Fail(L"No se encontró tar.exe (requiere Windows 10 1803 o superior)");
    return p;
}
}  // namespace

bool IsAudioExt(const std::wstring& ext) {
    return In(ext, {L"mp3", L"m4a", L"aac", L"wav", L"flac", L"ogg", L"oga", L"opus", L"aif", L"aiff", L"wma"});
}

bool IsVideoExt(const std::wstring& ext) {
    return In(ext, {L"mp4", L"m4v", L"mov", L"mkv", L"webm", L"avi", L"wmv", L"flv", L"mpg", L"mpeg", L"3gp",
                    L"ts", L"mts"});
}

bool IsArchiveExt(const std::wstring& ext) {
    return In(ext, {L"zip", L"tar", L"tgz", L"gz", L"7z", L"rar", L"bz2", L"xz"});
}

fs::path FfmpegPath() {
    static fs::path cached = FindProgram(L"ffmpeg.exe");
    return cached;
}

void Ffmpeg(const fs::path& in, const std::wstring& args, const fs::path& out) {
    fs::path exe = FfmpegPath();
    if (exe.empty())
        Fail(L"Audio y vídeo necesitan ffmpeg. Instálalo con: winget install Gyan.FFmpeg (o copia ffmpeg.exe junto a "
             L"FileTools.exe)");
    auto r = RunProcess(exe, L"-hide_banner -nostdin -y -i " + Quote(in) + L" " + args + L" " + Quote(out));
    if (r.exitCode != 0) {
        std::error_code ec;
        fs::remove(out, ec);
        Fail(L"ffmpeg falló: " + LastLines(r.output, 1));
    }
}

std::wstring FfmpegArgsFor(const std::wstring& ext, bool video) {
    if (ext == L"mp3") return L"-vn -c:a libmp3lame -q:a 2";
    if (ext == L"m4a") return L"-vn -c:a aac -b:a 192k";
    if (ext == L"wav") return L"-vn -c:a pcm_s16le";
    if (ext == L"flac") return L"-vn -c:a flac";
    if (ext == L"ogg") return L"-vn -c:a libvorbis -q:a 5";
    if (ext == L"opus") return L"-vn -c:a libopus -b:a 128k";
    if (ext == L"aiff") return L"-vn -c:a pcm_s16be";
    if (ext == L"wma") return L"-vn -c:a wmav2 -b:a 192k";
    if (ext == L"mp4" || ext == L"mov")
        return L"-c:v libx264 -preset medium -crf 21 -pix_fmt yuv420p -c:a aac -b:a 160k -movflags +faststart";
    if (ext == L"mkv") return L"-c:v libx264 -preset medium -crf 21 -c:a aac -b:a 160k";
    if (ext == L"webm") return L"-c:v libvpx-vp9 -crf 32 -b:v 0 -row-mt 1 -c:a libopus -b:a 128k";
    if (ext == L"avi") return L"-c:v mpeg4 -q:v 3 -c:a libmp3lame -q:a 3";
    if (ext == L"wmv") return L"-c:v wmv2 -q:v 3 -c:a wmav2 -b:a 160k";
    if (ext == L"gif")
        return L"-vf \"fps=12,scale='min(640,iw)':-2:flags=lanczos,split[a][b];[a]palettegen=stats_mode=diff[p];[b]["
               L"p]paletteuse=dither=sierra2_4a\" -loop 0";
    (void)video;
    return L"";
}

void ExtractArchive(const fs::path& archive, const fs::path& destDir) {
    fs::create_directories(destDir);
    auto r = RunProcess(TarPath(), L"-xf " + Quote(archive) + L" -C " + Quote(destDir));
    if (r.exitCode != 0) Fail(L"No se pudo extraer: " + LastLines(r.output, 1));
}

void CreateArchive(const fs::path& archive, const fs::path& baseDir, const std::vector<std::wstring>& names) {
    std::wstring ext = ExtOf(archive);
    std::wstring flags = ext == L"zip" ? L"-a -cf " : (ext == L"tgz" ? L"-czf " : L"-cf ");
    std::wstring args = flags + Quote(archive) + L" -C " + Quote(baseDir);
    for (const auto& n : names) args += L" " + Quote(n);
    auto r = RunProcess(TarPath(), args);
    if (r.exitCode != 0) {
        std::error_code ec;
        fs::remove(archive, ec);
        Fail(L"No se pudo comprimir: " + LastLines(r.output, 1));
    }
}
