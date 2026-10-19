#pragma once
#include "common.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

// IPv4 address of a local network adapter, best candidates first.
struct LanAddress {
    std::wstring ip;       // "192.168.1.34"
    std::wstring adapter;  // "Wi-Fi"
};
std::vector<LanAddress> LanAddresses();

// Minimal HTTP server that lets other devices on the network download `files` (regular files only) from
// http://<ip>:<port>/<token>/. The random token keeps the rest of the network from guessing the URL.
class FileServer {
public:
    explicit FileServer(std::vector<fs::path> files);  // throws AppError
    ~FileServer();                                       // stops and waits for every connection
    FileServer(const FileServer&) = delete;
    FileServer& operator=(const FileServer&) = delete;

    std::wstring Url(const std::wstring& ip) const;
    uint16_t Port() const { return port_; }

    struct Stats {
        int completed = 0;          // whole files sent
        int active = 0;             // downloads in progress
        uint64_t sent = 0, total = 0;  // bytes of the downloads in progress
        std::wstring lastClient;    // IP of the last device that opened the page
    };
    Stats GetStats() const;

private:
    struct Client;
    void AcceptLoop();
    void Serve(Client* c, std::wstring peer);
    void SendPage(uintptr_t sock, bool head);
    void SendFile(uintptr_t sock, size_t index, const std::string& range, bool head, bool inline_);

    std::vector<fs::path> files_;
    std::string token_;
    uint16_t port_ = 0;
    uintptr_t listen_ = ~uintptr_t(0);
    std::thread acceptThread_;
    std::atomic<bool> stopping_{false};

    mutable std::mutex mu_;
    std::vector<std::unique_ptr<Client>> clients_;
    int completed_ = 0, active_ = 0;
    uint64_t sent_ = 0, total_ = 0;
    std::wstring lastClient_;
};
