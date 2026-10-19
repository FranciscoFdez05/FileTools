// Winsock must come before <windows.h> (pulled in by common.h), which would otherwise bring in winsock 1.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>

#include "fileserver.h"

#include <bcrypt.h>

#include <algorithm>
#include <cctype>
#include <map>

namespace {

constexpr size_t kMaxClients = 32;
constexpr size_t kChunk = 256 * 1024;

bool SendAll(SOCKET s, const char* data, size_t size, const std::atomic<bool>& stopping) {
    while (size > 0 && !stopping) {
        int n = send(s, data, (int)std::min<size_t>(size, 1 << 20), 0);
        if (n <= 0) return false;
        data += n;
        size -= size_t(n);
    }
    return size == 0;
}

bool SendAll(SOCKET s, const std::string& text, const std::atomic<bool>& stopping) {
    return SendAll(s, text.data(), text.size(), stopping);
}

std::string HtmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&#39;"; break;
            default: out += c;
        }
    }
    return out;
}

// Percent-encodes everything but unreserved characters (RFC 3986), so it is valid both in URL paths and
// in RFC 5987 filename* parameters.
std::string PercentEncode(const std::string& s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string MimeType(const fs::path& p) {
    static const std::map<std::wstring, const char*> types = {
        {L"jpg", "image/jpeg"},        {L"jpeg", "image/jpeg"},       {L"png", "image/png"},
        {L"gif", "image/gif"},         {L"webp", "image/webp"},       {L"avif", "image/avif"},
        {L"heic", "image/heic"},       {L"bmp", "image/bmp"},         {L"svg", "image/svg+xml"},
        {L"pdf", "application/pdf"},   {L"mp4", "video/mp4"},         {L"m4v", "video/mp4"},
        {L"mov", "video/quicktime"},   {L"webm", "video/webm"},       {L"mkv", "video/x-matroska"},
        {L"mp3", "audio/mpeg"},        {L"m4a", "audio/mp4"},         {L"wav", "audio/wav"},
        {L"ogg", "audio/ogg"},         {L"opus", "audio/ogg"},        {L"flac", "audio/flac"},
        {L"txt", "text/plain; charset=utf-8"}, {L"md", "text/plain; charset=utf-8"},
        {L"zip", "application/zip"},   {L"apk", "application/vnd.android.package-archive"},
        {L"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {L"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {L"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    };
    auto it = types.find(ExtOf(p));
    return it == types.end() ? "application/octet-stream" : it->second;
}

bool IsPreviewable(const fs::path& p) {
    std::string mime = MimeType(p);
    return mime == "image/jpeg" || mime == "image/png" || mime == "image/gif" || mime == "image/webp";
}

std::string StatusPage(int code, const char* reason, const std::string& text) {
    std::string body = "<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width'>"
                       "<title>FileTools</title><body style='font:16px system-ui;padding:24px'><p>" +
                       HtmlEscape(text) + "</p>";
    return "HTTP/1.1 " + std::to_string(code) + " " + reason +
           "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) +
           "\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n" + body;
}

std::string RandomToken() {
    static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
    uint8_t bytes[8];
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        Fail(L"No se pudo generar el enlace");
    std::string t;
    for (uint8_t b : bytes) t += alphabet[b % (sizeof(alphabet) - 1)];
    return t;
}

// "bytes=a-b", "bytes=a-" or "bytes=-n". False if the header is malformed or out of range.
bool ParseRange(const std::string& header, uint64_t size, uint64_t& start, uint64_t& end) {
    if (header.rfind("bytes=", 0) != 0 || header.find(',') != std::string::npos || size == 0) return false;
    std::string spec = header.substr(6);
    size_t dash = spec.find('-');
    if (dash == std::string::npos) return false;
    std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
    try {
        if (a.empty()) {
            uint64_t n = std::stoull(b);
            if (n == 0) return false;
            start = n >= size ? 0 : size - n;
            end = size - 1;
        } else {
            start = std::stoull(a);
            end = b.empty() ? size - 1 : std::min<uint64_t>(std::stoull(b), size - 1);
        }
    } catch (...) {
        return false;
    }
    return start <= end && start < size;
}

}  // namespace

std::vector<LanAddress> LanAddresses() {
    ULONG len = 16 * 1024;
    std::vector<uint8_t> buf;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int i = 0; i < 3 && rc == ERROR_BUFFER_OVERFLOW; ++i) {
        buf.resize(len);
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &len);
    }
    if (rc != NO_ERROR) return {};
    std::vector<std::pair<int, LanAddress>> scored;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        std::wstring desc = ToLower(std::wstring(a->Description ? a->Description : L"") + L" " +
                                    (a->FriendlyName ? a->FriendlyName : L""));
        int score = 0;
        if (a->FirstGatewayAddress) score += 4;  // reaches a router: the network other devices are on
        if (a->IfType == IF_TYPE_IEEE80211) score += 2;
        else if (a->IfType == IF_TYPE_ETHERNET_CSMACD) score += 1;
        for (const wchar_t* v : {L"virtual", L"hyper-v", L"vethernet", L"vmware", L"virtualbox", L"wsl", L"vpn",
                                 L"tap-", L"tailscale", L"zerotier", L"loopback"})
            if (desc.find(v) != std::wstring::npos) score -= 6;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            if (sin->sin_family != AF_INET) continue;
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&sin->sin_addr);
            if (b[0] == 169 && b[1] == 254) continue;  // no DHCP answer
            if (b[0] == 127) continue;
            wchar_t ip[INET_ADDRSTRLEN] = {};
            InetNtopW(AF_INET, &sin->sin_addr, ip, INET_ADDRSTRLEN);
            scored.push_back({score, {ip, a->FriendlyName ? a->FriendlyName : L""}});
        }
    }
    std::stable_sort(scored.begin(), scored.end(), [](auto& x, auto& y) { return x.first > y.first; });
    std::vector<LanAddress> out;
    for (auto& s : scored) out.push_back(std::move(s.second));
    return out;
}

struct FileServer::Client {
    SOCKET sock = INVALID_SOCKET;
    bool closed = false;  // closed by the server while stopping (guarded by mu_)
    std::atomic<bool> done{false};
    std::thread thread;
};

FileServer::FileServer(std::vector<fs::path> files) : files_(std::move(files)) {
    if (files_.empty()) Fail(L"No hay archivos que enviar");
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) Fail(L"No se pudo iniciar la red");
    token_ = RandomToken();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        WSACleanup();
        Fail(L"No se pudo crear el servidor");
    }
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    // A fixed, memorable port when free; any port otherwise.
    bool bound = false;
    for (uint16_t port : {8765, 8766, 8767, 8768, 8769, 8780, 8781, 8782, 0}) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(port);
        if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            bound = true;
            break;
        }
    }
    sockaddr_in local{};
    int localLen = sizeof(local);
    if (!bound || listen(s, SOMAXCONN) != 0 || getsockname(s, reinterpret_cast<sockaddr*>(&local), &localLen) != 0) {
        closesocket(s);
        WSACleanup();
        Fail(L"No se pudo abrir un puerto para el servidor");
    }
    port_ = ntohs(local.sin_port);
    listen_ = s;
    acceptThread_ = std::thread([this] { AcceptLoop(); });
}

FileServer::~FileServer() {
    stopping_ = true;
    closesocket(SOCKET(listen_));  // wakes accept()
    if (acceptThread_.joinable()) acceptThread_.join();
    {
        // Closing a socket aborts the blocking send/recv another thread is doing on it.
        std::lock_guard lock(mu_);
        for (auto& c : clients_)
            if (!c->closed) {
                c->closed = true;
                closesocket(c->sock);
            }
    }
    for (auto& c : clients_)
        if (c->thread.joinable()) c->thread.join();
    clients_.clear();
    WSACleanup();
}

std::wstring FileServer::Url(const std::wstring& ip) const {
    return L"http://" + ip + L":" + std::to_wstring(port_) + L"/" + Utf8ToWide(token_) + L"/";
}

FileServer::Stats FileServer::GetStats() const {
    std::lock_guard lock(mu_);
    return Stats{completed_, active_, sent_, total_, lastClient_};
}

void FileServer::AcceptLoop() {
    while (!stopping_) {
        sockaddr_in addr{};
        int len = sizeof(addr);
        SOCKET s = accept(SOCKET(listen_), reinterpret_cast<sockaddr*>(&addr), &len);
        if (s == INVALID_SOCKET) {
            if (stopping_) break;
            Sleep(50);
            continue;
        }
        wchar_t ip[INET_ADDRSTRLEN] = {};
        InetNtopW(AF_INET, &addr.sin_addr, ip, INET_ADDRSTRLEN);
        std::lock_guard lock(mu_);
        // Reap finished connections.
        for (auto it = clients_.begin(); it != clients_.end();) {
            if ((*it)->done) {
                (*it)->thread.join();
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
        if (clients_.size() >= kMaxClients || stopping_) {
            closesocket(s);
            continue;
        }
        auto c = std::make_unique<Client>();
        c->sock = s;
        Client* raw = c.get();
        c->thread = std::thread([this, raw, peer = std::wstring(ip)] {
            Serve(raw, peer);
            {
                std::lock_guard lock(mu_);
                if (!raw->closed) {
                    raw->closed = true;
                    closesocket(raw->sock);
                }
            }
            raw->done = true;
        });
        clients_.push_back(std::move(c));
    }
}

void FileServer::Serve(Client* c, std::wstring peer) {
    SOCKET s = c->sock;
    DWORD timeout = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    std::string req;
    char buf[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16 * 1024 && !stopping_) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return;
        req.append(buf, n);
    }
    size_t lineEnd = req.find("\r\n");
    if (lineEnd == std::string::npos) return;
    std::string line = req.substr(0, lineEnd);
    size_t sp1 = line.find(' '), sp2 = line.rfind(' ');
    if (sp1 == std::string::npos || sp2 <= sp1) return;
    std::string method = line.substr(0, sp1), target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    bool head = method == "HEAD";
    if (method != "GET" && !head) {
        SendAll(s, StatusPage(405, "Method Not Allowed", "Método no admitido."), stopping_);
        return;
    }
    std::string range;
    for (size_t pos = lineEnd + 2; pos < req.size();) {
        size_t end = req.find("\r\n", pos);
        if (end == std::string::npos || end == pos) break;
        std::string h = req.substr(pos, end - pos);
        size_t colon = h.find(':');
        if (colon != std::string::npos) {
            std::string name = h.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return char(tolower(ch)); });
            if (name == "range") range = h.substr(h.find_first_not_of(' ', colon + 1));
        }
        pos = end + 2;
    }

    std::string path = target, query;
    if (size_t q = target.find('?'); q != std::string::npos) {
        path = target.substr(0, q);
        query = target.substr(q + 1);
    }
    std::string prefix = "/" + token_ + "/";
    if (path == "/" + token_) {
        SendAll(s, "HTTP/1.1 301 Moved Permanently\r\nLocation: " + prefix + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                stopping_);
        return;
    }
    if (path.rfind(prefix, 0) != 0) {
        SendAll(s, StatusPage(404, "Not Found", "Este enlace no es válido o ya no se está compartiendo."), stopping_);
        return;
    }
    std::string rest = path.substr(prefix.size());
    if (rest.empty()) {
        {
            std::lock_guard lock(mu_);
            lastClient_ = peer;
        }
        SendPage(s, head);
        return;
    }
    size_t slash = rest.find('/');
    size_t index = files_.size();
    try {
        index = std::stoul(rest.substr(0, slash));
    } catch (...) {
    }
    if (index >= files_.size()) {
        SendAll(s, StatusPage(404, "Not Found", "Archivo no encontrado."), stopping_);
        return;
    }
    {
        std::lock_guard lock(mu_);
        lastClient_ = peer;
    }
    SendFile(s, index, range, head, query == "view");
}

void FileServer::SendPage(uintptr_t sock, bool head) {
    SOCKET s = SOCKET(sock);
    uint64_t totalSize = 0;
    std::string items;
    for (size_t i = 0; i < files_.size(); ++i) {
        std::error_code ec;
        uint64_t size = fs::file_size(files_[i], ec);
        totalSize += ec ? 0 : size;
        std::string name = WideToUtf8(files_[i].filename().wstring());
        std::string href = std::to_string(i) + "/" + PercentEncode(name);
        items += "<li>";
        if (IsPreviewable(files_[i]))
            items += "<img src='" + href + "?view' alt='' loading=lazy>";
        items += "<div class=meta><span class=name>" + HtmlEscape(name) + "</span><span class=size>" +
                 HtmlEscape(WideToUtf8(ec ? L"—" : FormatSize(size))) + "</span></div><a class=btn href='" + href +
                 "' download>Descargar</a></li>";
    }
    std::string title = files_.size() == 1 ? WideToUtf8(files_[0].filename().wstring())
                                           : std::to_string(files_.size()) + " archivos";
    std::string body =
        "<!doctype html><html lang=es><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>" + HtmlEscape(title) + " · FileTools</title><style>"
        ":root{color-scheme:light dark;--bg:#f3f5f9;--card:#fff;--ink:#16181e;--muted:#60646f;--accent:#0067d6}"
        "@media(prefers-color-scheme:dark){:root{--bg:#15171c;--card:#22252c;--ink:#eef0f4;--muted:#9aa0ab;--accent:#4aa3ff}}"
        "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);"
        "font:16px/1.4 system-ui,-apple-system,'Segoe UI',sans-serif}"
        "main{max-width:640px;margin:0 auto;padding:24px 16px}h1{font-size:20px;margin:0 0 4px}"
        ".sub{color:var(--muted);margin:0 0 20px;font-size:14px}ul{list-style:none;margin:0;padding:0}"
        "li{background:var(--card);border-radius:14px;padding:14px;margin-bottom:12px;display:flex;flex-wrap:wrap;"
        "align-items:center;gap:12px;box-shadow:0 1px 3px rgba(0,0,0,.08)}"
        "li img{width:100%;max-height:320px;object-fit:contain;border-radius:8px}"
        ".meta{flex:1;min-width:0;display:flex;flex-direction:column}"
        ".name{font-weight:600;overflow-wrap:anywhere}.size{color:var(--muted);font-size:14px}"
        ".btn{background:var(--accent);color:#fff;text-decoration:none;padding:10px 18px;border-radius:999px;"
        "font-weight:600;white-space:nowrap}footer{color:var(--muted);font-size:13px;text-align:center;margin-top:20px}"
        "</style><main><h1>" + HtmlEscape(title) + "</h1><p class=sub>" +
        HtmlEscape(std::to_string(files_.size()) + (files_.size() == 1 ? " archivo · " : " archivos · ") +
                   WideToUtf8(FormatSize(totalSize))) +
        "</p><ul>" + items + "</ul><footer>Compartido desde FileTools</footer></main></html>";
    std::string header = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    if (SendAll(s, header, stopping_) && !head) SendAll(s, body, stopping_);
}

void FileServer::SendFile(uintptr_t sock, size_t index, const std::string& range, bool head, bool inline_) {
    SOCKET s = SOCKET(sock);
    const fs::path& file = files_[index];
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        SendAll(s, StatusPage(404, "Not Found", "El archivo ya no existe en el ordenador."), stopping_);
        return;
    }
    LARGE_INTEGER li{};
    GetFileSizeEx(h, &li);
    uint64_t size = uint64_t(li.QuadPart), start = 0, end = size ? size - 1 : 0;
    bool partial = false;
    if (!range.empty()) {
        if (!ParseRange(range, size, start, end)) {
            CloseHandle(h);
            SendAll(s, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" + std::to_string(size) +
                           "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
                    stopping_);
            return;
        }
        partial = true;
    }
    uint64_t length = size ? end - start + 1 : 0;
    std::string name = WideToUtf8(file.filename().wstring()), ascii;
    for (unsigned char c : name) ascii += (c >= 32 && c < 127 && c != '"' && c != '\\') ? char(c) : '_';
    std::string header = std::string("HTTP/1.1 ") + (partial ? "206 Partial Content" : "200 OK") +
                         "\r\nContent-Type: " + MimeType(file) + "\r\nContent-Length: " + std::to_string(length) +
                         "\r\nAccept-Ranges: bytes\r\nContent-Disposition: " + (inline_ ? "inline" : "attachment") +
                         "; filename=\"" + ascii + "\"; filename*=UTF-8''" + PercentEncode(name) +
                         "\r\nCache-Control: no-store\r\nConnection: close\r\n";
    if (partial)
        header += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                  std::to_string(size) + "\r\n";
    header += "\r\n";
    if (!SendAll(s, header, stopping_) || head) {
        CloseHandle(h);
        return;
    }
    // Previews and video seeks are not downloads: only count whole-file transfers.
    bool counted = !inline_ && start == 0 && length == size;
    if (counted) {
        std::lock_guard lock(mu_);
        ++active_;
        total_ += length;
    }
    LARGE_INTEGER pos{};
    pos.QuadPart = LONGLONG(start);
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    std::vector<char> chunk(kChunk);
    uint64_t left = length, done = 0;
    bool ok = true;
    while (left > 0 && ok && !stopping_) {
        DWORD want = DWORD(std::min<uint64_t>(left, chunk.size())), got = 0;
        if (!ReadFile(h, chunk.data(), want, &got, nullptr) || got == 0) break;
        ok = SendAll(s, chunk.data(), got, stopping_);
        left -= got;
        if (counted && ok) {
            std::lock_guard lock(mu_);
            sent_ += got;
            done += got;
        }
    }
    CloseHandle(h);
    if (counted) {
        std::lock_guard lock(mu_);
        --active_;
        total_ -= length;
        sent_ -= done;
        if (ok && left == 0) ++completed_;
    }
}
