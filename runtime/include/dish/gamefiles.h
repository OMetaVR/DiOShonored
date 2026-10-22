// Locating and verifying the user's game files on the host (used by the iOS import screen and `--check`-style tools).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "guest_abi.h"

namespace dish {

struct FileCheck {
    enum State { Ok, Missing, Mismatch, Unreadable };
    std::string name;      // "Dishonored.exe"
    std::string relpath;   // "Binaries/Win32/Dishonored.exe"
    std::string host;      // resolved host path ("" when missing)
    State state = Missing;
    uint64_t size = 0;
};

// Resolve `relpath` ('/' separated) below `root` ignoring case. Returns false when any component is missing.
bool gamefiles_resolve(const std::string &root, const std::string &relpath, std::string *host);

// Look for a directory that contains `marker_relpath` (e.g. "Binaries/Win32/Dishonored.exe"): `search_dir` itself, then its
// children, then theirs (depth <= 2), case-insensitive. Returns the first hit or "".
std::string gamefiles_find_root(const std::string &search_dir, const std::string &marker_relpath);

// Check every module of `prog` under `root`: presence, and SHA-256 when `hash` is true. Returns true when all are Ok.
// `progress(i, n)` is called before each file (it can take a while to hash a 25 MB exe on a phone).
bool gamefiles_verify(const GuestProgram &prog, const std::string &root, bool hash, std::vector<FileCheck> *out,
                      const std::function<void(size_t, size_t)> &progress = {});

}  // namespace dish
