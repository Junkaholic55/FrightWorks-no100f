// Dev menu (F8) for Scooby-Doo! Night of 100 Frights: teleport to any scene, save spots and go back to them,
// god mode, player stats, monsters off, power-ups on/off, free camera, texture finder. Only with [game] dev_tools = true (launcher: Extras >
// Developer tools); otherwise it reserves no memory and ignores F3 (video, no100f_recorder), F7 and F8.
//
// Scene switch: the game travels through a portal by setting the current scene's pending portal ([scene+0x70]);
// the scene update (0x800CEB30) then fades out and the switch handler (0x800ACD30, state 0xB/5) reads the portal's
// asset (+0x08 camera ID, +0x0C spawn marker ID, +0x10 yaw in degrees, +0x14 scene ID) and loads that scene.
// The menu writes its own portal + asset into a reserved MEM1 page and sets the pending portal. Marker 0 = the
// scene's own start position; the yaw always overrides the player asset's (0x800ACFC4).
//
// Spot teleport (same scene): what the scene setup does at spawn (0x800AD02C): player model matrix pos, player frame
// pos, camera follow target (0x8001E938). A spot in another scene switches first, then teleports once the new scene
// has been in play for a few frames.
//
// God mode: TakeDamage from sd2.ini (globals + 0x824) is read on every hit (0x8005A430 only subtracts the damage from
// health +0x2E8 when it's nonzero); the menu holds it at 0. Scooby is tinted green through his RpGeometry flag 0x40
// (MODULATEMATERIALCOLOR, geometry = [[instance+0x10]+0x18] +8) and material colours (+4), restored when it's turned off.
//
// Monsters: base type 0x02 (NPC) entities in the scene list. Off = update function (+0x30) cleared, which the scene loop
// skips, and the entity parked off-map like mkhip.py does. Names come from the scene HIP's ADBG records on the disc.
//
// Everything runs on the game thread (settings_overlay::Draw is called from the GXCopyDisp HLE).
#include "no100f_devmenu.h"
#include "no100f_devmenu_state.h"
#include "no100f_devmenu_progress.h"
#include "no100f_game_state.h"
#include "no100f_recorder.h"
#include "no100f_resources.h"
#include "build_info.h"
#include "overlay_theme.h"

#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cfloat>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <aurora/window_owner_sdl.hpp>
#include <dolphin/pad.h>

#include "abi_bridge.h"
#include "hle_stubs.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"

extern "C" void func_8016A300(CpuContext* ctx);  // the game's RwFrameUpdateObjects
extern "C" void func_80045AC8(CpuContext* ctx);
extern "C" void func_8001C1C4(CpuContext* ctx);
extern "C" void func_80048B2C(CpuContext* ctx);
extern "C" void func_800D5150(CpuContext* ctx);
extern "C" void func_8004C968(CpuContext* ctx);
extern "C" void func_8005A0A8(CpuContext* ctx);

namespace no100f_devmenu {
namespace {

constexpr uint32_t kSceneCurPtr = 0x80245CF4u;     // globals (0x80245190) + 0xB64: current scene
constexpr uint32_t kPlayerFramePtr = 0x802453C8u;    // globals + 0x238: player entity frame (matrix first, pos at +0x30)
constexpr uint32_t kPlayerModelPtr = 0x802453B4u;  // globals + 0x224: player model instance, [+0x20] = its matrix
constexpr uint32_t kGameMode = no100f_game_state::kModeAddress;
constexpr uint32_t kGameState = no100f_game_state::kStateAddress;
constexpr uint32_t kCameraTarget = 0x801BD0A0u;    // camera follow target: current (+0), previous (+0xC)
constexpr uint32_t kAreaTable = 0x801D6428u;       // 13 x 16: name, letter << 24, index, 0
constexpr uint32_t kLevelTable = 0x801D651Cu;      // 36-byte entries: title, index, snacks, snacks, tokens, 4 scene IDs
constexpr uint32_t kTakeDamage = 0x802459B4u;      // globals + 0x824: sd2.ini TakeDamage
// Cutscene manager pointer (globals + 0x860): set while a recorded cutscene plays (h001 intro), null in play.
constexpr uint32_t kCutsceneMgr = 0x802459F0;
// globals + 0x3B0: 1 while the player has control; 0 during a recorded cutscene or an in-world camera pan (i001 key).
constexpr uint32_t kPlayerControl = 0x80245540;
constexpr uint32_t kGodTint = 0x78FF78FFu;         // material colour RGBA while god mode is on
constexpr uint8_t kNpcBaseType = 0x02;

constexpr uint32_t kReserveStart = 0x816FF000u;    // below __ArenaHi (0x81700000); lowers the arena by one page
constexpr uint32_t kFakePortal = kReserveStart;
constexpr uint32_t kFakePortalAsset = kReserveStart + 0x40u;

const bool kReserved = [] {
    if (!RuntimeConfigFile::DevTools()) return false;
    RecompMod::RegisterMemoryReservation(kReserveStart, kReserveStart + 0x1000u, "NO100F dev menu portal");
    return true;
}();

struct Scene {
    uint32_t id = 0;
    std::string label;  // "i004  part 1/3"
};
struct Level {
    std::string title;  // "20. Mind Your Manors!  [1st Floor]"
    std::vector<Scene> scenes;
};
struct Spot {
    uint32_t scene = 0;
    float x = 0, y = 0, z = 0, yaw = 0;
    std::string name;
    bool cam = false;  // saved with the free camera on: its pose comes back with the spot
    float cx = 0, cy = 0, cz = 0, cyaw = 0, cpitch = 0;
};

bool g_open = false;
bool g_focused = false;  // the menu window has focus: game input is held (click the game to move with the menu open)
bool g_toggleFocus = false;  // LB pressed: focus the menu / give focus back to the game
std::vector<Level> g_levels;
std::vector<Spot> g_spots;
bool g_spotsLoaded = false;
bool g_thisSceneOnly = true;
char g_newSpotName[64] = "";
std::string g_status;
GalleryRules g_galleryRules;
SnackConfirmation g_snackConfirmation;
uint32_t g_playerEditScene = 0, g_playerEditSceneId = 0;
bool g_playerInputsReady = false;
int g_healthInput = 3, g_snacksInput = 0, g_snackAreaInput = 0, g_snackArea = 1;

// A spot in another scene: teleport after the switch, once the scene has been in play for kSettleFrames.
constexpr int kSettleFrames = 15;
constexpr int kPendingTimeoutFrames = 60 * 60;
bool g_pendingSpot = false;
Spot g_pending;
int g_pendingSettle = 0, g_pendingAge = 0;
SceneRequest g_sceneRequest;  // queued scene change (scene list, spots in another scene)
SkipRequest g_skipRequest;    // "Skip" button / skip command
bool g_skipPressed = false;
uint32_t g_skipMode = 0, g_skipState = 0, g_skipManager = 0;  // the sequence the skip was asked for
uint16_t g_skipButtons = 0;
bool IsPlayable(uint32_t id);

bool g_god = false;
uint32_t g_takeDamageSaved = 1;
std::map<uint32_t, uint32_t> g_tintSaved;  // geometry flags / material colour address -> original word

// Entity: +0x10 asset, +0x2C model instance list (+0 next, +0x20 matrix), +0x30 update, +0x40 frame (pos +0x30,
// previous pos +0x80), +0x4C bound centre.
struct Monster {
    uint32_t base = 0, asset = 0;
    std::string name;
    bool off = false;
    uint32_t update = 0;
    std::vector<std::pair<uint32_t, uint32_t>> saved;  // position word address, original value
};
std::vector<Monster> g_monsters;
uint32_t g_monSceneId = 0, g_monList = 0, g_monCount = 0;
bool g_allMonstersOff = false;
uint32_t g_namesSceneId = 0;
std::map<uint32_t, std::string> g_npcNames;  // NPC asset ID -> name

float ReadF(uint32_t a) { return Memory::ReadFloat32(a); }
void WriteF(uint32_t a, float v) { Memory::WriteFloat32(a, v); }

uint32_t Read32Or0(uint32_t a) {
    uint32_t v = 0;
    return Memory::TryRead32(a, v) ? v : 0;
}
bool IsGuestPtr(uint32_t p) { return p >= 0x80000000u && p < 0x81800000u; }

std::string CStr(uint32_t addr, size_t max = 64) {
    std::string s;
    for (size_t i = 0; i < max && IsGuestPtr(addr + static_cast<uint32_t>(i)); ++i) {
        const uint8_t c = Memory::Read8(addr + static_cast<uint32_t>(i));
        if (!c) break;
        s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return s;
}

std::string IdString(uint32_t id) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) s[i] = static_cast<char>(id >> (24 - 8 * i));
    return s;
}
uint32_t IdFromString(const std::string& s) {
    uint32_t id = 0;
    for (int i = 0; i < 4; ++i) id = (id << 8) | static_cast<uint8_t>(i < static_cast<int>(s.size()) ? std::toupper(s[i]) : ' ');
    return id;
}
std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void LoadLevels() {
    if (!g_levels.empty()) return;
    std::string areas[128];
    for (uint32_t a = kAreaTable; a < kAreaTable + 13 * 16; a += 16) {
        const uint32_t letter = Read32Or0(a + 4) >> 24;
        if (letter < 128) areas[letter] = CStr(Read32Or0(a));
    }
    for (uint32_t a = kLevelTable;; a += 36) {
        const uint32_t title = Read32Or0(a), index = Read32Or0(a + 4);
        if (!IsGuestPtr(title) || index != g_levels.size() + 1) break;
        Level level;
        std::vector<uint32_t> ids;
        for (uint32_t k = 0; k < 4; ++k)
            if (const uint32_t id = Read32Or0(a + 20 + 4 * k)) ids.push_back(id);
        char head[160];
        const uint32_t letter = ids.empty() ? 0 : ids[0] >> 24;
        std::snprintf(head, sizeof head, "%u. %s  [%s]", index, CStr(title).c_str(),
                      letter < 128 ? areas[letter].c_str() : "?");
        level.title = head;
        for (size_t k = 0; k < ids.size(); ++k) {
            if (ids[k] == IdFromString("R006")) continue;  // listed in the table, not on the disc
            char label[64];
            std::snprintf(label, sizeof label, "%s  part %zu/%zu", Lower(IdString(ids[k])).c_str(), k + 1, ids.size());
            level.scenes.push_back({ids[k], label});
        }
        g_levels.push_back(std::move(level));
    }
    g_levels.push_back({"Other", {{IdFromString("S006"), "s006  ending"}}});
}

std::string SceneName(uint32_t id) {
    LoadLevels();
    for (const Level& level : g_levels)
        for (const Scene& scene : level.scenes)
            if (scene.id == id) return level.title + "  " + scene.label;
    return Lower(IdString(id));
}

std::filesystem::path SpotsPath() { return RuntimeConfigFile::ApplicationDataDirectory() / "devmenu_spots.txt"; }

void LoadSpots() {
    if (g_spotsLoaded) return;
    g_spotsLoaded = true;
    std::ifstream in(SpotsPath());
    for (std::string line; std::getline(in, line);) {
        std::istringstream ss(line);
        std::string scene;
        Spot s;
        if (!(ss >> scene >> s.x >> s.y >> s.z >> s.yaw)) continue;
        s.scene = IdFromString(scene);
        std::getline(ss >> std::ws, s.name);
        // optional free camera pose before the name: cam=x,y,z,yaw,pitch
        if (s.name.rfind("cam=", 0) == 0 &&
            std::sscanf(s.name.c_str(), "cam=%f,%f,%f,%f,%f", &s.cx, &s.cy, &s.cz, &s.cyaw, &s.cpitch) == 5) {
            s.cam = true;
            const size_t sp = s.name.find(' ');
            s.name = sp == std::string::npos ? "" : s.name.substr(sp + 1);
        }
        g_spots.push_back(s);
    }
}

void SaveSpots() {
    std::ofstream out(SpotsPath(), std::ios::trunc);
    for (const Spot& s : g_spots) {
        char b[224];
        int n = std::snprintf(b, sizeof b, "%s %.3f %.3f %.3f %.4f ", IdString(s.scene).c_str(), s.x, s.y, s.z, s.yaw);
        if (s.cam)
            std::snprintf(b + n, sizeof b - n, "cam=%.3f,%.3f,%.3f,%.4f,%.4f ", s.cx, s.cy, s.cz, s.cyaw, s.cpitch);
        out << b << s.name << "\n";
    }
}

uint32_t CurrentScenePtr() {
    const uint32_t scene = Read32Or0(kSceneCurPtr);
    return IsGuestPtr(scene) ? scene : 0;
}
uint32_t CurrentSceneId() {
    const uint32_t scene = CurrentScenePtr();
    return scene ? Read32Or0(scene + 0x6C) : 0;
}
bool CutscenePlaying() { return Read32Or0(kCutsceneMgr) != 0; }
bool InPlay() { return Read32Or0(kGameMode) == 0xB && Read32Or0(kGameState) == 1 && CurrentScenePtr(); }
bool ProgressRead(uint32_t address, uint32_t& value) {
    return GuestRange(address, 4) && Memory::TryRead32(address, value);
}

ProgressAvailability ProgressState() {
    const uint32_t scene = CurrentScenePtr();
    uint32_t portal = 0, movie = 0, control = 0;
    const bool readable = scene && ProgressRead(scene + 0x70, portal) &&
                          ProgressRead(no100f_game_state::kMovieAddress, movie) &&
                          ProgressRead(kPlayerControl, control);
    const CpuContext* context = TryGetCpuContext();
    const bool safeContext = context && context->gpr[1] >= 0x80001000u &&
                             GuestRange(context->gpr[1] - 0x1000, 0x1020) &&
                             Memory::Contains(context->gpr[1] - 0x1000, 0x1020);
    return {no100f_game_state::g_diagnosticLayoutVerified.load(), readable && InPlay(),
            CutscenePlaying(), movie != 0, control == 1,
            portal != 0 || g_pendingSpot || g_sceneRequest.Active(), safeContext};
}

struct GallerySnapshot {
    std::array<uint32_t, kGalleryConditionCount> bases{};
    bool pictures = false, movies = false;
    const char* reason = nullptr;
};

GallerySnapshot ReadGallery() {
    GallerySnapshot result;
    if (!no100f_game_state::g_diagnosticLayoutVerified.load()) {
        result.reason = "Unsupported executable layout.";
        return result;
    }
    const uint32_t scene = CurrentScenePtr();
    uint32_t count, list;
    if (!InPlay() || CurrentSceneId() != kGalleryScene || !ProgressRead(scene + 0x74, count) ||
        count == 0 || count > 4000 || !ProgressRead(scene + 0x78, list) || !GuestRange(list, count * 4)) {
        result.reason = "Requires the loaded H003 gallery scene.";
        return result;
    }
    std::map<uint32_t, uint32_t> objects;
    if (!ReadSceneObjects(ProgressRead, list, count, objects)) {
        result.reason = "Unreadable/ambiguous gallery scene objects.";
        return result;
    }
    const auto find = [&](uint32_t id) {
        const auto it = objects.find(id);
        return it == objects.end() ? 0u : it->second;
    };
    result.pictures = result.movies = true;
    for (size_t i = 0; i < result.bases.size(); ++i) {
        const auto spec = ConditionAt(i);
        if (!ValidateCondition(ProgressRead, find, spec, result.bases[i])) {
            (spec.section == GallerySection::Pictures ? result.pictures : result.movies) = false;
        }
    }
    if (!result.pictures || !result.movies)
        result.reason = "Missing or modified gallery condition/link schema; affected section unavailable.";
    return result;
}

void ObserveGallery() {
    const uint32_t scene = CurrentScenePtr(), id = CurrentSceneId(), mode = Read32Or0(kGameMode);
    const bool loading = mode == 11 && Read32Or0(kGameState) == 5;
    g_galleryRules.Observe(scene, id, mode, scene != 0 && !loading &&
                          no100f_game_state::g_diagnosticLayoutVerified.load());
}

uint32_t NaturalCondition(uint32_t base) {
    CpuContext call = *TryGetCpuContext();
    CpuContextScope scope(&call);
    call.gpr[3] = base;
    func_80045AC8(&call);
    return static_cast<uint32_t>(call.gpr[3]);
}

std::string WarpReadout() {
    if (!no100f_game_state::g_diagnosticLayoutVerified.load()) return "warps unavailable: unsupported executable layout";
    std::array<uint32_t, 26> flags{};
    if (!ReadWarps(ProgressRead, flags)) return "warps unavailable: unreadable/modified table or non-boolean flag";
    std::string text = "warp discovery:";
    for (size_t i = 0; i < flags.size(); ++i)
        text += " " + Lower(IdString(kWarpScenes[i])) + "=" + std::to_string(flags[i]);
    return text;
}

std::string ChangeWarp(size_t index, bool enabled) {
    if (const char* reason = ProgressState().Reason()) return std::string("warp rejected: ") + reason;
    uint32_t before = 0;
    const bool changed = WriteWarp(ProgressRead, [index](uint32_t address, uint32_t value) {
        if (value == 1) {
            CpuContext call = *TryGetCpuContext();
            CpuContextScope scope(&call);
            call.gpr[3] = static_cast<uint32_t>(index);
            func_8004C968(&call);
            return true;
        }
        return Memory::TryWrite32(address, value);
    }, index, enabled, before);
    if (!changed) return "warp failed: invalid table/index/value or write/readback failure; inspect live flags.";
    return "warp " + Lower(IdString(kWarpScenes[index])) + " " + std::to_string(before) + " -> " +
           (enabled ? "1" : "0") + "; live discovery edit; approach can unlock again; normal saves can retain it.";
}

std::string ChangeGallery(GallerySection section, GalleryRule rule) {
    ObserveGallery();
    if (const char* reason = ProgressState().Reason()) return std::string("gallery rejected: ") + reason;
    const GallerySnapshot snapshot = ReadGallery();
    if (!(section == GallerySection::Pictures ? snapshot.pictures : snapshot.movies))
        return std::string("gallery rejected: ") + snapshot.reason;
    // Let menu entry/browsing evaluate conditions; eager evaluation exposes hidden movie icons on other screens.
    g_galleryRules.Set(section, rule);
    return std::string(section == GallerySection::Pictures ? "pictures: " : "movies: ") + RuleName(rule) +
           "; gallery-only test override, progression unchanged. Reopen/browse the gallery to refresh.";
}

std::string GalleryReadout() {
    ObserveGallery();
    if (const char* reason = ProgressState().Reason()) return std::string("gallery unavailable: ") + reason;
    const GallerySnapshot snapshot = ReadGallery();
    if (!snapshot.pictures && !snapshot.movies) return std::string("gallery unavailable: ") + snapshot.reason;
    std::string text = "gallery";
    for (GallerySection section : {GallerySection::Pictures, GallerySection::Movies}) {
        text += std::string("\n  ") + (section == GallerySection::Pictures ? "pictures" : "movies") + ": ";
        if (!(section == GallerySection::Pictures ? snapshot.pictures : snapshot.movies)) {
            text += snapshot.reason;
            continue;
        }
        const auto rule = g_galleryRules.Get(section);
        text += RuleName(rule);
        for (size_t i = 0; i < snapshot.bases.size(); ++i) {
            const auto spec = ConditionAt(i);
            if (spec.section != section || (section == GallerySection::Movies && !spec.visibility)) continue;
            const bool natural = NaturalCondition(snapshot.bases[i]) != 0;
            const bool effective = rule == GalleryRule::Natural ? natural : rule == GalleryRule::Unlocked;
            text += " " + std::to_string(section == GallerySection::Pictures ? i + 1 :
                                       (i - kPictureEntries.size()) / 2 + 1) +
                    "[game=" + (natural ? "open" : "locked") + ",effective=" +
                    (effective ? "open" : "locked") + "]";
        }
    }
    return text;
}

void ProgressLog(const std::string& action, const std::string& result) {
    std::ofstream log("devmenu.log", std::ios::app);
    if (!log) {
        RT_LOGF(RT_TAG_RUNTIME, "[devmenu] cannot open devmenu.log for %s\n", action.c_str());
        g_status = result + " (Could not write devmenu.log.)";
        return;
    }
    log << action << " -> " << result << "\n";
    if (!log) RT_LOGF(RT_TAG_RUNTIME, "[devmenu] cannot write devmenu.log for %s\n", action.c_str());
}

const char* PlayerReason(PlayerStats& snapshot, bool* readable = nullptr) {
    if (readable) *readable = false;
    if (const char* reason = ProgressState().Reason()) return reason;
    if (CurrentSceneId() == 0) return "Missing current scene identity.";
    const CpuContext* context = TryGetCpuContext();
    if (!context || context->gpr[13] != kPlayerSda) return "Unsupported game-thread SDA layout.";
    if (!ReadPlayerStats(ProgressRead, snapshot)) return "Unreadable player statistics.";
    if (readable) *readable = true;
    return snapshot.Reason();
}

void ObservePlayerEdits() {
    PlayerStats snapshot;
    const char* reason = PlayerReason(snapshot);
    const uint32_t scene = CurrentScenePtr(), id = CurrentSceneId();
    const bool reset = reason || scene != g_playerEditScene || id != g_playerEditSceneId;
    SnackAreas areas{};
    if (g_snackConfirmation.active &&
        (reset || !ReadSnackAreas(ProgressRead, areas) || !g_snackConfirmation.Matches(scene, id, areas))) {
        g_snackConfirmation.Clear();
        g_status = "Collected-snack confirmation cancelled: scene, availability or area state changed.";
        ProgressLog("snackcancel", g_status);
    }
    if (reset) g_playerInputsReady = false;
    g_playerEditScene = reason ? 0 : scene;
    g_playerEditSceneId = reason ? 0 : id;
}

std::string PlayerReadout() {
    PlayerStats snapshot;
    if (const char* reason = PlayerReason(snapshot)) return std::string("player unavailable: ") + reason;
    return "player health=" + std::to_string(snapshot.health) + "/5 snacks=" + std::to_string(snapshot.snacks) +
           " payment=0; one-time live edits; collected progression is separate.";
}

std::string SnackAreaLabel(uint32_t index, const SnackArea& area) {
    return std::to_string(index) + ". " + CStr(area.title);
}

std::string SnackAreasReadout() {
    PlayerStats player;
    if (const char* reason = PlayerReason(player)) return std::string("snackareas unavailable: ") + reason;
    SnackAreas areas{};
    if (!ReadSnackAreas(ProgressRead, areas)) return "snackareas unavailable: unreadable/modified area schema or counts.";
    std::ostringstream result;
    uint32_t total = 0;
    for (uint32_t i = 1; i < areas.size(); ++i) {
        result << "\n  " << SnackAreaLabel(i, areas[i]) << " collected=" << areas[i].collected
               << " target=" << areas[i].target;
        total += areas[i].collected;
    }
    result << "\nAll real areas collected (sum)=" << total << "; physical pickups/scene-local counts unchanged.";
    return result.str();
}

std::string ChangePlayerStat(PlayerStat stat, uint32_t value) {
    PlayerStats snapshot;
    if (const char* reason = PlayerReason(snapshot)) return std::string("player edit rejected: ") + reason;
    uint32_t before;
    const bool changed = WritePlayerStat(ProgressRead, [&](uint32_t address, uint32_t count) {
        if (stat == PlayerStat::Health && count > snapshot.health) {
            CpuContext call = *TryGetCpuContext();
            CpuContextScope scope(&call);
            call.gpr[3] = count - snapshot.health;
            func_8005A0A8(&call);
            return true;
        }
        return Memory::TryWrite32(address, count);
    }, stat, value, before);
    if (!changed) return "player edit failed: invalid value/state or write/readback failure; inspect live values.";
    return std::string(stat == PlayerStat::Health ? "health " : "spendable snacks ") + std::to_string(before) +
           " -> " + std::to_string(value) + (stat == PlayerStat::Health ?
           "; one-time stat edit, not damage simulation." :
           "; collection totals/pickups unchanged; a normal save can retain it.");
}

std::string StageSnackArea(uint32_t index, uint32_t value) {
    g_snackConfirmation.Clear();
    PlayerStats player;
    if (const char* reason = PlayerReason(player)) return std::string("snackarea rejected: ") + reason;
    SnackAreas areas{};
    if (!ReadSnackAreas(ProgressRead, areas)) return "snackarea rejected: unreadable/modified area schema or counts.";
    if (!g_snackConfirmation.Stage(CurrentScenePtr(), CurrentSceneId(), index, value, areas))
        return "snackarea rejected: select area 1..29 and a count within its live target.";
    g_snackArea = static_cast<int>(index);
    g_snackAreaInput = static_cast<int>(value);
    return "CONFIRM collected progression: " + SnackAreaLabel(index, areas[index]) + " " +
           std::to_string(areas[index].collected) + " -> " + std::to_string(value) +
           ". Can affect galleries and later normal saves; balance/physical pickups unchanged. Use snackconfirm or Cancel.";
}

std::string ConfirmSnackArea() {
    const SnackConfirmation pending = g_snackConfirmation;
    g_snackConfirmation.Clear();
    PlayerStats player;
    if (const char* reason = PlayerReason(player)) return std::string("snackconfirm rejected: ") + reason;
    if (!pending.active) return "snackconfirm rejected: no staged progression edit.";
    if (!WriteSnackArea(ProgressRead, Memory::TryWrite32, pending, CurrentScenePtr(), CurrentSceneId()))
        return "snackconfirm failed: stale confirmation, unsupported state or write/readback failure; inspect counts.";
    return "area " + std::to_string(pending.area) + " collected " + std::to_string(pending.before) + " -> " +
           std::to_string(pending.value) + "; balance/other areas/physical pickups unchanged. Normal saves can retain it.";
}

bool HandlePlayerCommand(const std::string& command, const std::string& arguments) {
    if (!IsPlayerCommand(command)) return false;
    const auto parsed = ParsePlayerCommand(command, arguments);
    using Kind = PlayerCommand::Kind;
    switch (parsed.kind) {
    case Kind::Read: g_status = PlayerReadout(); break;
    case Kind::Health: g_status = ChangePlayerStat(PlayerStat::Health, parsed.value); break;
    case Kind::Snacks: g_status = ChangePlayerStat(PlayerStat::Snacks, parsed.value); break;
    case Kind::Areas: g_status = SnackAreasReadout(); break;
    case Kind::Stage: g_status = StageSnackArea(parsed.area, parsed.value); break;
    case Kind::Confirm: g_status = ConfirmSnackArea(); break;
    case Kind::Cancel: g_snackConfirmation.Clear(); g_status = "Collected-snack confirmation cancelled."; break;
    default:
        g_snackConfirmation.Clear();
        g_status = "Invalid player command. Use player | health 1..5 | snacks 0..99999 | snackareas | "
                   "snackarea INDEX COUNT (stage) | snackconfirm | snackcancel. Pending confirmation cancelled.";
        break;
    }
    return true;
}

bool HandleProgressCommand(const std::string& command, const std::string& arguments) {
    if (command != "warps" && command != "warp" && command != "gallery") return false;
    const auto parsed = ParseProgressCommand(command, arguments);
    switch (parsed.kind) {
    case ProgressCommand::Kind::Warps: g_status = WarpReadout(); break;
    case ProgressCommand::Kind::Warp: g_status = ChangeWarp(parsed.warp, parsed.enabled); break;
    case ProgressCommand::Kind::Gallery: g_status = GalleryReadout(); break;
    case ProgressCommand::Kind::GallerySet: g_status = ChangeGallery(parsed.section, parsed.rule); break;
    default:
        g_status = "Invalid command. Use warps | warp SCENE on|off | gallery [pictures|movies natural|locked|unlocked].";
        break;
    }
    return true;
}

bool SkippableSequence() {
    return SkipControlFor(Read32Or0(kGameMode), Read32Or0(kGameState), CutscenePlaying()) != SkipControl::None;
}

bool SameSkipSequence() {
    return SkippableSequence() && Read32Or0(kGameMode) == g_skipMode &&
           Read32Or0(kGameState) == g_skipState && Read32Or0(kCutsceneMgr) == g_skipManager;
}

// Presses the game's own skip button (A in a recorded cutscene, Start on the title intro / attract demo) until the
// game takes it or the sequence ends.
void RequestSkip() {
    if (!SkippableSequence()) {
        g_status = "No recorded cutscene or title/demo sequence is playing.";
        return;
    }
    g_skipRequest.Begin(SDL_GetTicks());
    g_skipMode = Read32Or0(kGameMode);
    g_skipState = Read32Or0(kGameState);
    g_skipManager = Read32Or0(kCutsceneMgr);
    g_skipButtons = SkipControlFor(g_skipMode, g_skipState, g_skipManager != 0) == SkipControl::Start
                        ? PAD_BUTTON_START : PAD_BUTTON_A;
    g_status = "Requesting the game's normal skip; waiting for the skippable interval.";
}

bool ReadPlayer(Spot& s) {
    const uint32_t mat = Read32Or0(kPlayerFramePtr);
    if (!IsGuestPtr(mat)) return false;
    s.x = ReadF(mat + 0x30);
    s.y = ReadF(mat + 0x34);
    s.z = ReadF(mat + 0x38);
    s.yaw = std::atan2(ReadF(mat + 0x20), ReadF(mat + 0x28));
    s.scene = CurrentSceneId();
    return true;
}

void WriteYawMatrix(uint32_t m, float yaw) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    const float rows[3][3] = {{c, 0, -s}, {0, 1, 0}, {s, 0, c}};  // right, up, at
    for (uint32_t r = 0; r < 3; ++r)
        for (uint32_t k = 0; k < 3; ++k) WriteF(m + r * 0x10 + k * 4, rows[r][k]);
}

// Player entity frame ([globals+0x238]): +0x00 matrix (pos +0x30), +0x80 previous position, two Y-axis rotations
// (axis +0x98 / angle +0xA4, axis +0xB8 / angle +0xC4) the matrix is rebuilt from. The model instance matrix
// ([[globals+0x224]+0x20]) is what the scene setup also moves at spawn.
bool TeleportInScene(const Spot& s) {
    const uint32_t frame = Read32Or0(kPlayerFramePtr);
    const uint32_t holder = Read32Or0(kPlayerModelPtr);
    const uint32_t model = IsGuestPtr(holder) ? Read32Or0(holder + 0x20) : 0;
    if (!IsGuestPtr(frame) || !IsGuestPtr(model)) return false;
    for (const uint32_t m : {frame, model}) {
        WriteYawMatrix(m, s.yaw);
        WriteF(m + 0x30, s.x);
        WriteF(m + 0x34, s.y);
        WriteF(m + 0x38, s.z);
    }
    WriteF(frame + 0x80, s.x);
    WriteF(frame + 0x84, s.y);
    WriteF(frame + 0x88, s.z);
    WriteF(frame + 0xA4, s.yaw);
    WriteF(frame + 0xC4, s.yaw);
    for (const uint32_t v : {kCameraTarget, kCameraTarget + 0xCu}) {
        WriteF(v, s.x);
        WriteF(v + 4, s.y);
        WriteF(v + 8, s.z);
    }
    return true;
}

bool SwitchScene(uint32_t id, float yawRadians) {
    const uint32_t scene = CurrentScenePtr();
    if (!scene || !InPlay() || Read32Or0(scene + 0x70)) return false;
    for (uint32_t a = kFakePortal; a < kFakePortalAsset + 0x40u; a += 4) Memory::Write32(a, 0);
    Memory::Write32(kFakePortal + 0x10, kFakePortalAsset);
    Memory::Write32(kFakePortalAsset + 0x08, 0);  // camera: none -> the scene's default
    Memory::Write32(kFakePortalAsset + 0x0C, 0);  // spawn marker: none -> the player asset's position
    WriteF(kFakePortalAsset + 0x10, yawRadians * 180.0f / 3.14159265f);
    Memory::Write32(kFakePortalAsset + 0x14, id);
    Memory::Write32(scene + 0x70, kFakePortal);
    return true;
}

// Free camera: RwCameraBeginUpdate (0x80168C78, override at the end of this file) gets the main camera's frame
// replaced by this pose; RenderWare then rebuilds the view matrix and the culling frustum from it. The main camera is
// [globals + 0] (RwCamera: +4 frame, +0x18 beginUpdate); the frame holds the modelling matrix at +0x10 and the world
// matrix (LTM) at +0x50, rows right / up / at / pos 16 bytes apart, right = up x at. Game input is held while it is on.
constexpr uint32_t kMainCameraPtr = 0x80245190u;
struct FreeCam {
    bool on = false;
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0;  // yaw 0 looks along +z, pitch > 0 looks down
};
FreeCam g_cam;
// "camspin DEG": the free camera turns this many degrees per frame (steady motion for testing motion vectors
// and temporal upscalers; 0 stops).
float g_camSpinDegrees = 0.0f;
// "cammove DX DY DZ": the free camera moves this far per frame (world units; steady parallax for testing).
float g_camMove[3] = {0.0f, 0.0f, 0.0f};

uint32_t g_camScene = 0, g_camSceneId = 0;  // the scene the free camera belongs to

void CamRows(const FreeCam& c, float rows[4][3]) {
    const float cp = std::cos(c.pitch), sp = std::sin(c.pitch), cy = std::cos(c.yaw), sy = std::sin(c.yaw);
    const float at[3] = {sy * cp, -sp, cy * cp};
    const float right[3] = {cy, 0.0f, -sy};  // world up x at, normalised (on screen this points left)
    const float up[3] = {at[1] * right[2] - at[2] * right[1], at[2] * right[0] - at[0] * right[2],
                         at[0] * right[1] - at[1] * right[0]};  // at x right
    for (int k = 0; k < 3; ++k) {
        rows[0][k] = right[k];
        rows[1][k] = up[k];
        rows[2][k] = at[k];
    }
    rows[3][0] = c.x;
    rows[3][1] = c.y;
    rows[3][2] = c.z;
}

uint32_t MainCameraFrame() {
    const uint32_t camera = Read32Or0(kMainCameraPtr);
    const uint32_t frame = IsGuestPtr(camera) ? Read32Or0(camera + 4) : 0;
    return IsGuestPtr(frame) ? frame : 0;
}

// Start the free camera where the game camera is. Read the modelling matrix (what the game camera writes), not the
// LTM: right after the hub intro ends on its own the LTM is identity for a few seconds (free camera at the origin).
bool ReadMainCamera(FreeCam& c) {
    const uint32_t frame = MainCameraFrame();
    if (!frame) return false;
    const float ax = ReadF(frame + 0x30), ay = ReadF(frame + 0x34), az = ReadF(frame + 0x38);
    c.x = ReadF(frame + 0x40);
    c.y = ReadF(frame + 0x44);
    c.z = ReadF(frame + 0x48);
    c.yaw = std::atan2(ax, az);
    c.pitch = std::asin(std::clamp(-ay, -1.0f, 1.0f));
    return true;
}

void SetFreecam(bool on) {
    if (on && !g_cam.on && !ReadMainCamera(g_cam)) {
        g_status = "Camera not found.";
        return;
    }
    g_cam.on = on;
    if (on) {
        g_camScene = CurrentScenePtr();
        g_camSceneId = CurrentSceneId();
    }
    g_status = on ? "Free camera on" : "Free camera off";
}

// Spots saved with the free camera on keep its pose.
void AddCam(Spot& s) {
    s.cam = g_cam.on;
    if (s.cam) {
        s.cx = g_cam.x;
        s.cy = g_cam.y;
        s.cz = g_cam.z;
        s.cyaw = g_cam.yaw;
        s.cpitch = g_cam.pitch;
    }
}

void ApplySpotCam(const Spot& s) {
    if (!s.cam) return;
    g_cam = {true, s.cx, s.cy, s.cz, s.cyaw, s.cpitch};
    g_camScene = CurrentScenePtr();
    g_camSceneId = CurrentSceneId();
}

// Pad: left stick move, right stick look, LT/RT down/up, hold A fast / B slow. Keyboard: WASD, Q/E down/up, arrows
// look, Shift fast, Ctrl slow. Not while the menu has focus (its own navigation uses them).
void UpdateFreecam(bool menuFocused) {
    static SDL_Gamepad* pad = nullptr;
    if (!g_cam.on) return;
    // Once per frame (the camera itself is updated several times per frame).
    if (g_camSpinDegrees != 0.0f) g_cam.yaw += g_camSpinDegrees * 3.14159265f / 180.0f;
    g_cam.x += g_camMove[0];
    g_cam.y += g_camMove[1];
    g_cam.z += g_camMove[2];
    if (pad && !SDL_GamepadConnected(pad)) {
        SDL_CloseGamepad(pad);
        pad = nullptr;
    }
    if (!pad) {
        int n = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
            if (n > 0) pad = SDL_OpenGamepad(ids[0]);
            SDL_free(ids);
        }
    }
    if (menuFocused) return;
    const float dt = std::clamp(ImGui::GetIO().DeltaTime, 0.0f, 0.1f);
    auto axis = [&](SDL_GamepadAxis a) {
        if (!pad) return 0.0f;
        const float v = SDL_GetGamepadAxis(pad, a) / 32767.0f;
        return std::fabs(v) < 0.2f ? 0.0f : v;
    };
    auto button = [&](SDL_GamepadButton b) { return pad && SDL_GetGamepadButton(pad, b); };
    const bool* keys = SDL_GetKeyboardState(nullptr);
    auto key = [&](SDL_Scancode k) { return keys && keys[k] ? 1.0f : 0.0f; };
    float fwd = -axis(SDL_GAMEPAD_AXIS_LEFTY) + key(SDL_SCANCODE_W) - key(SDL_SCANCODE_S);
    float side = axis(SDL_GAMEPAD_AXIS_LEFTX) + key(SDL_SCANCODE_D) - key(SDL_SCANCODE_A);
    float lift = axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) - axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) + key(SDL_SCANCODE_E) -
                 key(SDL_SCANCODE_Q);
    // RenderWare's "right" row points to the screen's left (and yaw grows to the left): both are negated below
    const float turn = axis(SDL_GAMEPAD_AXIS_RIGHTX) + key(SDL_SCANCODE_RIGHT) - key(SDL_SCANCODE_LEFT);
    const float tilt = axis(SDL_GAMEPAD_AXIS_RIGHTY) + key(SDL_SCANCODE_DOWN) - key(SDL_SCANCODE_UP);
    float speed = 8.0f;  // units per second (Scooby runs ~6)
    if (button(SDL_GAMEPAD_BUTTON_SOUTH) || key(SDL_SCANCODE_LSHIFT)) speed *= 4.0f;
    if (button(SDL_GAMEPAD_BUTTON_EAST) || key(SDL_SCANCODE_LCTRL)) speed *= 0.2f;
    g_cam.yaw -= turn * 2.0f * dt;
    g_cam.pitch = std::clamp(g_cam.pitch + tilt * 1.5f * dt, -1.55f, 1.55f);
    // level moves: forward/back follow only the yaw (pitch doesn't climb or dive), height only from LT/RT
    const float sy = std::sin(g_cam.yaw), cy = std::cos(g_cam.yaw);
    g_cam.x += (sy * fwd - cy * side) * speed * dt;
    g_cam.y += lift * speed * dt;
    g_cam.z += (cy * fwd + sy * side) * speed * dt;
}

void GoToSpot(const Spot& s) {
    if (g_sceneRequest.Active() || g_pendingSpot) {
        g_status = "A level/spot transfer is already pending. Wait for it or cancel it first.";
        return;
    }
    if (!InPlay() || CutscenePlaying() || !Read32Or0(kPlayerControl)) {
        g_status = "Close the pause menu and skip or finish the cutscene before teleporting to a spot.";
        return;
    }
    if (s.scene == CurrentSceneId()) {
        g_status = TeleportInScene(s) ? "Teleported to " + s.name : "Player not found.";
        ApplySpotCam(s);
        return;
    }
    LoadLevels();
    if (!IsPlayable(s.scene) || !g_sceneRequest.Begin(s.scene, SDL_GetTicks())) {
        g_status = "The spot's destination is not a playable level.";
        return;
    }
    g_pending = s;
    g_pendingSpot = true;
    g_pendingSettle = g_pendingAge = 0;
    g_status = "Loading " + Lower(IdString(s.scene)) + ", then teleporting to " + s.name;
}

void GoToScene(uint32_t id) {
    LoadLevels();
    if (g_sceneRequest.Active() || g_pendingSpot) {
        g_status = "A level/spot transfer is already pending. Wait for it or cancel it first.";
        return;
    }
    if (!IsPlayable(id)) {
        g_status = "Unknown level ID.";
        return;
    }
    if (!IsPlayable(CurrentSceneId())) {
        g_status = "Start or load a game first. Debug travel cannot initialize a game from the title/demo screen.";
        return;
    }
    if (id == CurrentSceneId()) {
        g_status = "Already in " + Lower(IdString(id)) + ".";
        return;
    }
    g_pending = {};
    g_pending.scene = id;
    g_sceneRequest.Begin(id, SDL_GetTicks());
    g_status = "Queued " + Lower(IdString(id)) + ". Close the pause menu or skip/finish the cutscene to travel.";
}

void UpdatePending() {
    if (g_sceneRequest.Active()) {
        const uint32_t scene = CurrentScenePtr();
        const auto action = g_sceneRequest.Update(CurrentSceneId(), InPlay(),
                                                 CutscenePlaying() || !Read32Or0(kPlayerControl),
                                                 scene && Read32Or0(scene + 0x70), SDL_GetTicks());
        if (action == SceneRequest::Action::Submit) {
            SetFreecam(false);
            if (!SwitchScene(g_pending.scene, g_pending.yaw)) {
                g_sceneRequest.Cancel();
                g_pendingSpot = false;
                g_status = "Level switch refused: the game is not ready or another portal is pending.";
            } else {
                g_status = "Loading " + Lower(IdString(g_pending.scene)) + "...";
            }
        } else if (action == SceneRequest::Action::Timeout) {
            g_pendingSpot = false;
            g_status = "Level travel timed out. Close the pause menu or finish the cutscene, then retry.";
        } else if (action == SceneRequest::Action::Arrived) {
            g_status = "Loaded " + Lower(IdString(g_pending.scene)) + ".";
            g_pendingAge = g_pendingSettle = 0;
        }
        if (g_sceneRequest.Active()) return;
    }
    if (!g_pendingSpot) return;
    if (++g_pendingAge > kPendingTimeoutFrames) {
        g_pendingSpot = false;
        g_status = "Gave up waiting for " + Lower(IdString(g_pending.scene)) + ".";
        return;
    }
    if (!InPlay() || CutscenePlaying() || !Read32Or0(kPlayerControl) || CurrentSceneId() != g_pending.scene) {
        g_pendingSettle = 0;
        return;
    }
    if (++g_pendingSettle < kSettleFrames) return;
    g_pendingSpot = false;
    g_status = TeleportInScene(g_pending) ? "Teleported to " + g_pending.name : "Player not found.";
    ApplySpotCam(g_pending);
}

// Scooby's geometry flag and material colour words.
std::vector<std::pair<uint32_t, bool>> PlayerTintWords() {  // address, is the geometry flags word
    std::vector<std::pair<uint32_t, bool>> words;
    for (uint32_t inst = Read32Or0(kPlayerModelPtr), n = 0; IsGuestPtr(inst) && n < 32; inst = Read32Or0(inst), ++n) {
        const uint32_t atomic = Read32Or0(inst + 0x10);
        const uint32_t geom = IsGuestPtr(atomic) ? Read32Or0(atomic + 0x18) : 0;
        if (!IsGuestPtr(geom)) continue;
        words.push_back({geom + 8, true});
        const uint32_t mats = Read32Or0(geom + 0x20), count = Read32Or0(geom + 0x24);
        for (uint32_t m = 0; IsGuestPtr(mats) && m < count && m < 64; ++m)
            if (const uint32_t mat = Read32Or0(mats + m * 4); IsGuestPtr(mat)) words.push_back({mat + 4, false});
    }
    return words;
}

void ApplyTint() {
    for (const auto& [addr, isFlags] : PlayerTintWords()) {
        const uint32_t v = Read32Or0(addr), want = isFlags ? v | 0x40u : kGodTint;
        if (v == want) continue;
        g_tintSaved.emplace(addr, v);
        Memory::Write32(addr, want);
    }
}

// Only words that still belong to Scooby's model: a scene load may have freed the ones tinted before.
void RestoreTint() {
    for (const auto& [addr, isFlags] : PlayerTintWords())
        if (const auto it = g_tintSaved.find(addr); it != g_tintSaved.end()) Memory::Write32(addr, it->second);
    g_tintSaved.clear();
}

void SetGod(bool on) {
    if (on == g_god) return;
    g_god = on;
    if (on) {
        g_takeDamageSaved = Read32Or0(kTakeDamage);
        Memory::Write32(kTakeDamage, 0);
        if (InPlay()) ApplyTint();
    } else {
        Memory::Write32(kTakeDamage, g_takeDamageSaved);
        if (InPlay()) RestoreTint();
        g_tintSaved.clear();
    }
}

// Every frame: sd2.ini may be read again on a load, and a new scene can bring a new player model.
void UpdateGod() {
    if (!g_god) return;
    if (Read32Or0(kTakeDamage)) Memory::Write32(kTakeDamage, 0);
    if (InPlay()) ApplyTint();
}

// Power-ups: one bit each in globals + 0x5B8 (the bit index is the pickup's byte +5; 0x8004F0FC ORs it in, the
// pause-menu cheat in 0x800ABF08 sets all 15). A new game copies sd2.ini's eSPECIAL_* mask (globals + 0x820, read in
// 0x800BE6B8 in this bit order) into it (0x80057748); a scene change keeps it (tested i004 -> i001). Save data: the
// 'PLYR' block reader 0x800CF2B8 reads it back.
constexpr uint32_t kPowerups = 0x80245748u;  // globals + 0x5B8
// GIHE78 rev 1: mnu4.HIP's MAP COLLECTED COND hashes "Map Item Recieved" to 0x8F103F86. The variable table
// at 0x801D7800 resolves it to 0x800D5990, which reads r13-0x72A8. The normal grant helper (0x800D73F8,
// dispatcher event 0x101) only writes 1 here; reset 0x800D7840 writes 0. Mirror those exact writes to toggle,
// without changing Shovel, warp gates, room discovery or any other gadget. 0x800D7720/778C load/save it.
constexpr uint32_t kMapReceived = 0x80270FD8u;

uint32_t Powerups() { return Read32Or0(kPowerups); }
// Only the 15 power-up bits change; any other bit in the word is left alone.
void SetPowerups(uint32_t mask, uint32_t bits) {
    Memory::Write32(kPowerups, WithPowerupBits(Powerups(), mask, bits));
}

uint32_t PowerupAddress(const Powerup& power) { return power.bit ? kPowerups : kMapReceived; }

bool HasPowerup(const Powerup& power) {
    return PowerupEnabled(power, Read32Or0(PowerupAddress(power)));
}

void SetPowerup(const Powerup& power, bool on) {
    const uint32_t address = PowerupAddress(power);
    Memory::Write32(address, WithPowerup(power, Read32Or0(address), on));
}

void SetAllPowerups(bool on) {
    // Unlock the main-game set without granting the unused gadgets. Lock all still clears every gadget.
    SetPowerups(on ? kMainGamePowerups : kAllPowerups, on ? kMainGamePowerups : 0);
    Memory::Write32(kMapReceived, on ? 1u : 0u);
}

std::string PowerupList() {
    const uint32_t v = Powerups();
    char b[32];
    std::snprintf(b, sizeof b, "powerups %04X:", v);
    std::string s = b;
    for (const auto& power : kPowerupList)
        if (HasPowerup(power)) s += std::string(" ") + power.key;
    return s;
}

uint32_t BE32(const std::vector<uint8_t>& d, size_t o) {
    return o + 4 <= d.size() ? (uint32_t{d[o]} << 24) | (uint32_t{d[o + 1]} << 16) | (uint32_t{d[o + 2]} << 8) | d[o + 3] : 0;
}

// HIP: HIPA (no size), PACK, DICT > ATOC > AHDR (id, type, offset, size, plus, flags, ADBG: size +4, name +12).
void WalkHip(const std::vector<uint8_t>& d, size_t off, size_t end, int depth) {
    while (off + 8 <= end && depth < 4) {
        const uint32_t tag = BE32(d, off), size = BE32(d, off + 4);
        const size_t body = off + 8;
        if (tag == IdFromString("HIPA")) {
            off = body;
            continue;
        }
        if (tag == IdFromString("DICT") || tag == IdFromString("ATOC")) {
            WalkHip(d, body, std::min(end, body + size), depth + 1);
        } else if (tag == IdFromString("AHDR") && BE32(d, body + 4) == IdFromString("NPC ") &&
                   BE32(d, body + 24) == IdFromString("ADBG")) {
            const size_t nameEnd = std::min(end, body + 32 + BE32(d, body + 28));
            std::string name;
            for (size_t i = body + 36; i < nameEnd && d[i]; ++i) name.push_back(static_cast<char>(d[i]));
            while (!name.empty() && name.back() == ' ') name.pop_back();
            g_npcNames[BE32(d, body)] = name;
        }
        off = body + size;
    }
}

void LoadNpcNames(uint32_t sceneId) {
    if (sceneId == g_namesSceneId) return;
    g_namesSceneId = sceneId;
    g_npcNames.clear();
    const std::filesystem::path root = RuntimeConfigFile::ResolvedDvdRoot();
    if (root.empty()) return;
    const std::string id = Lower(IdString(sceneId));
    std::ifstream in(root / "files" / (std::string(1, static_cast<char>(std::toupper(id[0]))) + "0") / (id + ".HIP"),
                     std::ios::binary);
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    WalkHip(data, 0, data.size(), 0);
}

// A new scene (or the same one rebuilt) starts with every monster on.
void RefreshMonsters() {
    const uint32_t scene = CurrentScenePtr(), id = CurrentSceneId();
    const uint32_t list = Read32Or0(scene + 0x78), count = Read32Or0(scene + 0x74);
    if (id == g_monSceneId && list == g_monList && count == g_monCount) return;
    g_monSceneId = id;
    g_monList = list;
    g_monCount = count;
    g_monsters.clear();
    LoadNpcNames(id);
    for (uint32_t i = 0; IsGuestPtr(list) && i < count && i < 8000; ++i) {
        const uint32_t base = Read32Or0(list + i * 4);
        if (!IsGuestPtr(base) || Memory::Read8(base + 4) != kNpcBaseType) continue;
        Monster m;
        m.base = base;
        const uint32_t asset = Read32Or0(base + 0x10);
        m.asset = IsGuestPtr(asset) ? Read32Or0(asset) : Read32Or0(base);
        const auto it = g_npcNames.find(m.asset);
        char hex[16];
        std::snprintf(hex, sizeof hex, "%08X", m.asset);
        m.name = it != g_npcNames.end() && !it->second.empty() ? it->second : hex;
        g_monsters.push_back(std::move(m));
    }
}

// Every position the entity has: frame, previous frame position, model instance matrices, bound centre.
std::vector<uint32_t> MonsterPosAddrs(uint32_t base) {
    std::vector<uint32_t> addrs;
    if (const uint32_t frame = Read32Or0(base + 0x40); IsGuestPtr(frame)) {
        addrs.push_back(frame + 0x30);
        addrs.push_back(frame + 0x80);
    }
    for (uint32_t inst = Read32Or0(base + 0x2C), n = 0; IsGuestPtr(inst) && n < 32; inst = Read32Or0(inst), ++n)
        if (const uint32_t mat = Read32Or0(inst + 0x20); IsGuestPtr(mat)) addrs.push_back(mat + 0x30);
    addrs.push_back(base + 0x4C);
    return addrs;
}

// Off: no update, parked at (2000 + 50i, -500, 2000). Repeated every frame in case the game sets them again.
void ParkMonster(Monster& m, size_t index) {
    if (const uint32_t update = Read32Or0(m.base + 0x30)) {
        m.update = update;
        Memory::Write32(m.base + 0x30, 0);
    }
    const float park[3] = {2000.0f + 50.0f * static_cast<float>(index), -500.0f, 2000.0f};
    for (const uint32_t a : MonsterPosAddrs(m.base)) {
        for (uint32_t k = 0; k < 3; ++k) {
            const uint32_t w = a + 4 * k;
            if (std::none_of(m.saved.begin(), m.saved.end(), [w](const auto& s) { return s.first == w; }))
                m.saved.push_back({w, Read32Or0(w)});
            WriteF(w, park[k]);
        }
    }
    m.off = true;
}

void RestoreMonster(Monster& m) {
    if (!m.off) return;
    if (m.update && !Read32Or0(m.base + 0x30)) Memory::Write32(m.base + 0x30, m.update);
    for (const auto& [addr, value] : m.saved) Memory::Write32(addr, value);
    m.saved.clear();
    m.off = false;
}

void SetAllMonstersOff(bool off) {
    g_allMonstersOff = off;
    if (!off)
        for (Monster& m : g_monsters) RestoreMonster(m);
}

void UpdateMonsters() {
    if (!InPlay()) return;
    RefreshMonsters();
    for (size_t i = 0; i < g_monsters.size(); ++i)
        if (g_allMonstersOff || g_monsters[i].off) ParkMonster(g_monsters[i], i);
}

Monster* FindMonster(const std::string& key) {
    for (size_t i = 0; i < g_monsters.size(); ++i)
        if (std::to_string(i) == key || Lower(g_monsters[i].name) == Lower(key)) return &g_monsters[i];
    return nullptr;
}

std::string MonsterList() {
    std::string out = std::to_string(g_monsters.size()) + " monsters" + (g_allMonstersOff ? " (all off)" : "");
    for (size_t i = 0; i < g_monsters.size(); ++i) {
        const Monster& m = g_monsters[i];
        const uint32_t frame = Read32Or0(m.base + 0x40);
        char b[200];
        std::snprintf(b, sizeof b, "\n  %zu %-10s %08X base %08X %s update %08X pos %.2f %.2f %.2f bound %.2f %.2f %.2f", i,
                      m.name.c_str(), m.asset, m.base, m.off ? "OFF" : "on ", Read32Or0(m.base + 0x30),
                      IsGuestPtr(frame) ? ReadF(frame + 0x30) : 0.f, IsGuestPtr(frame) ? ReadF(frame + 0x34) : 0.f,
                      IsGuestPtr(frame) ? ReadF(frame + 0x38) : 0.f, ReadF(m.base + 0x4C), ReadF(m.base + 0x50),
                      ReadF(m.base + 0x54));
        out += b;
    }
    return out;
}

// Suspect HD textures for the Textures tab, from UserData/devmenu_textures.txt (tab-separated: id, replacement file name
// without extension, severity, asset, scenes (space-separated), level). "drawn" = the game has drawn that replacement in
// this level (see UpdateDrawn).
struct Suspect {
    std::string id, key, severity, asset, level;
    std::vector<uint32_t> scenes;
    bool effect = false;  // 7th column "effect": drawn at runtime (kills, bonks, pickups...): Effects tab
};
std::vector<Suspect> g_suspects;
bool g_suspectsLoaded = false;
bool g_suspectsThisScene[2] = {true, true};  // Textures, Effects
std::string g_findId;  // suspect drawn solid magenta ("Find"), empty = none

// Draws the suspect's replacement solid magenta (needs texture replacements on); nullptr = off.
void SetFind(const Suspect* s) {
    g_findId = s ? s->id : "";
    aurora_texture_replacement_highlight(s ? s->key.c_str() : nullptr);
    g_status = s ? "Finding " + s->id + " (magenta)" : "Find off";
}

// Verdicts per scene, UserData/devmenu_verdicts.txt (tab-separated: id, scene, status; scene "*" = every scene). The
// verdict button sets fine/bugged/fixed for the current scene (a texture can be fine in one level and broken in
// another); tools/debug/texhunt/verdicts.py merges them into suspects.csv and adds "fixed" for textures fixed in the pack.
std::map<std::pair<std::string, std::string>, std::string> g_verdicts;  // (id, scene) -> status
bool g_hideFine[2] = {true, true};  // Textures, Effects

std::filesystem::path VerdictsPath() { return RuntimeConfigFile::ApplicationDataDirectory() / "devmenu_verdicts.txt"; }

std::string Verdict(const std::string& id, uint32_t scene) {
    auto it = g_verdicts.find({id, Lower(IdString(scene))});
    if (it == g_verdicts.end()) it = g_verdicts.find({id, "*"});
    return it == g_verdicts.end() ? "" : it->second;
}

void SetVerdict(const std::string& id, uint32_t scene, const std::string& status) {
    const std::string sceneName = Lower(IdString(scene));
    if (status.empty()) {
        g_verdicts.erase({id, sceneName});
    } else {
        g_verdicts[{id, sceneName}] = status;
    }
    std::ofstream out(VerdictsPath(), std::ios::trunc);
    out << "# id\tscene\tstatus  (dev menu Textures tab; tools/debug/texhunt/verdicts.py)\n";
    for (const auto& [key, vstatus] : g_verdicts) out << key.first << '\t' << key.second << '\t' << vstatus << '\n';
    g_status = id + (status.empty() ? " unmarked" : " marked " + status) + " in " + sceneName;
}

// Drawn per level, UserData/devmenu_drawn.txt (tab-separated: id, scene), kept across sessions. aurora notes each
// replacement on upload; on entering a level the menu clears that and drops the texture caches, so what gets noted after
// that was drawn in this level.
std::set<std::pair<std::string, std::string>> g_drawn;  // (id, scene)
uint32_t g_drawnScene = 0;

std::filesystem::path DrawnPath() { return RuntimeConfigFile::ApplicationDataDirectory() / "devmenu_drawn.txt"; }

bool Drawn(const std::string& id, uint32_t scene) { return g_drawn.count({id, Lower(IdString(scene))}) > 0; }

std::string DrawnScenes(const std::string& id) {
    std::string out;
    for (const auto& [did, scene] : g_drawn)
        if (did == id) out += (out.empty() ? "" : " ") + scene;
    return out;
}

void LoadSuspects() {
    if (g_suspectsLoaded) return;
    g_suspectsLoaded = true;
    std::ifstream drawn(DrawnPath());
    for (std::string line; std::getline(drawn, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t tab = line.find('\t');
        if (line.empty() || line[0] == '#' || tab == std::string::npos) continue;
        g_drawn.insert({line.substr(0, tab), line.substr(tab + 1)});
    }
    std::ifstream verdicts(VerdictsPath());
    for (std::string line; std::getline(verdicts, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        std::vector<std::string> f;
        std::istringstream ss(line);
        for (std::string field; std::getline(ss, field, '\t');) f.push_back(field);
        if (f.size() < 3 || f[0].empty() || f[0][0] == '#') continue;
        g_verdicts[{f[0], f[1]}] = f[2];
    }
    std::ifstream in(RuntimeConfigFile::ApplicationDataDirectory() / "devmenu_textures.txt");
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> f;
        std::istringstream ss(line);
        for (std::string field; std::getline(ss, field, '\t');) f.push_back(field);
        if (f.size() < 6 || f[0].empty() || f[0][0] == '#') continue;
        Suspect s{f[0], f[1], f[2], f[3], f[5], {}};
        std::istringstream sc(f[4]);
        for (std::string id; sc >> id;) s.scenes.push_back(IdFromString(id));
        s.effect = f.size() > 6 && IsEffectEntry(f[6]);
        g_suspects.push_back(std::move(s));
    }
}

void UpdateDrawn() {
    static uint32_t tick = 0;
    if (!InPlay()) return;
    const uint32_t scene = CurrentSceneId();
    if (scene != g_drawnScene) {  // new level: forget the last one's textures
        g_drawnScene = scene;
        aurora_texture_replacement_reset_used();
        return;
    }
    if (++tick % 30 != 0) return;
    LoadSuspects();
    const std::string sceneName = Lower(IdString(scene));
    bool added = false;
    for (const Suspect& s : g_suspects)
        if (aurora_texture_replacement_used(s.key.c_str())) added |= g_drawn.insert({s.id, sceneName}).second;
    if (!added) return;
    std::ofstream out(DrawnPath(), std::ios::trunc);
    out << "# id\tscene  (dev menu: suspect texture drawn in that level)\n";
    for (const auto& [id, dscene] : g_drawn) out << id << '\t' << dscene << '\n';
}

// Spots are per scene: a name can be saved once in each scene. This scene's spot with that name, else the first one.
const Spot* FindSpot(const std::string& name, uint32_t scene) {
    const Spot* any = nullptr;
    for (const Spot& s : g_spots) {
        if (s.name != name) continue;
        if (s.scene == scene) return &s;
        if (!any) any = &s;
    }
    return any;
}

// devmenu.txt (cwd = Play), read every 30 frames and then deleted; results go to devmenu.log. The file waits (is
// left alone) until the game is in play with no scene/spot transfer pending, unless every line is "skip" or, with no
// transfer pending, "scene".
std::string ReadinessText() {
    ObserveGallery();
    const ProgressAvailability state = ProgressState();
    const auto flag = [](uint32_t address) -> std::string {
        uint32_t value;
        if (!ProgressRead(address, value)) return "unavailable";
        return value ? "yes" : "no";
    };
    const bool layout = no100f_game_state::g_diagnosticLayoutVerified.load();
    const auto guestFlag = [&](uint32_t address) { return layout ? flag(address) : "unsupported"; };
    const auto guestWord = [&](uint32_t address) -> std::string {
        uint32_t value;
        if (!layout) return "unsupported";
        return ProgressRead(address, value) ? std::to_string(value) : "unavailable";
    };
    PlayerStats player;
    const char* playerReason = PlayerReason(player);
    return std::string("interaction readiness: ") + (state.Reason() ? state.Reason() : "ready") +
           "\n  mode=" + guestWord(kGameMode) + " state=" + guestWord(kGameState) +
           "\n  cutscene=" + guestFlag(kCutsceneMgr) + " movie=" + guestFlag(no100f_game_state::kMovieAddress) +
           " control=" + guestFlag(kPlayerControl) +
           "\n  portal=" + (CurrentScenePtr() ? guestFlag(CurrentScenePtr() + 0x70) : "unavailable") +
           " travel=" + (g_sceneRequest.Active() ? "pending" : "none") +
           " spot=" + (g_pendingSpot ? "pending" : "none") +
           " freecam=" + (g_cam.on ? "on" : "off") + " input_capture=" + (BlocksInput() ? "yes" : "no") +
           "\n  pictures=" + RuleName(g_galleryRules.Get(GallerySection::Pictures)) +
           " movies=" + RuleName(g_galleryRules.Get(GallerySection::Movies)) +
           "\n  player_edits=" + (playerReason ? playerReason : "ready");
}

std::string ProgressReceipt() {
    std::ostringstream result;
    result << "progress receipt build=" << BuildInfo::Id() << "\n" << WhereText();
    result << "\nprofile_data=" << RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ApplicationDataDirectory())
           << "\nprofile_saves=" << RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::SavesDirectory());
    if (no100f_game_state::g_diagnosticLayoutVerified.load()) {
        uint32_t map, powers;
        if (ProgressRead(kMapReceived, map) && ProgressRead(kPowerups, powers))
            result << "\nMap=" << map << " gadgets=0x" << std::hex << powers << std::dec;
        else
            result << "\nMap/gadgets unavailable: unreadable state";
    } else {
        result << "\nMap/gadgets unavailable: unsupported layout";
    }
    result << "\n" << PlayerReadout() << "\n" << WarpReadout() << "\n" << GalleryReadout() << "\n" << ReadinessText();
    return result.str();
}

void DrawWarps() {
    ImGui::TextWrapped("One-time live discovery edits. Approaching a gate can unlock it again. Normal saves can retain changes.");
    if (!no100f_game_state::g_diagnosticLayoutVerified.load()) {
        ImGui::TextDisabled("Unavailable: unsupported executable layout.");
        return;
    }
    std::array<uint32_t, 26> flags{};
    if (!ReadWarps(ProgressRead, flags)) {
        ImGui::TextWrapped("Unavailable: unreadable/modified table or non-boolean flag.");
        return;
    }
    const char* reason = ProgressState().Reason();
    if (reason) ImGui::TextWrapped("Edits unavailable: %s", reason);
    uint32_t discovered = 0;
    for (uint32_t flag : flags) discovered += flag;
    ImGui::Text("Discovered %u / 26", discovered);
    for (size_t i = 0; i < flags.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        ImGui::TextWrapped("%s%s", SceneName(kWarpScenes[i]).c_str(),
                           CurrentSceneId() == kWarpScenes[i] ? " (current scene)" : "");
        ImGui::Text("Index %zu: discovery %u", i, flags[i]);
        ImGui::SameLine();
        ImGui::BeginDisabled(reason != nullptr);
        if (ImGui::Button(flags[i] ? "Disable" : "Enable")) {
            g_status = ChangeWarp(i, flags[i] == 0);
            ProgressLog("warp " + Lower(IdString(kWarpScenes[i])), g_status);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        ImGui::Separator();
    }
}

void DrawGallery() {
    ImGui::TextWrapped("H003-only access tests, not progression edits. Overrides clear when leaving the gallery. Normal movie playback can still update watched state.");
    const GallerySnapshot snapshot = ReadGallery();
    if (snapshot.reason) ImGui::TextWrapped("%s", snapshot.reason);
    const char* reason = ProgressState().Reason();
    if (reason) ImGui::TextWrapped("Edits unavailable: %s", reason);
    for (GallerySection section : {GallerySection::Pictures, GallerySection::Movies}) {
        const bool available = section == GallerySection::Pictures ? snapshot.pictures : snapshot.movies;
        const char* label = section == GallerySection::Pictures ? "Picture Gallery" : "Movie Gallery";
        ImGui::PushID(label);
        ImGui::Text("%s: %s", label, RuleName(g_galleryRules.Get(section)));
        int selected = static_cast<int>(g_galleryRules.Get(section));
        ImGui::BeginDisabled(!available || reason != nullptr);
        if (ImGui::Combo("Test access", &selected, "Game rules\0Force locked\0Force unlocked\0")) {
            g_status = ChangeGallery(section, static_cast<GalleryRule>(selected));
            ProgressLog(std::string("gallery ") + label, g_status);
        }
        ImGui::EndDisabled();
        if (available && !reason && ImGui::TreeNode("Natural availability (read-only)")) {
            if (section == GallerySection::Pictures) {
                CpuContext call = *TryGetCpuContext();
                CpuContextScope scope(&call);
                func_800D5150(&call);
                ImGui::Text("Snack-complete areas: %u / 30", static_cast<uint32_t>(call.gpr[3]));
            }
            for (size_t i = 0; i < snapshot.bases.size(); ++i) {
                const auto spec = ConditionAt(i);
                if (spec.section != section || (section == GallerySection::Movies && !spec.visibility)) continue;
                const unsigned number = static_cast<unsigned>(section == GallerySection::Pictures ? i + 1 :
                                                              (i - kPictureEntries.size()) / 2 + 1);
                const bool natural = NaturalCondition(snapshot.bases[i]) != 0;
                const GalleryRule rule = g_galleryRules.Get(section);
                const bool effective = rule == GalleryRule::Natural ? natural : rule == GalleryRule::Unlocked;
                ImGui::Text("%s %u: game %s, effective %s", section == GallerySection::Pictures ? "Picture" : "Movie",
                            number, natural ? "open" : "locked", effective ? "open" : "locked");
                if (section == GallerySection::Pictures) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(requires %u complete areas)", spec.threshold);
                }
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::BeginDisabled(reason != nullptr || (!snapshot.pictures && !snapshot.movies));
    if (ImGui::Button("Clear gallery overrides")) {
        g_galleryRules.Clear();
        g_status = "Gallery overrides cleared. Reopen/browse the gallery to refresh.";
        ProgressLog("gallery clear", g_status);
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Visibility refreshes through normal gallery entry/browsing. Close/reopen the gallery after changes.");
}

void DrawDiagnostics() {
    ImGui::TextWrapped("%s", ReadinessText().c_str());
    if (ImGui::Button("Log progression receipt")) {
        g_status = ProgressReceipt();
        ProgressLog("receipt", g_status);
    }
    ImGui::TextDisabled("Read-only; local devmenu.log only. No import/restore.");
}

//   scene ID            go to scene ID (start position; queued until the game is in play with no cutscene)
//   skip                skip the recorded cutscene / title intro / attract demo with the game's own button
//   spot NAME           go to a saved spot
//   save NAME           save the current spot
//   tp X Y Z [YAWDEG]   teleport in this scene
//   where               log the scene and Scooby's position
//   peek ADDR [N]       log N (default 64) words from ADDR
//   frame               peek the player frame
//   god on|off          god mode
//   powerups [all|none|HEX]  log powers; unlock main-game set / lock everything / set gadget mask (Map unchanged)
//   powerup NAME on|off unlock / lock one (Map or sd2.ini name without eSPECIAL_, e.g. Spring)
//   player / health 1..5 / snacks 0..99999  read / one-time player-stat edits
//   snackareas / snackarea INDEX COUNT / snackconfirm / snackcancel  separate confirmed area-counter edits
//   warps / warp SCENE on|off    read / change one live discovery flag (normal saves can retain it)
//   gallery [pictures|movies natural|locked|unlocked]  H003-only access tests, not progression edits
//   receipt / readiness         read-only progression receipt / interaction blockers
//   monsters [on|off]   list this scene's monsters; all monsters off (every scene) / back on
//   monster N|NAME on|off
//   ents                log the scene's entity list
//   textures            log which suspect textures (devmenu_textures.txt) have been drawn in this level
//   find ID|off         draw that suspect's texture solid magenta / back to normal
//   freecam on|off      free camera (starts at the game camera)
//   camspin DEG         free camera turns DEG degrees every frame (0 stops; turns the free camera on)
//   cammove DX DY DZ    free camera moves this far every frame (world units; 0 0 0 stops)
//   shot FILE           screenshot of the next presented frame (as F2, without the clipboard/notice)
//   mvview MODE [SCALE] motion vectors view (AURORA_MOTION_VECTORS=1): 0 off, 1 motion only, 2 over the picture;
//                       SCALE = pixels per frame for full color
//   cam X Y Z YAW PITCH free camera at this pose (degrees; pitch > 0 looks down)
//   cutscene            "cutscene yes|no control yes|no": recorded cutscene playing (devmenu_press.txt skips it), player
//                       has control (no during cutscenes and in-world camera pans)
//   perf                draw calls of the last frame + present timing (fps, average/p95 frame time)
void PollCommandFile() {
    static uint32_t tick = 0;
    if (++tick % 30 != 0) return;
    std::ifstream in("devmenu.txt");
    if (!in) return;
    std::vector<std::string> lines;
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    in.close();
    const bool busy = g_pendingSpot || g_sceneRequest.Active();
    if (!InPlay() || busy) {
        for (const std::string& l : lines) {
            std::istringstream ss(l);
            std::string cmd;
            if (!(ss >> cmd)) continue;
            const bool diagnostic = cmd == "warps" || cmd == "warp" || cmd == "gallery" ||
                                    cmd == "receipt" || cmd == "readiness" || IsPlayerCommand(cmd);
            if (cmd != "skip" && !(cmd == "scene" && !busy) && !diagnostic) return;  // wait for play
        }
    }
    std::remove("devmenu.txt");
    std::ofstream log("devmenu.log", std::ios::app);
    for (const std::string& l : lines) {
        std::istringstream ss(l);
        std::string cmd, arg;
        ss >> cmd;
        std::getline(ss >> std::ws, arg);
        g_status.clear();
        if (HandleProgressCommand(cmd, arg) || HandlePlayerCommand(cmd, arg)) {
        } else if (cmd == "receipt" || cmd == "readiness") {
            g_status = !arg.empty() ? "receipt/readiness take no arguments." :
                       cmd == "receipt" ? ProgressReceipt() : ReadinessText();
        } else if (cmd == "skip") {
            RequestSkip();
        } else if (cmd == "scene") {
            GoToScene(IdFromString(arg));
        } else if (cmd == "spot") {
            LoadSpots();
            g_status = "No spot named " + arg;
            if (const Spot* s = FindSpot(arg, CurrentSceneId())) GoToSpot(*s);
        } else if (cmd == "save") {
            LoadSpots();
            Spot s;
            if (ReadPlayer(s) && s.scene) {
                s.name = arg;
                AddCam(s);
                g_spots.push_back(s);
                SaveSpots();
                g_status = "Saved " + arg;
            }
        } else if (cmd == "where") {
            g_status = WhereText();
        } else if (cmd == "peek" || cmd == "frame") {
            uint32_t addr = 0, n = 64;
            if (cmd == "frame") {
                addr = Read32Or0(kPlayerFramePtr);
            } else {
                std::sscanf(arg.c_str(), "%x %u", &addr, &n);
            }
            char b[64];
            std::snprintf(b, sizeof b, "%08X:", addr);
            g_status = b;
            for (uint32_t i = 0; IsGuestPtr(addr) && i < n; ++i) {
                const uint32_t w = Read32Or0(addr + i * 4);
                std::snprintf(b, sizeof b, "%s+%02X %08X %g", i % 4 ? "  " : "\n  ", i * 4, w, ReadF(addr + i * 4));
                g_status += b;
            }
        } else if (cmd == "god") {
            SetGod(arg == "on");
            char b[64];
            std::snprintf(b, sizeof b, "god %s, TakeDamage %u, %zu words tinted", g_god ? "on" : "off",
                          Read32Or0(kTakeDamage), g_tintSaved.size());
            g_status = b;
        } else if (cmd == "powerups") {
            if (arg == "all") {
                SetAllPowerups(true);
            } else if (arg == "none") {
                SetAllPowerups(false);
            } else if (!arg.empty()) {
                SetPowerups(kAllPowerups, static_cast<uint32_t>(std::strtoul(arg.c_str(), nullptr, 16)));
            }
            g_status = PowerupList();
        } else if (cmd == "powerup") {
            std::istringstream as(arg);
            std::string name, state;
            as >> name >> state;
            const auto it = std::find_if(std::begin(kPowerupList), std::end(kPowerupList),
                                         [&](const Powerup& p) { return Lower(p.key) == Lower(name); });
            if (it == std::end(kPowerupList) || (state != "on" && state != "off")) {
                g_status = "powerup NAME on|off (" + arg + ")";
            } else {
                SetPowerup(*it, state == "on");
                g_status = PowerupList();
            }
        } else if (cmd == "monsters") {
            if (arg == "on" || arg == "off") SetAllMonstersOff(arg == "off");
            UpdateMonsters();
            g_status = MonsterList();
        } else if (cmd == "monster") {
            const size_t sp = arg.find_last_of(' ');
            Monster* m = sp == std::string::npos ? nullptr : FindMonster(arg.substr(0, sp));
            if (!m) {
                g_status = "No monster " + arg;
            } else {
                if (arg.substr(sp + 1) == "off") {
                    m->off = true;
                } else {
                    RestoreMonster(*m);
                }
                UpdateMonsters();
                g_status = MonsterList();
            }
        } else if (cmd == "textures") {
            LoadSuspects();
            g_status = "drawn suspects:";
            for (const Suspect& s : g_suspects)
                if (Drawn(s.id, CurrentSceneId())) g_status += " " + s.id;
        } else if (cmd == "cutout") {
#ifdef AURORA_HAS_CUTOUT_MODE
            if (!arg.empty()) aurora_set_cutout_mode(std::atoi(arg.c_str()));
            g_status = "cutout mode " + std::to_string(aurora_get_cutout_mode());
#else
            g_status = "cutout: not in this build";
#endif
        } else if (cmd == "cutscene") {
            g_status = std::string(CutscenePlaying() ? "cutscene yes" : "cutscene no") +
                       (Read32Or0(kPlayerControl) == 1 ? " control yes" : " control no");
        } else if (cmd == "perf") {
            AuroraPresentTiming t{};
            aurora_get_present_timing(&t);
            const AuroraStats* st = aurora_get_stats();
            char b[192];
            std::snprintf(b, sizeof b, "perf draws %u merged %u fps %.1f avg %.2f ms p95 %.2f ms jitter %.2f ms",
                          st ? st->drawCallCount : 0u, st ? st->mergedDrawCallCount : 0u, t.framesPerSecond,
                          t.averageFrameTimeMs, t.p95FrameTimeMs, t.jitterMs);
            g_status = b;
#ifdef AURORA_HAS_MOTION_VECTORS
        } else if (cmd == "mvview") {
            std::istringstream as(arg);
            int mode = 0;
            float scale = 0.0f;
            if (as >> mode) {
                as >> scale;
                aurora_set_motion_vector_view(mode, scale);
            }
            g_status = std::string("motion vectors ") + (aurora_motion_vectors_active() ? "on" : "off (AURORA_MOTION_VECTORS=1)") +
                       ", view " + std::to_string(aurora_get_motion_vector_view());
#endif
        } else if (cmd == "camspin") {
            g_camSpinDegrees = arg.empty() ? 0.0f : static_cast<float>(std::atof(arg.c_str()));
            if (g_camSpinDegrees != 0.0f && !g_cam.on) SetFreecam(true);
            g_status = "camera spin " + std::to_string(g_camSpinDegrees) + " degrees per frame" +
                       (g_cam.on ? "" : " (no free camera here)");
        } else if (cmd == "cammove") {
            std::istringstream as(arg);
            float dx = 0.0f, dy = 0.0f, dz = 0.0f;
            as >> dx >> dy >> dz;
            g_camMove[0] = dx;
            g_camMove[1] = dy;
            g_camMove[2] = dz;
            if ((dx != 0.0f || dy != 0.0f || dz != 0.0f) && !g_cam.on) SetFreecam(true);
            g_status = "camera move " + arg + " per frame";
        } else if (cmd == "shot") {
            if (arg.empty()) {
                g_status = "shot FILE (.png or .bmp; relative to the working folder)";
            } else {
                aurora_request_screenshot(arg.c_str());
                g_status = "Screenshot requested: " + arg;
            }
        } else if (cmd == "freecam") {
            SetFreecam(arg == "on");
        } else if (cmd == "cam") {
            FreeCam c;
            std::istringstream as(arg);
            if (as >> c.x >> c.y >> c.z >> c.yaw >> c.pitch) {
                c.on = true;
                c.yaw *= 3.14159265f / 180.0f;
                c.pitch *= 3.14159265f / 180.0f;
                g_cam = c;
                // The pose belongs to this scene (as with "freecam on"): ApplyFreecam turns it off elsewhere.
                g_camScene = CurrentScenePtr();
                g_camSceneId = CurrentSceneId();
                g_status = "Free camera at " + arg;
            } else {
                g_status = "cam X Y Z YAW PITCH";
            }
        } else if (cmd == "find") {
            LoadSuspects();
            const auto it = std::find_if(g_suspects.begin(), g_suspects.end(), [&](const Suspect& s) { return s.id == arg; });
            if (arg == "off") {
                SetFind(nullptr);
            } else if (it != g_suspects.end()) {
                SetFind(&*it);
            } else {
                g_status = "No suspect " + arg;
            }
        } else if (cmd == "ents") {
            const uint32_t scene = CurrentScenePtr();
            const uint32_t list = scene ? Read32Or0(scene + 0x78) : 0, count = scene ? Read32Or0(scene + 0x74) : 0;
            char b[160];
            std::snprintf(b, sizeof b, "%u bases at %08X; typed +1A8 %08X x%u, +1AC %08X x%u", count, list,
                          Read32Or0(scene + 0x1A8), Read32Or0(scene + 0xFC), Read32Or0(scene + 0x1AC), Read32Or0(scene + 0x100));
            g_status = b;
            for (uint32_t i = 0; IsGuestPtr(list) && i < count && i < 4000; ++i) {
                const uint32_t base = Read32Or0(list + i * 4);
                const uint32_t asset = Read32Or0(base + 0x10), frame = Read32Or0(base + 0x40);
                std::snprintf(b, sizeof b, "\n  %4u %08X id %08X type %02X flags %04X asset %08X frame %08X pos %.2f %.2f %.2f", i, base,
                              Read32Or0(base), Read32Or0(base + 4) >> 24, (Read32Or0(base + 4) >> 0) & 0xFFFF, asset, frame,
                              IsGuestPtr(frame) ? ReadF(frame + 0x30) : 0.f, IsGuestPtr(frame) ? ReadF(frame + 0x34) : 0.f,
                              IsGuestPtr(frame) ? ReadF(frame + 0x38) : 0.f);
                g_status += b;
            }
        } else if (cmd == "tp") {
            Spot s;
            float yawDeg = 0;
            std::istringstream as(arg);
            if (ReadPlayer(s) && (as >> s.x >> s.y >> s.z)) {
                if (as >> yawDeg) s.yaw = yawDeg * 3.14159265f / 180.0f;
                s.name = "tp";
                GoToSpot(s);
            }
        }
        log << l << " -> " << g_status << "\n";
    }
}

void DrawStatus() {
    const uint32_t id = CurrentSceneId();
    ImGui::Text("Level: %s", id ? SceneName(id).c_str() : "-");
    Spot here;
    if (ReadPlayer(here))
        ImGui::Text("Scooby: %.2f %.2f %.2f  yaw %.1f deg", here.x, here.y, here.z, here.yaw * 180.0f / 3.14159265f);
    ImGui::Text("Game mode %X state %u%s", Read32Or0(kGameMode), Read32Or0(kGameState), InPlay() ? " (in play)" : "");
    ImGui::BeginDisabled(!SkippableSequence());
    const char* skipLabel = CutscenePlaying() ? "Skip cutscene (A)"
                            : Read32Or0(kGameState) == 1 ? "Exit attract demo (Start)"
                                                         : "Skip title intro (Start)";
    if (ImGui::Button(skipLabel)) RequestSkip();
    ImGui::EndDisabled();
    if (g_skipRequest.Active()) ImGui::TextDisabled("Waiting for the game's skip window...");
    if (g_sceneRequest.Active() || g_pendingSpot) {
        ImGui::SameLine();
        ImGui::BeginDisabled(g_sceneRequest.Submitted());
        if (ImGui::Button("Cancel travel")) {
            g_sceneRequest.Cancel();
            g_pendingSpot = false;
            g_status = "Pending debug travel canceled.";
        }
        ImGui::EndDisabled();
        if (g_sceneRequest.Submitted()) ImGui::TextDisabled("A load already started; let it finish normally.");
    }
    bool god = g_god;
    if (OverlayTheme::Toggle("God mode (no damage, green Scooby)", &god)) SetGod(god);
    bool cam = g_cam.on;
    if (OverlayTheme::Toggle("Free camera", &cam)) SetFreecam(cam);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pad: left stick move, right stick look, LT/RT down/up, hold A fast, B slow.\n"
                          "Keys: WASD, Q/E down/up, arrows look, Shift fast, Ctrl slow.\n"
                          "Scooby stays put. Spots saved now keep the camera.");
    if (g_cam.on) {
        ImGui::SameLine();
        ImGui::TextDisabled("%.1f %.1f %.1f  yaw %.0f pitch %.0f", g_cam.x, g_cam.y, g_cam.z,
                            g_cam.yaw * 180.0f / 3.14159265f, g_cam.pitch * 180.0f / 3.14159265f);
    }
#ifdef AURORA_HAS_MOTION_VECTORS
    if (aurora_motion_vectors_active()) {
        // Motion vectors are on (AURORA_MOTION_VECTORS=1): show them live instead of the picture.
        static const char* const kViews[] = {"Off", "Motion only", "Over the picture"};
        int view = aurora_get_motion_vector_view();
        float scale = aurora_get_motion_vector_view_scale();
        ImGui::SetNextItemWidth(OverlayTheme::Px(160.0f));
        if (ImGui::Combo("Motion vectors view", &view, kViews, 3)) aurora_set_motion_vector_view(view, 0.0f);
        if (view != 0) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(OverlayTheme::Px(120.0f));
            if (ImGui::SliderFloat("px for full color", &scale, 1.0f, 64.0f, "%.0f")) aurora_set_motion_vector_view(view, scale);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Grey (or the picture) = still; hue = direction the surface came from, saturation = speed.\n"
                              "Red: from the right, green-yellow: from below, cyan: from the left, blue-purple: from above.");
    }
#endif
    bool recordAudio = RuntimeConfigFile::RecordAudio();
    if (OverlayTheme::Toggle("Record audio (F3 videos)", &recordAudio)) RuntimeConfigFile::SetRecordAudio(recordAudio);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("F3 starts / stops a video of the game (Videos folder, needs ffmpeg).\n"
                          "On: the video has the game's sound (before the volume and mute settings).\n"
                          "Applies to the next recording.");
    bool captureOverlay = RuntimeConfigFile::CaptureOverlay();
    if (OverlayTheme::Toggle("Menus in screenshots and videos", &captureOverlay))
        RuntimeConfigFile::SetCaptureOverlay(captureOverlay);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("On: F2 screenshots and F3 videos show this menu, the F10 board and notices.\n"
                          "Off: only the DEV stamp with the build. The REC light is never recorded.");
    if (!g_status.empty()) {
        const float maxHeight = ImGui::GetTextLineHeightWithSpacing() * 4.0f;
        const float wrapWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x);
        if (ImGui::CalcTextSize(g_status.c_str(), nullptr, false, wrapWidth).y > maxHeight) {
            ImGui::TextDisabled("Command output (scroll for full readout)");
            if (ImGui::BeginChild("##command-output", ImVec2(0.0f, maxHeight),
                                  ImGuiChildFlags_AlwaysUseWindowPadding))
                ImGui::TextWrapped("%s", g_status.c_str());
            ImGui::EndChild();
        } else {
            ImGui::TextWrapped("%s", g_status.c_str());
        }
    }
}

void DrawSpots() {
    LoadSpots();
    ImGui::SetNextItemWidth(OverlayTheme::Px(220.0f));
    ImGui::InputTextWithHint("##spotname", "spot name", g_newSpotName, sizeof g_newSpotName);
    ImGui::SameLine();
    if (ImGui::Button("Save current spot")) {
        Spot s;
        if (ReadPlayer(s) && s.scene) {
            s.name = g_newSpotName[0] ? g_newSpotName : Lower(IdString(s.scene)) + " spot " + std::to_string(g_spots.size() + 1);
            AddCam(s);
            g_spots.push_back(s);
            SaveSpots();
            g_newSpotName[0] = 0;
            g_status = "Saved " + s.name;
        } else {
            g_status = "Player not found.";
        }
    }
    OverlayTheme::Toggle("This level only", &g_thisSceneOnly);
    const uint32_t cur = CurrentSceneId();
    int erase = -1;
    if (ImGui::BeginTable("spots", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < static_cast<int>(g_spots.size()); ++i) {
            const Spot& s = g_spots[i];
            if (g_thisSceneOnly && s.scene != cur) continue;
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Go")) GoToSpot(s);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(Lower(IdString(s.scene)).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.name.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Delete")) erase = i;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (erase >= 0) {
        g_spots.erase(g_spots.begin() + erase);
        SaveSpots();
    }
}

void DrawScenes() {
    LoadLevels();
    ImGui::TextDisabled("Spawns at the level's start position.");
    ImGui::TextWrapped("One transfer at a time. Paused/cutscene requests wait for normal play; travel turns free camera off.");
    const uint32_t cur = CurrentSceneId();
    for (const Level& level : g_levels) {
        if (!ImGui::TreeNode(level.title.c_str())) continue;
        for (const Scene& scene : level.scenes) {
            ImGui::PushID(static_cast<int>(scene.id));
            if (ImGui::Selectable(scene.label.c_str(), scene.id == cur)) GoToScene(scene.id);
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
}

void DrawMonsters() {
    bool allOff = g_allMonstersOff;
    if (OverlayTheme::Toggle("All monsters off (every level)", &allOff)) SetAllMonstersOff(allOff);
    ImGui::TextDisabled("Off = frozen and parked off-map. Per-monster switches reset on a level change.");
    if (!InPlay()) {
        ImGui::TextDisabled("Not in play.");
        return;
    }
    if (g_monsters.empty()) ImGui::TextDisabled("No monsters in this level.");
    ImGui::BeginDisabled(g_allMonstersOff);
    if (ImGui::BeginTable("monsters", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
        for (size_t i = 0; i < g_monsters.size(); ++i) {
            Monster& m = g_monsters[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = !m.off;
            if (OverlayTheme::Toggle(m.name.c_str(), &on)) {
                if (on) {
                    RestoreMonster(m);
                } else {
                    ParkMonster(m, i);
                }
            }
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%08X", m.asset);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
}

void DrawPowerups() {
    if (ImGui::Button("Unlock all")) SetAllPowerups(true);
    ImGui::SameLine();
    if (ImGui::Button("Lock all")) SetAllPowerups(false);
    ImGui::SameLine();
    ImGui::TextDisabled("mask %04X", Powerups() & kAllPowerups);
    ImGui::TextDisabled("Unlock all leaves unused powers unchanged.\nLock all clears them too.");
    ImGui::TextWrapped("Changes take effect at once and stay through level changes. A new game or a loaded save "
                       "replaces them.");
    if (Read32Or0(kGameMode) != 0xB) ImGui::TextDisabled("Not in a game: starting one resets these.");
    bool unusedHeading = false;
    for (const auto& power : kPowerupList) {
        if (power.unused && !unusedHeading) {
            ImGui::Separator();
            ImGui::TextUnformatted(kUnusedPowerupHeading);
            unusedHeading = true;
        }
        ImGui::PushID(power.key);
        bool on = HasPowerup(power);
        if (OverlayTheme::Toggle(power.label, &on)) SetPowerup(power, on);
        ImGui::PopID();
    }
}

void DrawPlayer() {
    PlayerStats snapshot;
    bool readable;
    const char* reason = PlayerReason(snapshot, &readable);
    if (!reason && !g_playerInputsReady) {
        g_healthInput = static_cast<int>(snapshot.health);
        g_snacksInput = snapshot.snacks <= kDiagnosticSnackLimit ? static_cast<int>(snapshot.snacks) : 0;
        SnackAreas areas{};
        g_snackAreaInput = g_snackConfirmation.active ? static_cast<int>(g_snackConfirmation.value) :
                          ReadSnackAreas(ProgressRead, areas) ?
                          static_cast<int>(areas[static_cast<size_t>(g_snackArea)].collected) : 0;
        g_playerInputsReady = true;
    }
    if (ImGui::CollapsingHeader("Health and balance", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (reason) ImGui::TextWrapped("Unavailable: %s", reason);
        if (readable) ImGui::Text("Health %u/5 | Spendable snacks %u", snapshot.health, snapshot.snacks);
        ImGui::TextWrapped("One-time edits, not damage simulation. No kill/resurrection or automatic save.");
        ImGui::BeginDisabled(reason != nullptr);
        ImGui::SetNextItemWidth(OverlayTheme::Px(150));
        ImGui::InputInt("Health (1..5)", &g_healthInput, 1, 1);
        ImGui::BeginDisabled(g_healthInput < 1 || g_healthInput > 5);
        if (ImGui::Button("Set health")) {
            g_status = ChangePlayerStat(PlayerStat::Health, static_cast<uint32_t>(g_healthInput));
            ProgressLog("health", g_status);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Restore to 5")) {
            g_status = ChangePlayerStat(PlayerStat::Health, 5);
            ProgressLog("health 5", g_status);
        }
        ImGui::SetNextItemWidth(OverlayTheme::Px(150));
        ImGui::InputInt("Spendable snacks", &g_snacksInput);
        ImGui::TextDisabled("0..99999: diagnostic input limit, not the game's maximum.");
        ImGui::BeginDisabled(g_snacksInput < 0 || g_snacksInput > static_cast<int>(kDiagnosticSnackLimit));
        if (ImGui::Button("Set snack balance")) {
            g_status = ChangePlayerStat(PlayerStat::Snacks, static_cast<uint32_t>(g_snacksInput));
            ProgressLog("snacks", g_status);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::TextWrapped("Balance edits leave collection totals and galleries unchanged. A later normal save can retain them.");
    }
    if (ImGui::CollapsingHeader("Power-ups")) DrawPowerups();
    if (ImGui::CollapsingHeader("Collected-snack progression")) {
        ImGui::TextWrapped("Advanced counter diagnostic. Can change natural picture-gallery access and persist in a "
                           "later normal save. Does NOT collect/remove/respawn physical snacks or reconcile "
                           "scene-local pickup statistics. Balance stays unchanged. Reopen galleries to refresh.");
        SnackAreas areas{};
        if (reason) ImGui::TextWrapped("Unavailable: %s", reason);
        else if (!ReadSnackAreas(ProgressRead, areas))
            ImGui::TextWrapped("Unavailable: unreadable/modified area schema or unsupported counts/targets.");
        else {
            const auto selected = static_cast<uint32_t>(g_snackArea);
            if (ImGui::BeginCombo("Area", SnackAreaLabel(selected, areas[selected]).c_str())) {
                for (uint32_t i = 1; i < areas.size(); ++i) {
                    if (ImGui::Selectable(SnackAreaLabel(i, areas[i]).c_str(), i == selected) && i != selected) {
                        g_snackArea = static_cast<int>(i);
                        g_snackAreaInput = static_cast<int>(areas[i].collected);
                        if (g_snackConfirmation.active) {
                            g_snackConfirmation.Clear();
                            g_status = "Collected-snack confirmation cancelled: selected area changed.";
                            ProgressLog("snackcancel", g_status);
                        }
                    }
                }
                ImGui::EndCombo();
            }
            const auto& area = areas[static_cast<size_t>(g_snackArea)];
            uint32_t sum = 0;
            for (size_t i = 1; i < areas.size(); ++i) sum += areas[i].collected;
            ImGui::Text("Collected %u / live target %u | All real areas (sum) %u", area.collected, area.target, sum);
            ImGui::SetNextItemWidth(OverlayTheme::Px(150));
            if (ImGui::InputInt("Area collected count", &g_snackAreaInput) && g_snackConfirmation.active) {
                g_snackConfirmation.Clear();
                g_status = "Collected-snack confirmation cancelled: requested count changed.";
                ProgressLog("snackcancel", g_status);
            }
            ImGui::BeginDisabled(g_snackAreaInput < 0 || static_cast<uint32_t>(g_snackAreaInput) > area.target);
            if (ImGui::Button("Review progression change")) {
                g_status = StageSnackArea(static_cast<uint32_t>(g_snackArea), static_cast<uint32_t>(g_snackAreaInput));
                ProgressLog("snackarea", g_status);
            }
            ImGui::EndDisabled();
            if (g_snackConfirmation.active) {
                ImGui::TextWrapped("Confirm %s: %u -> %u collected. Affects galleries; later saves can retain it.",
                                   SnackAreaLabel(g_snackConfirmation.area, areas[g_snackConfirmation.area]).c_str(),
                                   g_snackConfirmation.before, g_snackConfirmation.value);
                if (ImGui::Button("Confirm collected progression")) {
                    g_status = ConfirmSnackArea();
                    ProgressLog("snackconfirm", g_status);
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel progression change")) {
                    g_snackConfirmation.Clear();
                    g_status = "Collected-snack confirmation cancelled.";
                    ProgressLog("snackcancel", g_status);
                }
            }
        }
    }
}

// Textures tab (Suspect / LoadSuspects above PollCommandFile). "Save spot" saves Scooby's position under the suspect's ID
// in this scene (other scenes keep theirs).
void SaveSpotAs(const std::string& name) {
    Spot s;
    if (!ReadPlayer(s) || !s.scene) {
        g_status = "Player not found.";
        return;
    }
    s.name = name;
    AddCam(s);
    g_spots.erase(std::remove_if(g_spots.begin(), g_spots.end(),
                                 [&](const Spot& o) { return o.name == name && o.scene == s.scene; }),
                  g_spots.end());
    g_spots.push_back(s);
    SaveSpots();
    g_status = "Saved " + name + " in " + Lower(IdString(s.scene));
}

// Toggles keep their label and turn this colour while on (a changing label would shift the table).
const ImVec4 kFindColor(0.75f, 0.2f, 0.75f, 1.0f);
const ImVec4 kFineColor(0.2f, 0.6f, 0.25f, 1.0f);
const ImVec4 kSavedColor(0.2f, 0.4f, 0.75f, 1.0f);

bool ToggleButton(const char* label, bool on, const ImVec4& col) {
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x * 1.2f, col.y * 1.2f, col.z * 1.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x * 1.4f, col.y * 1.4f, col.z * 1.4f, 1.0f));
    }
    const bool pressed = ImGui::SmallButton(label);
    if (on) ImGui::PopStyleColor(3);
    return pressed;
}

// This level's verdict, one button cycling Unset -> Fine -> Bugged -> Fixed -> Unset; fixed width so the table doesn't
// move, and one ID whatever it says ("###verdict") so the nav cursor stays on it.
const ImVec4 kBuggedColor(0.75f, 0.2f, 0.2f, 1.0f);
const ImVec4 kFixedColor(0.25f, 0.45f, 0.8f, 1.0f);

std::string NextVerdict(const std::string& mark) {
    return mark.empty() ? "fine" : mark == "fine" ? "bugged" : mark == "bugged" ? "fixed" : "";
}

bool VerdictButton(const std::string& mark) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::CalcTextSize("Bugged").x + style.FramePadding.x * 2.0f;
    const bool on = !mark.empty();
    const ImVec4 col = mark == "fine" ? kFineColor : mark == "bugged" ? kBuggedColor : kFixedColor;
    if (on) {
        ImGui::PushStyleColor(ImGuiCol_Button, col);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(col.x * 1.2f, col.y * 1.2f, col.z * 1.2f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(col.x * 1.4f, col.y * 1.4f, col.z * 1.4f, 1.0f));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0.0f));  // SmallButton height
    const char* label = mark == "fine"     ? "Fine###verdict"
                        : mark == "bugged" ? "Bugged###verdict"
                        : mark == "fixed"  ? "Fixed###verdict"
                                           : "Unset###verdict";
    const bool pressed = ImGui::Button(label, ImVec2(width, 0.0f));
    ImGui::PopStyleVar();
    if (on) ImGui::PopStyleColor(3);
    return pressed;
}

// Scenes from the level table (menus and boot aren't scenes you can load into or mark).
bool IsPlayable(uint32_t id) {
    for (const Level& level : g_levels)
        for (const Scene& scene : level.scenes)
            if (scene.id == id) return true;
    return false;
}

bool Done(const std::string& verdict) { return verdict == "fine" || verdict == "fixed"; }

// Fine or fixed in how many of the playable scenes that use it, and how many of those there are.
std::pair<int, int> FineCount(const Suspect& s) {
    int fine = 0, total = 0;
    for (uint32_t id : s.scenes) {
        if (!IsPlayable(id)) continue;
        ++total;
        fine += Done(Verdict(s.id, id));
    }
    return {fine, total};
}

// Texture log (Textures tab checkbox, or NO100F_TEXLOG=1 at start): every texture's draw states go to
// Play\devmenu_texlog.csv (aurora gx.cpp), rows tagged with the scene ("none" before a level is loaded).
bool g_texLog = [] {
    const char* v = std::getenv("NO100F_TEXLOG");
    return v != nullptr && *v != '\0' && std::string(v) != "0";
}();

void UpdateTexLog() {
    static std::string last;
    const uint32_t scene = CurrentSceneId();
    const std::string want = !g_texLog ? "" : scene ? Lower(IdString(scene)) : "none";
    if (want == last) return;
    last = want;
    aurora_texture_log(want.c_str());
}

// Rows marked since the menu opened stay listed with "Hide fine/fixed" on, so the cursor isn't lost mid-cycle.
std::set<std::string> g_markedOpen;

// Textures tab (effects = false) / Effects tab (effects = true: drawn at runtime, e.g. on a kill or a bonk).
void DrawTextures(bool effects) {
    bool& thisScene = g_suspectsThisScene[effects ? 1 : 0];
    bool& hideFine = g_hideFine[effects ? 1 : 0];
    if (effects)
        ImGui::TextWrapped("Textures drawn at runtime (bonks, pickups, particles). This is not a gameplay-effects toggle.");
    if (ImGui::Button("Reload devmenu_textures.txt")) {
        g_suspects.clear();
        g_suspectsLoaded = false;
    }
    LoadSuspects();
    LoadSpots();
    LoadLevels();
    OverlayTheme::Toggle("Log every texture (devmenu_texlog.csv)", &g_texLog);
#ifdef AURORA_HAS_CUTOUT_MODE
    // Branches with aurora's cut-out modes (feat/hd-cutout-twopass): switch live to compare.
    static const char* const kCutoutModes[] = {"Game (alpha > 0)", "Cut (HD: alpha > 127)", "Two-pass (HD)",
                                               "Two-pass (all textures)"};
    int cutout = aurora_get_cutout_mode();
    if (!effects) {
        ImGui::SetNextItemWidth(OverlayTheme::Px(220.0f));
        if (ImGui::Combo("Cut-outs (F7 cycles)", &cutout, kCutoutModes, 4)) aurora_set_cutout_mode(cutout);
    }
#endif
    if (g_suspects.empty()) {
        ImGui::TextWrapped("No list loaded: put devmenu_textures.txt in UserData (e.g. from autotest\\texhunt), then Reload. Effects entries have 'effect' in the seventh column.");
        return;
    }
    OverlayTheme::Toggle("This level only", &thisScene);
    ImGui::SameLine();
    OverlayTheme::Toggle("Hide fine/fixed", &hideFine);
    ImGui::SameLine();
    ImGui::TextDisabled("drawn = seen on screen in this level");
    const uint32_t cur = CurrentSceneId();
    // boot.HIP textures are loaded for every scene
    const auto isHere = [&](const Suspect& s) {
        return std::find(s.scenes.begin(), s.scenes.end(), cur) != s.scenes.end() ||
               std::find(s.scenes.begin(), s.scenes.end(), IdFromString("BOOT")) != s.scenes.end();
    };
    // "fine" = fine/fixed in this scene (this scene only) / in all its scenes (all scenes)
    const auto isFine = [&](const Suspect& s) {
        const auto [fineScenes, totalScenes] = FineCount(s);
        return thisScene ? Done(Verdict(s.id, cur)) : totalScenes > 0 && fineScenes == totalScenes;
    };
    const auto shown = [&](const Suspect& s) {
        return s.effect == effects && (!thisScene || isHere(s)) && !(hideFine && isFine(s) && !g_markedOpen.count(s.id));
    };
    if (std::none_of(g_suspects.begin(), g_suspects.end(), shown)) {
        ImGui::TextWrapped("Nothing to show: untick 'This level only' or 'Hide fine/fixed'.");
        return;
    }
    std::string lastLevel;
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("suspects", 7, kTableFlags)) return;
    for (size_t i = 0; i < g_suspects.size(); ++i) {
        const Suspect& s = g_suspects[i];
        if (!shown(s)) continue;
        const bool here = isHere(s);
        const std::string verdict = Verdict(s.id, cur);
        const auto [fineScenes, totalScenes] = FineCount(s);
        const bool fine = isFine(s);
        if (!thisScene && s.level != lastLevel) {
            lastLevel = s.level;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.level.c_str());
        }
        ImGui::PushID(static_cast<int>(i));
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const ImVec4 col = s.severity == "high" ? ImVec4(1.0f, 0.42f, 0.42f, 1.0f)
                           : s.severity == "medium" ? ImVec4(1.0f, 0.72f, 0.3f, 1.0f)
                                                    : ImVec4(0.6f, 0.67f, 0.73f, 1.0f);
        if (fine) {  // crossed out
            ImGui::TextDisabled("%s", s.id.c_str());
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            const float y = (a.y + b.y) * 0.5f;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), ImGui::GetColorU32(ImGuiCol_TextDisabled));
        } else {
            ImGui::TextColored(col, "%s", s.id.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(s.asset.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", s.key.c_str(), s.level.c_str());
        ImGui::TableNextColumn();
        if (Drawn(s.id, cur)) {
            ImGui::TextColored(ImVec4(0.45f, 0.86f, 0.45f, 1.0f), "drawn");
        } else {
            ImGui::TextDisabled("not yet");
        }
        if (ImGui::IsItemHovered()) {
            const std::string where = DrawnScenes(s.id);
            ImGui::SetTooltip("drawn in: %s", where.empty() ? "no level yet" : where.c_str());
        }
        ImGui::TableNextColumn();
        const Spot* spot = FindSpot(s.id, cur);
        if (here && ToggleButton("Save spot", spot && spot->scene == cur, kSavedColor)) SaveSpotAs(s.id);
        if (!here) {
            const auto it = std::find_if(s.scenes.begin(), s.scenes.end(), IsPlayable);
            if (it != s.scenes.end()) {
                const std::string label = "Go " + Lower(IdString(*it));
                if (ImGui::SmallButton(label.c_str())) GoToScene(*it);
            } else {
                ImGui::TextDisabled("menu");
            }
        }
        ImGui::TableNextColumn();
        if (spot) {
            const std::string label = spot->scene == cur ? "Go to spot" : "Go to spot " + Lower(IdString(spot->scene));
            if (ImGui::SmallButton(label.c_str())) GoToSpot(*spot);
        }
        ImGui::TableNextColumn();
        const bool finding = g_findId == s.id;
        if (ToggleButton("Find", finding, kFindColor)) SetFind(finding ? nullptr : &s);
        ImGui::TableNextColumn();
        if (totalScenes > 1) {
            ImGui::TextDisabled("%d/%d", fineScenes, totalScenes);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("fine or fixed in %d of its %d levels", fineScenes, totalScenes);
            ImGui::SameLine();
        }
        if (here && IsPlayable(cur) && VerdictButton(verdict)) {
            SetVerdict(s.id, cur, NextVerdict(verdict));
            g_markedOpen.insert(s.id);
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// While Find is on: a label at the bottom centre (also with the menu closed); takes no input or focus.
void DrawFindToast() {
    if (g_findId.empty()) return;
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 40.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##devmenu_find", nullptr, kFlags))
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 1.0f, 1.0f), "Finding %s: drawn solid magenta", g_findId.c_str());
    ImGui::End();
}

// F7 (branches with aurora's cut-out modes): cycle the mode, shown for 2 s at the bottom of the screen.
uint64_t g_cutoutToastUntil = 0;

void CycleCutoutMode() {
#ifdef AURORA_HAS_CUTOUT_MODE
    aurora_set_cutout_mode((aurora_get_cutout_mode() + 1) % 4);
    g_cutoutToastUntil = SDL_GetTicks() + 2000;
#endif
}

void DrawCutoutToast() {
#ifdef AURORA_HAS_CUTOUT_MODE
    if (SDL_GetTicks() >= g_cutoutToastUntil) return;
    static const char* const kNames[] = {"Game (alpha > 0)", "Cut (HD: alpha > 127)", "Two-pass (HD)",
                                         "Two-pass (all textures)"};
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y - 80.0f),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.75f);
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoDocking;
    if (ImGui::Begin("##devmenu_cutout", nullptr, kFlags))
        ImGui::Text("Cut-outs: %d %s", aurora_get_cutout_mode(), kNames[aurora_get_cutout_mode() & 3]);
    ImGui::End();
#endif
}

// Button presses for scripts, also while a cutscene plays (devmenu.txt is read only in play): every 30 frames
// "devmenu_press.txt" is read and deleted; each line "BUTTON[+BUTTON...] [FRAMES] [cutscene]" (a b x y z start up down
// left right, and the control stick pushed all the way: sup sdown sleft sright, half way: hup hdown hleft hright; default
// 10 frames) is held on pad 1 for that many frames, lines one after another. "cutscene": only if a recorded cutscene is
// playing then (A skips it after a few seconds; in play A jumps). Logged to devmenu.log.
struct Press {
    uint16_t buttons;
    int frames;
    int8_t stickX = 0;
    int8_t stickY = 0;
};
std::vector<Press> g_presses;
int g_pressFrames = 0;  // frames the front press has been held

// The stick part of a press line: true if every name is a button or a stick direction.
bool PressStick(const std::string& names, int8_t& x, int8_t& y) {
    static const std::map<std::string, std::pair<int, int>> kStick{
        {"sup", {0, 1}}, {"sdown", {0, -1}}, {"sleft", {-1, 0}}, {"sright", {1, 0}},
        {"hup", {0, 2}}, {"hdown", {0, -2}}, {"hleft", {-2, 0}}, {"hright", {2, 0}}};
    int sx = 0, sy = 0;
    std::istringstream ss(names);
    for (std::string n; std::getline(ss, n, '+');) {
        const auto it = kStick.find(Lower(n));
        if (it == kStick.end()) continue;
        const auto value = [](int v) { return v == 0 ? 0 : (std::abs(v) == 1 ? 110 : 55) * (v > 0 ? 1 : -1); };
        if (it->second.first != 0) sx = value(it->second.first);
        if (it->second.second != 0) sy = value(it->second.second);
    }
    x = static_cast<int8_t>(sx);
    y = static_cast<int8_t>(sy);
    return sx != 0 || sy != 0;
}

// PressBits' "valid line" bit for a line with only stick directions (no PAD_BUTTON_* uses it); masked off when held.
constexpr uint16_t kStickOnly = 0x8000;

uint16_t PressBits(const std::string& names) {
    static const std::map<std::string, uint16_t> kBits{
        {"a", PAD_BUTTON_A},       {"b", PAD_BUTTON_B},       {"x", PAD_BUTTON_X},         {"y", PAD_BUTTON_Y},
        {"z", PAD_TRIGGER_Z},      {"start", PAD_BUTTON_START}, {"up", PAD_BUTTON_UP},     {"down", PAD_BUTTON_DOWN},
        {"left", PAD_BUTTON_LEFT}, {"right", PAD_BUTTON_RIGHT}};
    uint16_t bits = 0;
    bool any = false;
    std::istringstream ss(names);
    for (std::string n; std::getline(ss, n, '+');) {
        const std::string name = Lower(n);
        if (name.size() > 1 && (name[0] == 's' || name[0] == 'h') &&
            (name.substr(1) == "up" || name.substr(1) == "down" || name.substr(1) == "left" || name.substr(1) == "right")) {
            any = true;  // a stick direction (PressStick)
            continue;
        }
        const auto it = kBits.find(name);
        if (it == kBits.end()) return 0;
        bits |= it->second;
        any = true;
    }
    return any ? (bits | kStickOnly) : 0;
}

void UpdatePresses() {
    static uint32_t tick = 0;
    if (++tick % 30 == 0) {
        if (std::ifstream in{"devmenu_press.txt"}) {
            std::ofstream log("devmenu.log", std::ios::app);
            for (std::string l; std::getline(in, l);) {
                std::istringstream ss(l);
                std::string names;
                int frames = 10;
                std::string when;
                if (!(ss >> names)) continue;
                ss >> frames >> when;
                const uint16_t bits = PressBits(names);
                const bool skip = when == "cutscene" && !CutscenePlaying();
                Press press{static_cast<uint16_t>(bits & ~kStickOnly), frames};
                PressStick(names, press.stickX, press.stickY);
                if (bits && frames > 0 && !skip) g_presses.push_back(press);
                log << "press " << l << " -> " << (!bits ? "unknown button" : skip ? "skipped (no cutscene)" : "queued") << "\n";
            }
            in.close();
            std::remove("devmenu_press.txt");
        }
    }
    if (!g_presses.empty() && ++g_pressFrames > g_presses.front().frames) {
        g_presses.erase(g_presses.begin());
        g_pressFrames = 0;
    }
}

} // namespace

void GalleryConditionalCallback(CpuContext* ctx) {
    const uint32_t target = static_cast<uint32_t>(ctx->gpr[4]);
    const uint32_t event = static_cast<uint32_t>(ctx->gpr[5]);
    GalleryRule rule = GalleryRule::Natural;
    if (Enabled() && event == 0x3C) {
        ObserveGallery();
        const auto state = ProgressState();
        const bool active = g_galleryRules.Get(GallerySection::Pictures) != GalleryRule::Natural ||
                            g_galleryRules.Get(GallerySection::Movies) != GalleryRule::Natural;
        if (active && state.layout && state.inPlay && state.context && !state.cinematic && !state.movie && !state.transfer) {
            const auto snapshot = ReadGallery();
            for (size_t i = 0; i < snapshot.bases.size(); ++i) {
                if (snapshot.bases[i] != target) continue;
                const auto spec = ConditionAt(i);
                if (spec.section == GallerySection::Pictures ? snapshot.pictures : snapshot.movies) {
                    rule = g_galleryRules.Get(spec.section);
                } else {
                    g_galleryRules.Set(spec.section, GalleryRule::Natural);
                    g_status = "Gallery override cleared: condition/link schema changed.";
                    ProgressLog("gallery invalidated", g_status);
                }
                break;
            }
        }
    }
    ctx->gpr[3] = ConditionalEvent(target, event, rule,
        [&](uint32_t base) {
            ctx->gpr[3] = base;
            func_80045AC8(ctx);
            return ctx->gpr[3] != 0;
        },
        [&](uint32_t base) {
            ctx->gpr[3] = base;
            ctx->gpr[4] = Memory::Read32(base + 0x10);
            func_8001C1C4(ctx);
        },
        [&](uint32_t base, uint32_t result) {
            ctx->gpr[3] = ctx->gpr[4] = base;
            ctx->gpr[5] = result;
            func_80048B2C(ctx);
        });
}

bool Enabled() noexcept {
    static const bool enabled = RuntimeConfigFile::DevTools();
    return enabled;
}

void HandleSdlEvent(const SDL_Event& event) noexcept {
    if (!Enabled()) return;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F8) g_open = !g_open;
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F7) CycleCutoutMode();
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F3)
        no100f_recorder::Toggle();
    // LB: not mapped on the default controller layouts (it is L on the NSO GameCube one)
    if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER && g_open)
        g_toggleFocus = true;
}

bool PressedStick(int8_t& x, int8_t& y) noexcept {
    if (g_presses.empty() || g_pressFrames == 0) return false;
    x = g_presses.front().stickX;
    y = g_presses.front().stickY;
    return x != 0 || y != 0;
}

uint16_t PressedButtons() noexcept {
    const uint16_t scripted = g_presses.empty() || g_pressFrames == 0 ? 0 : g_presses.front().buttons;
    return scripted | (g_skipPressed && SameSkipSequence() ? g_skipButtons : 0);
}

bool IsOpen() noexcept { return g_open; }

std::string WhereText() {
    Spot s;
    if (!ReadPlayer(s)) return "Player not found.";
    char b[128];
    std::snprintf(b, sizeof b, "%s %.3f %.3f %.3f yaw %.1f", Lower(IdString(s.scene)).c_str(), s.x, s.y, s.z,
                  s.yaw * 180.0f / 3.14159265f);
    return b;
}

bool BlocksInput() noexcept { return (g_open && g_focused) || g_cam.on; }

// RwCameraBeginUpdate replacement hook (see FreeCam): the main camera's frame gets the free camera pose.
void ApplyFreecam(uint32_t camera, CpuContext* ctx) {
    if (!g_cam.on || camera != Read32Or0(kMainCameraPtr)) return;
    // A scene change (travel, a door, a warp) turns the free camera off: its pose belongs to the old scene.
    if (CurrentScenePtr() != g_camScene || CurrentSceneId() != g_camSceneId) {
        g_cam.on = false;
        return;
    }
    const uint32_t frame = MainCameraFrame();
    if (!frame) return;
    float rows[4][3];
    CamRows(g_cam, rows);
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t k = 0; k < 3; ++k) {
            WriteF(frame + 0x10 + r * 0x10 + k * 4, rows[r][k]);  // modelling
            WriteF(frame + 0x50 + r * 0x10 + k * 4, rows[r][k]);  // LTM
        }
    ctx->gpr[3] = frame;
    func_8016A300(ctx);  // RwFrameUpdateObjects: frame dirty -> view + frustum resynced
    ctx->gpr[3] = camera;
}

void Draw() noexcept {
    if (!Enabled() || !kReserved) return;
    ObserveGallery();
    if (InPlay() && CurrentSceneId() == kGalleryScene &&
        (g_galleryRules.Get(GallerySection::Pictures) != GalleryRule::Natural ||
         g_galleryRules.Get(GallerySection::Movies) != GalleryRule::Natural)) {
        const auto snapshot = ReadGallery();
        bool cleared = false;
        for (GallerySection section : {GallerySection::Pictures, GallerySection::Movies}) {
            if (!(section == GallerySection::Pictures ? snapshot.pictures : snapshot.movies) &&
                g_galleryRules.Get(section) != GalleryRule::Natural) {
                g_galleryRules.Set(section, GalleryRule::Natural);
                cleared = true;
            }
        }
        if (cleared) {
            g_status = "Gallery override cleared: missing/modified scene schema.";
            ProgressLog("gallery invalidated", g_status);
        }
    }
    UpdatePresses();
    const bool wasSkipping = g_skipRequest.Active();
    g_skipPressed = g_skipRequest.Press(SDL_GetTicks(), SameSkipSequence());
    if (wasSkipping && !g_skipRequest.Active())
        g_status = SameSkipSequence() ? "The game did not accept the skip within 10 seconds; retry when skippable."
                                      : "The cinematic sequence ended.";
    UpdatePending();
    ObservePlayerEdits();
    UpdateGod();
    UpdateMonsters();
    PollCommandFile();
    UpdateDrawn();
    UpdateTexLog();
    DrawFindToast();
    DrawCutoutToast();
    UpdateFreecam(g_open && g_focused);
    const bool wasFocused = g_focused;
    g_focused = false;
    if (!g_open) {
        g_toggleFocus = false;
        g_markedOpen.clear();
        return;
    }
    using OverlayTheme::Px;
    ImGui::SetNextWindowSize(ImVec2(Px(540.0f), Px(640.0f)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(Px(30.0f), Px(40.0f)), ImGuiCond_FirstUseEver);
    // LB: collapse when leaving the menu, expand + focus when coming back (ImGui puts the nav cursor back on the last item)
    if (g_toggleFocus) ImGui::SetNextWindowCollapsed(wasFocused);
    if (g_toggleFocus && !wasFocused) ImGui::SetNextWindowFocus();
    // NoNavFocus: ImGui's gamepad window switcher (X) would otherwise grab the menu while you play; LB toggles instead
    const bool visible = ImGui::Begin("Dev menu (F8)", &g_open, ImGuiWindowFlags_NoNavFocus);
    if (g_toggleFocus && wasFocused) ImGui::SetWindowFocus(nullptr);
    g_toggleFocus = false;
    g_focused = g_open && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (visible) {
        // Header: the sign lettering of the F10 board, and the build this is.
        {
            using namespace OverlayTheme;
            ImDrawList* list = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImFont* heading = Heading();
            OutlinedText(list, heading, at, Col(Colour::Lime), Col(Colour::Ink), Px(2), "DEV MENU");
            const float width = heading->CalcTextSizeA(heading->FontSize, FLT_MAX, 0.0f, "DEV MENU").x;
            ImFont* small = Small();
            list->AddText(small, small->FontSize,
                          ImVec2(at.x + width + Px(12), at.y + heading->FontSize - small->FontSize - Px(1)),
                          Col(Colour::CreamDim), BuildInfo::Id());
            ImGui::Dummy(ImVec2(0.0f, heading->FontSize + Px(2)));
        }
        ImGui::TextDisabled(g_focused ? "Game input paused: click the game or press LB to move."
                                      : "Click here or press LB to use the menu.");
        DrawStatus();
        ImGui::Separator();
        if (ImGui::BeginTabBar("devtabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
            if (ImGui::BeginTabItem("Spots")) {
                DrawSpots();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Levels")) {
                DrawScenes();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Monsters")) {
                DrawMonsters();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Player")) {
                DrawPlayer();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Warps")) {
                DrawWarps();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Gallery")) {
                DrawGallery();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Diagnostics")) {
                DrawDiagnostics();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Resources")) {
                no100f_resources::Draw();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Textures")) {
                ImGui::PushID("textures");  // both tabs draw the same widgets: keep their IDs apart
                DrawTextures(false);
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Effects")) {
                ImGui::PushID("effects");
                DrawTextures(true);
                ImGui::PopID();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace no100f_devmenu

extern "C" void No100f_DevmenuConditionalEvent(CpuContext* ctx) {
    no100f_devmenu::GalleryConditionalCallback(ctx);
}
PPC_NATIVE_OVERRIDE_VOID(80045A3C, No100f_DevmenuConditionalEvent, (CpuContext* ctx), (ctx));

// RwCameraBeginUpdate (0x80168C78) is `return camera->beginUpdate(camera)` (+0x18); native in full so the free camera
// can replace the main camera's frame first.
extern "C" void No100f_DevmenuCameraBeginUpdate(CpuContext* ctx) {
    const uint32_t camera = ctx->gpr[3];
    no100f_devmenu::ApplyFreecam(camera, ctx);
    InvokeIndirectCpu(Memory::Read32(camera + 0x18), ctx);
}
PPC_NATIVE_OVERRIDE_VOID(80168C78, No100f_DevmenuCameraBeginUpdate, (CpuContext* ctx), (ctx));
