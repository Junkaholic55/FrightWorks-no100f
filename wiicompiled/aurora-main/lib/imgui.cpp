#include "imgui.hpp"
#include <aurora/window_owner.hpp>
#include <stdexcept>

#include <cstddef>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <webgpu/webgpu_cpp.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_render.h>

#include "fs_helper.hpp"
#include "internal.hpp"
#include "webgpu/gpu.hpp"
#include "window.hpp"

#define IMGUI_IMPL_WEBGPU_BACKEND_DAWN
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "backends/imgui_impl_wgpu.h"
#include "tracy/Tracy.hpp"

namespace aurora::imgui {
static float g_scale;
static std::string g_imguiLog{};
static bool g_useSdlRenderer = false;
// Set once ImGui::Render() has produced this frame's draw data. Interpolation encodes up to four
// ImGui passes per frame, and every one of them used to rebuild the draw lists from scratch.
static bool g_frameDataBuilt = false;

static std::vector<SDL_Texture*> g_sdlTextures;
static std::vector<wgpu::Texture> g_wgpuTextures;

void remove_legacy_ini_file(const char* basePath) noexcept {
  if (basePath == nullptr || *basePath == '\0') {
    return;
  }

  std::error_code ec;
  std::filesystem::remove(fs_path_from_string(basePath) / "imgui.ini", ec);
}

void create_context() noexcept {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  remove_legacy_ini_file(g_config.userPath);
  remove_legacy_ini_file(g_config.cachePath);
  g_imguiLog = std::string{g_config.cachePath} + "/imgui.log";
  io.IniFilename = nullptr;
  io.LogFilename = g_imguiLog.c_str();
  ImGui::LoadIniSettingsFromMemory("", 0);
  io.WantSaveIniSettings = false;
}

void initialize() {
  ZoneScoped;
  SDL_Renderer* renderer = window::get_sdl_renderer();
  ImGui_ImplSDL3_InitForSDLRenderer(window::get_sdl_window(), renderer);
  ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);  // every connected pad plays, so every pad navigates
  g_useSdlRenderer = renderer != nullptr;
  if (g_useSdlRenderer) {
    ImGui_ImplSDLRenderer3_Init(renderer);
  } else {
    ImGui_ImplWGPU_InitInfo info;
    info.Device = webgpu::g_device.Get();
    info.RenderTargetFormat = static_cast<WGPUTextureFormat>(webgpu::g_graphicsConfig.surfaceConfiguration.format);
    // Interpolation records up to four ImGui passes in one command buffer, so keep three logical
    // frames of renderer resources to stop them overwriting each other's vertex/index buffers.
    info.NumFramesInFlight = 12;
    ImGui_ImplWGPU_Init(&info);
  }
}

void shutdown() {
  ZoneScoped;
  // Startup can fail before either backend initializes. A context alone does
  // not mean its renderer/platform backend owns resources to release.
  if (ImGui::GetCurrentContext() != nullptr) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.BackendRendererUserData != nullptr) {
      if (g_useSdlRenderer) {
        ImGui_ImplSDLRenderer3_Shutdown();
      } else {
        ImGui_ImplWGPU_Shutdown();
      }
    }
    if (io.BackendPlatformUserData != nullptr) {
      ImGui_ImplSDL3_Shutdown();
    }
    ImGui::DestroyContext();
  }
  for (const auto& texture : g_sdlTextures) {
    SDL_DestroyTexture(texture);
  }
  g_sdlTextures.clear();
  g_wgpuTextures.clear();
  g_useSdlRenderer = false;
  g_scale = 0.f;
  g_frameDataBuilt = false;
}

void process_event(const SDL_Event& event) noexcept {
  auto renderEvent = event;
  if (g_useSdlRenderer) {
    if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
      SDL_ConvertEventToRenderCoordinates(renderer, &renderEvent);
    }
  }
  ImGui_ImplSDL3_ProcessEvent(&renderEvent);
}

bool wants_capture_event(const SDL_Event& event) noexcept {
  if (ImGui::GetCurrentContext() == nullptr) {
    return false;
  }

  const ImGuiIO& io = ImGui::GetIO();
  switch (event.type) {
  case SDL_EVENT_MOUSE_MOTION:
  case SDL_EVENT_MOUSE_BUTTON_DOWN:
  case SDL_EVENT_MOUSE_BUTTON_UP:
  case SDL_EVENT_MOUSE_WHEEL:
  case SDL_EVENT_FINGER_DOWN:
  case SDL_EVENT_FINGER_MOTION:
  case SDL_EVENT_FINGER_UP:
  case SDL_EVENT_FINGER_CANCELED:
    return io.WantCaptureMouse;
  case SDL_EVENT_KEY_DOWN:
  case SDL_EVENT_KEY_UP:
  case SDL_EVENT_TEXT_INPUT:
    return io.WantCaptureKeyboard || io.WantTextInput;
  default:
    return false;
  }
}

void new_frame(const AuroraWindowSize& size) {
  if (window_owner::enabled() && (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable))
    throw std::runtime_error("Window owner does not support ImGui multi-viewports");
  ZoneScoped;
  ImVec2 framebufferScale{
      size.width > 0 ? static_cast<float>(size.native_fb_width) / static_cast<float>(size.width) : 1.0f,
      size.height > 0 ? static_cast<float>(size.native_fb_height) / static_cast<float>(size.height) : 1.0f,
  };
  ImVec2 displaySize{static_cast<float>(size.width), static_cast<float>(size.height)};

  if (g_useSdlRenderer) {
    if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
      float renderScaleX = 1.0f;
      float renderScaleY = 1.0f;
      SDL_GetRenderScale(renderer, &renderScaleX, &renderScaleY);
      if (renderScaleX > 0.0f && renderScaleY > 0.0f &&
          (std::fabs(renderScaleX - 1.0f) > 0.0001f || std::fabs(renderScaleY - 1.0f) > 0.0001f)) {
        int outputWidth = static_cast<int>(size.native_fb_width);
        int outputHeight = static_cast<int>(size.native_fb_height);
        SDL_GetRenderOutputSize(renderer, &outputWidth, &outputHeight);
        displaySize = {
            static_cast<float>(outputWidth) / renderScaleX,
            static_cast<float>(outputHeight) / renderScaleY,
        };
        framebufferScale = {renderScaleX, renderScaleY};
      }
    }
    ImGui_ImplSDLRenderer3_NewFrame();
    g_scale = size.scale;
  } else {
    if (g_scale != size.scale) {
      if (g_scale > 0.f) {
        ImGui_ImplWGPU_CreateDeviceObjects();
      }
      g_scale = size.scale;
    }
    if (!ImGui::GetIO().Fonts->IsBuilt()) {
      ImGui_ImplWGPU_CreateDeviceObjects();
    }
    ImGui_ImplWGPU_NewFrame();
  }
  ImGui_ImplSDL3_NewFrame();

  ImGuiIO& io = ImGui::GetIO();
  io.DisplayFramebufferScale = framebufferScale;
  ImGui::GetIO().DisplaySize = displaySize;
  ImGui::NewFrame();
  g_frameDataBuilt = false;
}

void render_frame_data() noexcept {
  ZoneScoped;
  if (g_frameDataBuilt) {
    return;
  }
  ImGui::Render();
  auto* data = ImGui::GetDrawData();
  data->FramebufferScale = ImGui::GetIO().DisplayFramebufferScale;
  g_frameDataBuilt = true;
}

void render(const wgpu::RenderPassEncoder& pass, uint32_t targetWidth, uint32_t targetHeight) noexcept {
  ZoneScoped;
  render_frame_data();

  auto* data = ImGui::GetDrawData();
  if (g_useSdlRenderer) {
    SDL_Renderer* renderer = window::get_sdl_renderer();
    SDL_RenderClear(renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(data, renderer);
    SDL_RenderPresent(renderer);
  } else if (data != nullptr) {
    pass.PushDebugGroup("Aurora: Dear Imgui");
    const float drawnWidth = data->DisplaySize.x * data->FramebufferScale.x;
    const float drawnHeight = data->DisplaySize.y * data->FramebufferScale.y;
    if (targetWidth != 0 && targetHeight != 0 && data->DisplaySize.x > 0.f && data->DisplaySize.y > 0.f &&
        (std::lround(drawnWidth) != static_cast<long>(targetWidth) ||
         std::lround(drawnHeight) != static_cast<long>(targetHeight))) {
      // The window was resized after this ImGui frame began: draw it scaled to the target for this one frame
      // (a shallow copy, as render_capture does) instead of with scissors outside the render area.
      ImDrawData scaled = *data;
      scaled.FramebufferScale = ImVec2(static_cast<float>(targetWidth) / data->DisplaySize.x,
                                       static_cast<float>(targetHeight) / data->DisplaySize.y);
      ImGui_ImplWGPU_RenderDrawData(&scaled, pass.Get());
    } else {
      ImGui_ImplWGPU_RenderDrawData(data, pass.Get());
    }
    pass.PopDebugGroup();
  }
}

void render_capture(const wgpu::RenderPassEncoder& pass, bool stampOnly, float scale) noexcept {
  ZoneScoped;
  render_frame_data();
  const ImDrawData* data = ImGui::GetDrawData();
  if (g_useSdlRenderer || data == nullptr) {
    return;
  }
  // A shallow copy holding a subset of the lists; the backend sizes its viewport and scissors from
  // DisplaySize * FramebufferScale, so the scale maps window coordinates onto the capture image.
  ImDrawData capture = *data;
  capture.CmdLists.clear();
  capture.TotalVtxCount = 0;
  capture.TotalIdxCount = 0;
  for (ImDrawList* list : data->CmdLists) {
    const char* owner = list->_OwnerName != nullptr ? list->_OwnerName : "";
    if (std::strcmp(owner, AURORA_CAPTURE_HIDDEN_WINDOW) == 0 ||
        (stampOnly && std::strcmp(owner, AURORA_CAPTURE_STAMP_WINDOW) != 0)) {
      continue;
    }
    capture.CmdLists.push_back(list);
    capture.TotalVtxCount += list->VtxBuffer.Size;
    capture.TotalIdxCount += list->IdxBuffer.Size;
  }
  capture.CmdListsCount = capture.CmdLists.Size;
  if (capture.CmdListsCount == 0) {
    return;
  }
  capture.FramebufferScale = ImVec2(data->FramebufferScale.x * scale, data->FramebufferScale.y * scale);
  pass.PushDebugGroup("Aurora: Dear Imgui capture");
  ImGui_ImplWGPU_RenderDrawData(&capture, pass.Get());
  pass.PopDebugGroup();
}

ImTextureID add_texture(uint32_t width, uint32_t height, const uint8_t* data) noexcept {
  if (SDL_Renderer* renderer = window::get_sdl_renderer()) {
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
    SDL_UpdateTexture(texture, nullptr, data, width * 4);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    g_sdlTextures.push_back(texture);
    return reinterpret_cast<ImTextureID>(texture);
  }
  const wgpu::Extent3D size{
      .width = width,
      .height = height,
      .depthOrArrayLayers = 1,
  };
  const wgpu::TextureDescriptor textureDescriptor{
      .label = "imgui texture",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .dimension = wgpu::TextureDimension::e2D,
      .size = size,
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .mipLevelCount = 1,
      .sampleCount = 1,
  };
  const wgpu::TextureViewDescriptor textureViewDescriptor{
      .label = "imgui texture view",
      .format = wgpu::TextureFormat::RGBA8Unorm,
      .dimension = wgpu::TextureViewDimension::e2D,
      .mipLevelCount = WGPU_MIP_LEVEL_COUNT_UNDEFINED,
      .arrayLayerCount = WGPU_ARRAY_LAYER_COUNT_UNDEFINED,
  };
  auto texture = webgpu::g_device.CreateTexture(&textureDescriptor);
  auto textureView = texture.CreateView(&textureViewDescriptor);
  {
    const wgpu::TexelCopyTextureInfo dstView{
        .texture = texture,
    };
    const wgpu::TexelCopyBufferLayout dataLayout{
        .bytesPerRow = 4 * width,
        .rowsPerImage = height,
    };
    webgpu::g_queue.WriteTexture(&dstView, data, width * height * 4, &dataLayout, &size);
  }
  g_wgpuTextures.push_back(texture);
  return reinterpret_cast<ImTextureID>(textureView.MoveToCHandle());
}
} // namespace aurora::imgui

// C bindings
extern "C" {
ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8) {
  return aurora::imgui::add_texture(width, height, static_cast<const uint8_t*>(rgba8));
}
}
