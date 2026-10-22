// Runtime configuration (set by the host executable before runtime_init).
#pragma once
#include <string>
#include <vector>

namespace dish {

struct RuntimeConfig {
    std::string game_root;      // host directory holding the user's Dishonored install
    std::string save_dir;       // host directory for saves / config / logs / temp (created on demand)
    std::string guest_game_dir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Dishonored";
    std::string guest_user_dir = "C:\\Users\\Player";
    std::vector<std::string> args;   // extra command-line arguments for the guest process
    std::string exe_relpath = "Binaries/Win32/Dishonored.exe";
    bool verify_hashes = true;       // loader: refuse modules whose SHA-256 differs from guest_program
    bool game_root_writes_to_overlay = true;   // writes under the game root go to <save_dir>/GameOverlay
};

RuntimeConfig &config();

}  // namespace dish
