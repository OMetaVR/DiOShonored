// Virtual file system: Windows paths (drive letters, backslashes, case-insensitive) <-> host paths.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace dish {

struct DirEntry {
    std::string name;     // real (host) name, UTF-8
    bool is_dir;
    uint64_t size;
    int64_t mtime_ns;     // unix time ns
};

// Build the mount table from config(). Safe to call again (tests).
void vfs_init();
void vfs_clear_cache();

// Normalise a path (UTF-8, any slashes, relative to the current directory) into canonical form "C:\dir\file".
std::string vfs_canon(const std::string &path);
std::string vfs_getcwd();
bool vfs_setcwd(const std::string &path);   // must exist as a directory

// Resolve for reading/stat. On success fills *host (and *is_dir). Virtual ancestor directories (e.g. "C:\Users") resolve
// with *host empty and *is_dir = true.
bool vfs_resolve(const std::string &canon, std::string *host, bool *is_dir);
// Resolve for creation/write. The parent directory must exist. Returns false (and sets *err) if not creatable.
bool vfs_resolve_create(const std::string &canon, std::string *host, uint32_t *err);
// Copy-on-write support for opening an existing game-root file for writing: returns the writable host path.
bool vfs_writable_for_existing(const std::string &canon, std::string *host, uint32_t *err);
bool vfs_list(const std::string &canon, std::vector<DirEntry> *out);
// Invalidate cached directory listing after a create/delete/rename done through the VFS.
void vfs_note_change(const std::string &host_path);
bool vfs_mkdir(const std::string &canon, uint32_t *err);

// Guest path of the game directory / user directories
std::string vfs_game_dir();
std::string vfs_known_folder(int csidl);    // CSIDL_* -> canonical guest path ("" if unknown)
// For logging: map a host path back to a guest path if possible
std::string vfs_host_to_guest(const std::string &host);

}  // namespace dish
