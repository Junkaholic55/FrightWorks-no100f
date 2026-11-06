#include "hle_stubs.h"
#include "memory.h"
#include "hle/controller_status_contract.h"
#include "hle/no100f_pad_actions.h"
#include "hle/no100f_xbox_layout.h"
#include "controller_button_names.h"
#include "input_bindings.h"
#include "wii_remote_input.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "no100f_devmenu.h"

#include <aurora/aurora.h>
#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>

namespace {

std::atomic<bool> g_rumbleEnabled{true};

// Original Xbox controls (hle/no100f_xbox_layout.h), applied to each controller's mapped buttons inside PADRead.
std::array<No100fXboxLayout::PortState, PAD_CHANMAX> g_xboxPorts;
bool g_xboxMenu = false;
// The pad buttons for Black / White ([controller] xbox_black / xbox_white), read once per PADRead while the layout is
// on; PAD_NATIVE_BUTTON_DISABLED when off or unknown.
std::array<uint32_t, 2> g_xboxNative{PAD_NATIVE_BUTTON_DISABLED, PAD_NATIVE_BUTTON_DISABLED};

uint32_t XboxNative(size_t action) {
    const std::string name = RuntimeConfigFile::XboxButton(action);
    const auto* native = ControllerNames::FindNativeButton(name);
    if (native == nullptr || native->nativeButton == PAD_NATIVE_BUTTON_INVALID) {
        static std::array<std::string, 2> reported;
        if (reported[action] != name) {
            reported[action] = name;
            RT_LOGF(RT_TAG_CONFIG, "[xbox-layout] unknown %s button '%s': off\n", action == 0 ? "Black" : "White",
                    name.c_str());
        }
        return PAD_NATIVE_BUTTON_DISABLED;
    }
    // "left_trigger@30": the threshold rides in the binding like the button mapping's.
    if (PADIsAxisButton(native->nativeButton)) {
        if (const size_t at = name.find('@'); at != std::string::npos) {
            const int threshold = std::atoi(name.c_str() + at + 1);
            if (threshold >= 1 && threshold <= 100) {
                return PADAxisButtonIdentity(native->nativeButton) | (static_cast<uint32_t>(threshold) << 8);
            }
        }
    }
    return native->nativeButton;
}

bool IsXboxBound(uint32_t nativeButton) {
    if (nativeButton == PAD_NATIVE_BUTTON_INVALID || nativeButton == PAD_NATIVE_BUTTON_DISABLED) return false;
    return std::any_of(g_xboxNative.begin(), g_xboxNative.end(), [&](uint32_t bound) {
        return bound != PAD_NATIVE_BUTTON_DISABLED && PADAxisButtonIdentity(bound) == PADAxisButtonIdentity(nativeButton);
    });
}

// Buttons and trigger / stick directions, like aurora's own mapping (pad.cpp is_native_binding_pressed).
bool NativeButtonHeld(SDL_Gamepad* gamepad, uint32_t nativeButton) {
    if (gamepad == nullptr || nativeButton == PAD_NATIVE_BUTTON_INVALID || nativeButton == PAD_NATIVE_BUTTON_DISABLED) {
        return false;
    }
    if (PADIsAxisButton(nativeButton)) {
        const uint32_t axis = PADAxisButtonAxis(nativeButton);
        const uint32_t threshold = PADAxisButtonThreshold(nativeButton);
        if (axis >= SDL_GAMEPAD_AXIS_COUNT || threshold < 1 || threshold > 100) return false;
        int value = SDL_GetGamepadAxis(gamepad, static_cast<SDL_GamepadAxis>(axis));
        if (PADAxisButtonNegative(nativeButton)) value = -value;
        return value > 0 && value * 100 >= static_cast<int>(threshold) * 32767;
    }
    return nativeButton < SDL_GAMEPAD_BUTTON_COUNT &&
           SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(nativeButton));
}

u16 XboxLayoutFilter(u32 port, SDL_Gamepad* gamepad, u16 buttons) {
    if (port >= g_xboxPorts.size()) {
        return buttons;
    }
    // The Black / White buttons send only Black / White: drop the GameCube buttons that only they pressed.
    u16 bound = 0, others = 0;
    for (const bool alt : {false, true}) {
        u32 count = 0;
        PADButtonMapping* mappings = alt ? PADGetAltButtonMappings(port, &count) : PADGetButtonMappings(port, &count);
        for (u32 i = 0; mappings != nullptr && i < count; ++i) {
            if (IsXboxBound(mappings[i].nativeButton)) bound |= mappings[i].padButton;
            else if (NativeButtonHeld(gamepad, mappings[i].nativeButton)) others |= mappings[i].padButton;
        }
    }
    buttons = No100fXboxLayout::DropBound(buttons, bound, others);
    return No100fXboxLayout::Filter(g_xboxPorts[port], buttons, g_xboxMenu, NativeButtonHeld(gamepad, g_xboxNative[0]),
                                    NativeButtonHeld(gamepad, g_xboxNative[1]));
}

// A digital button bound to L or R has no analog travel of its own. On real
// hardware the click only engages at full depression, so report a full pull.
// The pad buttons player 1's GameCube A, B, X and Y are mapped to (config names, nullptr = not mapped), for the
// button prompts. `swapBX`: the original Xbox layout in gameplay, where the GameCube B button does X's job and back.
std::array<const char*, 4> PromptButtons(bool swapBX) {
    static constexpr u16 kSlots[] = {PAD_BUTTON_A, PAD_BUTTON_B, PAD_BUTTON_X, PAD_BUTTON_Y};
    std::array<const char*, 4> out{};
    u32 count = 0;
    const PADButtonMapping* mappings = PADGetButtonMappings(0, &count);
    for (size_t slot = 0; slot < out.size(); ++slot) {
        for (u32 i = 0; mappings != nullptr && i < count; ++i) {
            const u32 native = mappings[i].nativeButton;
            if (mappings[i].padButton != kSlots[slot] || native == PAD_NATIVE_BUTTON_INVALID ||
                native == PAD_NATIVE_BUTTON_DISABLED) {
                continue;
            }
            const auto& item = ControllerNames::NativeButtonForValue(native);
            if (item.nativeButton != PAD_NATIVE_BUTTON_INVALID) out[slot] = item.configName;
            break;
        }
    }
    if (swapBX) std::swap(out[1], out[2]);
    return out;
}

void FillTriggersHeldByButtons(PADStatus* statuses) {
    if (InputBindings::InputBlocked()) {
        return;
    }
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) {
            continue;
        }
        const s32 index = PADGetIndexForPort(port);
        if (index < 0) {
            continue;
        }
        SDL_Gamepad* gamepad = PADGetSDLGamepadForIndex(static_cast<u32>(index));
        if (gamepad == nullptr) {
            continue;
        }
        const auto scan = [&](PADButtonMapping* mappings, u32 count) {
            if (mappings == nullptr) {
                return;
            }
            for (u32 i = 0; i < count; ++i) {
                const PADButtonMapping& mapping = mappings[i];
                if (mapping.padButton != PAD_TRIGGER_L && mapping.padButton != PAD_TRIGGER_R) {
                    continue;
                }
                if (RuntimeConfigFile::XboxOriginalLayout() && IsXboxBound(mapping.nativeButton)) {
                    continue;  // Black / White, not gum / run
                }
                if (!NativeButtonHeld(gamepad, mapping.nativeButton)) {
                    continue;
                }
                if (mapping.padButton == PAD_TRIGGER_L) {
                    statuses[port].triggerLeft = 255;
                } else {
                    statuses[port].triggerRight = 255;
                }
            }
        };
        u32 count = 0;
        scan(PADGetButtonMappings(port, &count), count);
        count = 0;
        scan(PADGetAltButtonMappings(port, &count), count);
    }
}

void WritePadStatus(uint32_t base, const PADStatus& status) {
    const auto guestStatus = PadStatusContract::Encode({
        status.button,
        status.stickX,
        status.stickY,
        status.substickX,
        status.substickY,
        status.triggerL,
        status.triggerR,
        status.analogA,
        status.analogB,
        status.err,
    });
    uint8_t* dst = Memory::GetPointer(base, guestStatus.size());
    std::memcpy(dst, guestStatus.data(), guestStatus.size());
}

} // namespace

extern "C" void PAD_HLE_SetRumbleEnabled(bool enabled)
{
    g_rumbleEnabled.store(enabled, std::memory_order_relaxed);
}

void No100fPad::XboxButtons(uint32_t port, bool& black, bool& white)
{
    black = white = false;
    if (port < g_xboxPorts.size()) {
        black = g_xboxPorts[port].black;
        white = g_xboxPorts[port].white;
    }
}

extern "C" uint32_t PAD__Init_HLE()
{
    return PADInit() ? 1u : 0u;
}
// Scooby-Doo! Unmasked (GameCube): PADInit is the game's pad API.
PPC_NATIVE_OVERRIDE(800012CC, PAD__Init_HLE, uint32_t, (), ());

// PADRead: gathers every GameCube pad source for the frame and writes the statuses to guest memory.
extern "C" uint32_t PAD__Read_HLE(uint32_t statusPtr)
{
    if (statusPtr == 0) {
        return 0;
    }

    PADStatus statuses[PAD_CHANMAX]{};
    // Keep looking for a Bluetooth Wii Remote that dropped out (or was turned on late).
    WiiRemoteInput::Poll();
    const bool xboxLayout = RuntimeConfigFile::XboxOriginalLayout();
    if (xboxLayout) {
        const uint32_t scenePtr = Memory::Read32(No100fXboxLayout::kScenePointer);
        const uint32_t sceneAddress = scenePtr + No100fXboxLayout::kSceneIdOffset;
        const uint32_t scene = scenePtr && Memory::Contains(sceneAddress, 4) ? Memory::Read32(sceneAddress) : 0;
        const uint32_t mode = Memory::Read32(No100fXboxLayout::kModeAddress);
        g_xboxMenu = No100fXboxLayout::IsMenuMode(mode) || No100fXboxLayout::IsMenuScene(scene);
        for (auto& port : g_xboxPorts) {
            port.black = port.white = false;
        }
        g_xboxNative = {XboxNative(0), XboxNative(1)};
    }
    uint32_t rumbleMask = xboxLayout ? PADReadWithGamepadButtonFilter(statuses, XboxLayoutFilter) : PADRead(statuses);
    // Wii Remotes reach the game through KPAD, not as GameCube pads. This also
    // applies while input is blocked (overlay open) so the port does not flip
    // between "connected" and "no controller" every time the overlay toggles.
    WiiRemoteInput::HideRemotesFromPad(statuses, PAD_CHANMAX);

    // On-screen button prompts follow the first connected controller and player 1's button mapping (button_icons beside the game,
    // tools/make-button-icons.py); with the Xbox layout, gameplay swaps B and X.
    static int family = -1;
    if (static unsigned n = 0; n++ % 30 == 0) {
        int count = 0, style = -1;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
            if (count > 0) switch (SDL_GetGamepadTypeForID(ids[0])) {
                case SDL_GAMEPAD_TYPE_UNKNOWN: break;
                case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: style = 1; break;
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
                case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: style = 2; break;
                default: style = 0; break;
            }
            SDL_free(ids);
        }
        family = style;
    }
    const auto promptButtons = PromptButtons(xboxLayout && !g_xboxMenu);
    aurora_set_button_icons(family, promptButtons.data());
    FillTriggersHeldByButtons(statuses);
    if (xboxLayout) {
        // A trigger used for Black / White doesn't also pull the GameCube trigger (gum / run) with its analog travel.
        for (const uint32_t native : g_xboxNative) {
            if (native == PAD_NATIVE_BUTTON_DISABLED || !PADIsAxisButton(native)) continue;
            const uint32_t axis = PADAxisButtonAxis(native);
            for (auto& status : statuses) {
                if (axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) status.triggerLeft = 0;
                if (axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) status.triggerRight = 0;
            }
        }
    }
    InputBindings::Apply(statuses);
    if (const uint16_t pressed = no100f_devmenu::PressedButtons()) {
        statuses[0].err = PAD_ERR_NONE;
        statuses[0].button |= pressed;
    }
    if (int8_t x = 0, y = 0; no100f_devmenu::PressedStick(x, y)) {
        statuses[0].err = PAD_ERR_NONE;
        statuses[0].stickX = x;
        statuses[0].stickY = y;
    }
    if (uint16_t scripted = 0; WiiRemoteInput::ScriptedPadButtons(scripted)) {
        statuses[0].err = PAD_ERR_NONE;
        statuses[0].button |= scripted;
        if (scripted) { static int n = 0; if (n++ < 5) RT_LOGF(RT_TAG_OS, "[pad] PADRead scripted buttons 0x%04X\n", scripted); }
    }

    try {
        for (uint32_t i = 0; i < PAD_CHANMAX; ++i) {
            WritePadStatus(statusPtr + static_cast<uint32_t>(i * PadStatusContract::kGuestStatusSize),
                           statuses[i]);
        }
    } catch (const Memory::AccessViolation&) {
        return 0;
    }

    return rumbleMask;
}
// Scooby-Doo! Unmasked (GameCube): PADRead is the game's pad API.
PPC_NATIVE_OVERRIDE(8010CEE0, PAD__Read_HLE, uint32_t, (uint32_t statusPtr), (statusPtr));

extern "C" uint32_t PAD__Reset_HLE(uint32_t mask)
{
    return PADReset(mask) ? 1u : 0u;
}
// Scooby-Doo! Unmasked (GameCube): PADReset is the game's pad API.
PPC_NATIVE_OVERRIDE(8010CACC, PAD__Reset_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" uint32_t PAD__Recalibrate_HLE(uint32_t mask)
{
    return PADRecalibrate(mask) ? 1u : 0u;
}
// Scooby-Doo! Unmasked (GameCube): PADRecalibrate is the game's pad API.
PPC_NATIVE_OVERRIDE(8010CBCC, PAD__Recalibrate_HLE, uint32_t, (uint32_t mask), (mask));

extern "C" void PAD__ControlMotor_HLE(int32_t chan, uint32_t command)
{
    if (command == PAD_MOTOR_RUMBLE && !g_rumbleEnabled.load(std::memory_order_relaxed)) {
        command = PAD_MOTOR_STOP;
    }
    PADControlMotor(chan, command);
}
// Scooby-Doo! Unmasked (GameCube): PADControlMotor is the game's pad API.
PPC_NATIVE_OVERRIDE_VOID(8010D28C, PAD__ControlMotor_HLE, (int32_t chan, uint32_t command), (chan, command));
