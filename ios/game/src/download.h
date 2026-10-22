#pragma once
// Dev builds: copy the user's own game files from their computer (tools/dev/game_files_server.py) into the app's Documents.
// Plain HTTP/1.1 over POSIX sockets (LAN / Tailscale), parallel workers, resumes partial files. Thread-safe progress snapshot.
#include <cstdint>
#include <mutex>
#include <string>

struct GameDownload {
    std::mutex mu;
    bool running = false, done = false, ok = false;
    std::string error, current;
    uint64_t bytes_total = 0, bytes_done = 0;
    size_t files_total = 0, files_done = 0;
    double mb_per_s = 0;
};

// Written into the game root only after every file arrived (removed when a download starts).
constexpr const char *kGameDownloadCompleteMarker = ".dish_download_complete";

// server: "host:port". Files land in dest_root/<relpath>. Returns immediately; progress in `d`.
void game_download_start(GameDownload &d, const std::string &server, const std::string &dest_root);
