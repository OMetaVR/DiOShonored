// steam_api.dll: offline null objects. Calls go through the guest vtables exactly like the game's (thiscall, callee pops).
#include "testing.h"
#include <cstring>

using namespace dish;

// obj->vtbl[slot](args...) with `this` in ecx; returns {eax, edx, pop}
static t::Call vcall(uint32_t obj, uint32_t slot, std::initializer_list<uint32_t> args) { return t::call_addr(rd32(rd32(obj) + 4 * slot), args, obj); }

TEST(offline_steam_signed_out_null_objects) {
    t::setup_runtime("steam");
    CHECK_EQ(t::hle("steam_api.dll", "SteamAPI_Init", {}).eax() & 0xFF, 1u);
    uint32_t rs = t::hle("steam_api.dll", "SteamRemoteStorage", {}).eax();
    CHECK(rs != 0);
    CHECK_EQ(t::hle("steam_api.dll", "SteamRemoteStorage", {}).eax(), rs);   // one object per interface
    auto c = vcall(rs, 11, {});                                              // GetFileCount()
    CHECK_EQ(c.eax(), 0u);
    CHECK_EQ(c.pop(), 0u);
    c = vcall(rs, 12, {0, t::galloc(4)});                                    // GetFileNameAndSize(int, int32 *)
    CHECK_EQ(c.pop(), 8u);
    CHECK(c.eax() != 0 && rd8(c.eax()) == 0);                                // "" rather than NULL
    c = vcall(rs, 9, {t::gstr("x.sav")});                                   // GetFileTimestamp -> int64
    CHECK_EQ(c.pop(), 4u);
    CHECK_EQ(c.edx(), 0u);
    uint32_t user = t::hle("steam_api.dll", "SteamUser", {}).eax();
    CHECK_EQ(vcall(user, 1, {}).eax() & 0xFF, 0u);                           // BLoggedOn
    uint32_t id = t::galloc(8);
    wr32(id, 0xDEADBEEF); wr32(id + 4, 0xDEADBEEF);
    c = vcall(user, 2, {id});                                                // CSteamID GetSteamID(): hidden result pointer
    CHECK_EQ(c.eax(), id);
    CHECK_EQ(c.pop(), 4u);
    CHECK_EQ(rd32(id), 0u);
    CHECK_EQ(rd32(id + 4), 0u);
    uint32_t friends = t::hle("steam_api.dll", "SteamFriends", {}).eax();
    c = vcall(friends, 22, {t::gstr("chat"), 1, 2});                         // ActivateGameOverlayToUser(const char *, CSteamID)
    CHECK_EQ(c.pop(), 12u);
    uint32_t utils = t::hle("steam_api.dll", "SteamUtils", {}).eax();
    CHECK_EQ(vcall(utils, 9, {}).eax(), 205100u);                            // GetAppID
    CHECK_EQ(t::hle("steam_api.dll", "SteamGameServer", {}).eax(), 0u);
}
