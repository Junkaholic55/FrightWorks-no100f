#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef enum {
  AURORA_DISPLAY_MODE_WINDOWED,
  AURORA_DISPLAY_MODE_BORDERLESS,
  AURORA_DISPLAY_MODE_EXCLUSIVE,
} AuroraDisplayMode;

// A connected monitor for choosing where fullscreen goes. id survives reboots and display renumbering.
typedef struct {
  char id[128];
  char label[256];  // name, size and place: "VS248 (1920x1080, above right)"
  bool selected;    // what aurora_set_fullscreen_monitor's id currently resolves to
} AuroraMonitor;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

// GPU driver workarounds. AUTO picks them from the graphics adapter in use (see AuroraInfo.gpuName);
// ON and OFF force them, for a device the detection gets wrong.
typedef enum {
  AURORA_GPU_COMPAT_AUTO = 0,
  AURORA_GPU_COMPAT_ON = 1,
  AURORA_GPU_COMPAT_OFF = 2,
} AuroraGpuCompat;

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  // Read-only application resources. Defaults to SDL_GetBasePath(), which is
  // where release builds place initial_pipeline_cache.db.
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  uint16_t maxTextureAnisotropy;
  // No vsync knob exists: the swapchain is always configured for a
  // non-blocking present mode (Immediate, else Mailbox). See best_present_mode.
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureReplacements;
  bool allowTextureDumps;
  bool disableCopyFilter;
  // When false, Aurora centers the first window. When true, windowPosX/Y are restored verbatim,
  // including negative coordinates on monitors left of or above the primary display.
  bool hasWindowPosition;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;

  // Optional directory for the portable GX pipeline database. When null, the
  // database is stored in cachePath with Dawn's machine-specific cache.
  const char* pipelineCachePath;

  AuroraGpuCompat gpuCompat;
} AuroraConfig;

typedef enum {
  AURORA_INITIALIZATION_SUCCESS = 0,
  AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE = 1,
} AuroraInitializationStatus;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
  AuroraInitializationStatus initializationStatus;
  // On failure, owned by SDL on the calling thread. Copy before another SDL call.
  const char* initializationError;
  // The graphics adapter ("Adreno (TM) 840") and whether the GPU driver workarounds are on.
  const char* gpuName;
  bool gpuCompatActive;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
const AuroraEvent* aurora_update();
bool aurora_begin_frame();
void aurora_end_frame();
typedef void (*AuroraFrameWorkerWaitCallback)();
// Called from the producer thread at bounded intervals while Aurora waits for
// the asynchronous frame worker. The callback must not enter Aurora.
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback);
void aurora_wait_for_frame_worker();
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros);
// Absolute schedule for the next sealed frame, on steady_clock: baseNanos anchors the group and
// intervalNanos is the period, so slot k of N+1 fires at base + k*interval/(N+1). Zeros clear it.
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos);
// Reports whether the frame about to be sealed met its display boundary. Interpolation sizes its
// slot group from this, backing off after misses. Only paced presents may report.
void aurora_report_producer_paced(bool paced);
void aurora_request_frame_capture(uint32_t frame, const char* outputPath);
// Saves the next presented frame as a PNG at outputPath (UTF-8), encoded on a worker thread: the game frame without
// the ImGui overlay, or as aurora_set_capture_overlay says. Replaces a capture still waiting for its frame.
void aurora_request_screenshot(const char* outputPath);
// Outcome of the last screenshot: 1 saved, -1 failed, 0 nothing new. Reading it clears it.
int aurora_take_screenshot_result(void);
// What screenshots and video frames show of the ImGui overlay. NONE: screenshots are the game frame at its render size
// (the default). STAMP / ALL: the window's picture (game scaled as on screen) with only the ImGui window named
// AURORA_CAPTURE_STAMP_WINDOW on top / with every ImGui window. The window named AURORA_CAPTURE_HIDDEN_WINDOW is never
// captured (an on-screen recording light).
typedef enum {
  AURORA_CAPTURE_OVERLAY_NONE = 0,
  AURORA_CAPTURE_OVERLAY_STAMP = 1,
  AURORA_CAPTURE_OVERLAY_ALL = 2,
} AuroraCaptureOverlay;
#define AURORA_CAPTURE_STAMP_WINDOW "##CaptureStamp"
#define AURORA_CAPTURE_HIDDEN_WINDOW "##CaptureHidden"
void aurora_set_capture_overlay(AuroraCaptureOverlay overlay);
// Video capture: every sealed frame as the window's picture (overlay as set above, NONE counting as STAMP), scaled
// down to at most maxHeight rows, read back asynchronously and passed to the callback on a render thread with the
// Unix time (microseconds) it was rendered. Pixels are tightly packed 8-bit, BGRA when bgra is true, else RGBA; the
// pointer is valid only during the call. Frames are skipped while 4 are still being read back. NULL stops; after
// that returns, the callback is not called again. Returns false when the swapchain format can't be captured (HDR).
typedef void (*AuroraVideoFrameCallback)(const uint8_t* pixels, uint32_t width, uint32_t height, bool bgra,
                                         int64_t unixMicros, void* user);
bool aurora_set_video_capture(AuroraVideoFrameCallback callback, void* user, uint32_t maxHeight);
bool aurora_flush_efb_copies_to_ram();
bool aurora_flush_efb_copy_to_ram(void* dest);

void aurora_set_log_level(AuroraLogLevel level);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_display_mode(AuroraDisplayMode mode);
AuroraDisplayMode aurora_get_display_mode();
// Windows only (elsewhere: no monitors, fullscreen stays on the window's monitor). Fills up to maxCount, returns how many.
uint32_t aurora_get_monitors(AuroraMonitor* monitors, uint32_t maxCount);
// The monitor fullscreen goes to: an AuroraMonitor id, or "auto" for the one the window is on. A monitor that is gone
// falls back to the only connected one of the same model, else the main monitor. Applied at once when fullscreen.
void aurora_set_fullscreen_monitor(const char* id);
// Cut-out draws ("alpha > 0" + depth write: leaves, grass, webs): 0 = the game's own test, 1 = alpha > 127 for HD
// replacements, 2 = two passes for HD replacements (solid part with depth write, faint part blended without),
// 3 = two passes for every texture (originals too; the default). Takes effect at the next draw.
#define AURORA_HAS_CUTOUT_MODE 1
void aurora_set_cutout_mode(int mode);
int aurora_get_cutout_mode();
// Motion vectors (env AURORA_MOTION_VECTORS=1 at start, single-sampled EFB only): where every surface was last
// frame, per pixel. The view shows them on screen: 0 = off, 1 = motion only, 2 = over a dimmed picture (grey or the
// picture = still, hue = direction, saturation = speed, full at `scale` pixels per frame; 0 keeps the scale).
#define AURORA_HAS_MOTION_VECTORS 1
bool aurora_motion_vectors_active();
void aurora_set_motion_vector_view(int mode, float scale);
int aurora_get_motion_vector_view();
float aurora_get_motion_vector_view_scale();

typedef enum {
  AURORA_METALFX_DISABLED,
  AURORA_METALFX_UNSUPPORTED,
  AURORA_METALFX_NOT_UPSCALING,
  AURORA_METALFX_ACTIVE,
  AURORA_METALFX_ERROR,
} AuroraMetalFXStatus;

// Changes are consumed at the next sealed frame boundary. MetalFX only applies
// when both source dimensions are smaller than the aspect-fitted output.
void aurora_set_metalfx_spatial(bool enabled);
bool aurora_get_metalfx_spatial();
bool aurora_is_metalfx_spatial_supported();
// Button prompts for the controller in use: family 0 xbox, 1 playstation, 2 nintendo, -1 the game's own GameCube
// icons. buttons[4]: the pad button (controller_button_names.h config name, e.g. "south") drawn for the GameCube A,
// B, X and Y buttons; NULL, "" or a button without an icon = that GameCube button's usual one.
void aurora_set_button_icons(int family, const char* const buttons[4]);
// The folder of those icons (<UserData>/button_icons/<family>, UTF-8; one <config name>.png per pad button), or "" when
// no controller is connected (BUTTON_ICONS=<family> pins one, for tests). Writes at most size - 1 bytes and a NUL;
// returns the full length.
size_t aurora_get_button_icon_folder(char* out, size_t size);
// Dev menu (F8): whether texture_replacements/<name>.png|.dds has been drawn since start.
bool aurora_texture_replacement_used(const char* name);
// Dev menu (F8): forget what has been drawn (the dev menu tracks drawn per level).
void aurora_texture_replacement_reset_used();
// Dev menu (F8): draw the replacement with this file name (no extension) solid magenta; nullptr or "" = off.
void aurora_texture_replacement_highlight(const char* name);
// Dev menu (F8): log every texture's draw states to devmenu_texlog.csv, rows tagged with this scene; nullptr or
// "" = off.
void aurora_texture_log(const char* scene);
AuroraMetalFXStatus aurora_get_metalfx_status();

AuroraBackend aurora_get_backend();
const AuroraBackend* aurora_get_available_backends(size_t* count);

#ifdef __cplusplus
}
#endif

#endif
