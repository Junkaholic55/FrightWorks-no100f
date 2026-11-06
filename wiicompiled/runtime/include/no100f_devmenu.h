#pragma once

#include <cstdint>
#include <string>

#include <SDL3/SDL_events.h>

struct CpuContext;

// Dev menu (F8): teleport to any scene, save spots (scene + position + yaw) and teleport back to them.
// Shipped in every build, active only with [game] dev_tools = true (the launcher's Developer tools option).
namespace no100f_devmenu {
// [game] dev_tools, read once.
bool Enabled() noexcept;
void HandleSdlEvent(const SDL_Event& event) noexcept;
// Called from settings_overlay::Draw (game thread, once per GXCopyDisp, inside the ImGui frame).
void Draw() noexcept;
bool IsOpen() noexcept;
// The "where" command's answer ("h001 -19.315 0.060 16.083 yaw 90.0"), or why there is none. Game thread.
std::string WhereText();
// Game input is held: menu open and focused (click the game or press LB to move with it open), or free camera on.
bool BlocksInput() noexcept;
// Free camera: called from the RwCameraBeginUpdate override with the camera about to render.
void ApplyFreecam(uint32_t camera, CpuContext* ctx);
// Pad 1 buttons (PAD_BUTTON_*) held by the "devmenu_press.txt" file command or the Skip button, ORed in by PADRead.
uint16_t PressedButtons() noexcept;
// Pad 1 control stick held by the same file command (sup/sdown/sleft/sright, hup/...); false when none.
bool PressedStick(int8_t& x, int8_t& y) noexcept;
} // namespace no100f_devmenu
