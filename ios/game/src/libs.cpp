// Library location. SDL3 is linked statically (LiveContainer crashes at launch for apps that ship Frameworks/), and so is the
// iOS d3d9 backend. Optional dynamic libraries (a DXVK + MoltenVK experiment) are looked up in a configurable folder next to the
// executable and passed to DXVK / SDL as absolute paths, since dlopen() under LiveContainer does not search the app bundle:
//   DISH_LIB_DIR=<name>   folder inside the app bundle to try first ("Libs" or "Frameworks"); both are tried
//   DXVK_VULKAN_LIBRARY / SDL_VULKAN_LIBRARY / DISH_DXVK_LIB   explicit absolute paths, always win
#include "libs.h"
#include <SDL3/SDL.h>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <vector>

static bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

std::string dish_ios_configure_libraries() {
    std::string base = SDL_GetBasePath() ? SDL_GetBasePath() : "";   // the app bundle directory, with trailing '/'
    std::vector<std::string> dirs;
    if (const char *e = getenv("DISH_LIB_DIR")) dirs.push_back(e);
    dirs.push_back("Libs");
    dirs.push_back("Frameworks");
    std::string used;
    for (const std::string &d : dirs) {
        std::string dir = base + d + "/";
        std::string mvk = dir + "MoltenVK.framework/MoltenVK";
        if (!exists(mvk)) mvk = dir + "libMoltenVK.dylib";
        std::string dxvk = dir + "libdxvk_d3d9.0.dylib";
        if (!exists(mvk) && !exists(dxvk)) continue;
        if (exists(mvk)) {
            setenv("DXVK_VULKAN_LIBRARY", mvk.c_str(), 0);
            setenv("SDL_VULKAN_LIBRARY", mvk.c_str(), 0);
        }
        if (exists(dxvk)) setenv("DISH_DXVK_LIB", dxvk.c_str(), 0);
        used = dir;
        break;
    }
    return used.empty() ? "SDL3 static, no optional Libs/ or Frameworks/ folder" : "SDL3 static, d3d9 libraries from " + used;
}
