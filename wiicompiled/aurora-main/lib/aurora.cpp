#include <aurora/aurora.h>
#include <aurora/window_owner.hpp>

#ifdef AURORA_ENABLE_GX
#include "gfx/common.hpp"
#include "gfx/texture_replacement.hpp"
#include "gfx/efb_ram_copy.hpp"
#include "gx/fifo.hpp"
#include "gx/shader_info.hpp"
#include "imgui.hpp"
#include "webgpu/gpu.hpp"
#include "webgpu/metalfx.hpp"
#include <webgpu/webgpu_cpp.h>
#endif

#include "input.hpp"
#include "internal.hpp"
#include "window.hpp"

#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_thread.h>
#include <magic_enum.hpp>

#include "system_info.hpp"
#include "tracy/Tracy.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef AURORA_ENABLE_GX
namespace aurora::gx {
// Producer pacing feedback for the adaptive slot count, defined in lib/gx/shader_info.cpp;
// declared here so the C entry point at the bottom of this file can forward to it.
void report_producer_paced(bool paced) noexcept;
} // namespace aurora::gx
// Screenshot PNG writer, defined in lib/gfx/png_io.cpp.
extern "C" bool aurora_write_png_rgb(const char* path, const uint8_t* rgb, uint32_t width, uint32_t height);
#endif

namespace aurora {
AuroraConfig g_config;
uint32_t g_sdlCustomEventsStart;
char g_gameName[4];
std::atomic<AuroraFrameWorkerWaitCallback> g_frameWorkerWaitCallback{nullptr};
// Presentation schedule for the frame being sealed, set by the producer. Jobs carry absolute
// deadlines derived from it, so the presenter cannot drift. Zero means present when ready.
std::atomic<uint64_t> g_presentScheduleBaseNanos{0};
std::atomic<uint64_t> g_presentScheduleIntervalNanos{0};
std::atomic<bool> g_metalfxRequested{false};
std::atomic<bool> g_metalfxSupported{false};
std::atomic<AuroraMetalFXStatus> g_metalfxStatus{AURORA_METALFX_DISABLED};

namespace {
Module Log("aurora");

std::atomic<uint32_t> g_captureFrame{UINT32_MAX};
// g_captureFrame value for "the next presented frame" (screenshots).
constexpr uint32_t kCaptureNextFrame = UINT32_MAX - 1;
std::mutex g_captureMutex;  // guards the two below
std::string g_captureOutputPath;
bool g_captureScreenshot = false;  // PNG on a worker thread, result in g_screenshotResult
std::atomic<int> g_captureOverlay{AURORA_CAPTURE_OVERLAY_NONE};  // AuroraCaptureOverlay
// 0 = nothing new, 1 = saved, -1 = failed; read and cleared by aurora_take_screenshot_result.
std::atomic<int> g_screenshotResult{0};

using PresentClock = std::chrono::steady_clock;

struct PresentTimingSample {
  PresentClock::time_point presentedAt{};
  std::chrono::nanoseconds interval{};
  // Slot carried a copy of the native image instead of replayed interpolation, so the present
  // counts toward the rate but shows no new motion.
  bool duplicated = false;
};

std::mutex g_presentTimingMutex;
std::array<PresentTimingSample, 512> g_presentTimingSamples{};
size_t g_presentTimingWriteIndex = 0;
size_t g_presentTimingSampleCount = 0;
std::optional<PresentClock::time_point> g_lastSuccessfulPresent;
uint64_t g_totalPresentCount = 0;

void wait_until_precise(PresentClock::time_point deadline) noexcept {
  // Busy-spin tail covering the waitable timer's wakeup jitter. 150 us was not enough in
  // practice; 400 us absorbs the observed overshoot, about 1.6 ms of one core at 240 Hz.
  constexpr auto kSpinWindow = std::chrono::microseconds(400);
  auto now = PresentClock::now();
  if (now >= deadline) {
    return;
  }

  const auto timerDeadline = deadline - now > kSpinWindow ? deadline - kSpinWindow : deadline;
#if defined(_WIN32)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  struct HighResolutionTimer {
    HANDLE handle = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                             TIMER_MODIFY_STATE | SYNCHRONIZE);
    ~HighResolutionTimer() {
      if (handle != nullptr) {
        ::CloseHandle(handle);
      }
    }
  };
  static thread_local HighResolutionTimer timer;
  if (timer.handle != nullptr && timerDeadline > now) {
    const auto remaining100ns =
        std::chrono::duration_cast<std::chrono::duration<int64_t, std::ratio<1, 10000000>>>(
            timerDeadline - now);
    LARGE_INTEGER due{};
    due.QuadPart = -std::max<int64_t>(remaining100ns.count(), 1);
    if (::SetWaitableTimerEx(timer.handle, &due, 0, nullptr, nullptr, nullptr, 0) != FALSE) {
      ::WaitForSingleObject(timer.handle, INFINITE);
    }
  } else
#endif
  {
    std::this_thread::sleep_until(timerDeadline);
  }

  while (PresentClock::now() < deadline) {
#if defined(_WIN32)
    YieldProcessor();
#else
    std::this_thread::yield();
#endif
  }
}

void record_successful_present(bool, uint32_t,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds,
                               std::chrono::nanoseconds, bool duplicated,
                               uint32_t, std::chrono::nanoseconds) noexcept {
  const auto now = PresentClock::now();
  std::chrono::nanoseconds interval{};
  {
    std::lock_guard lock(g_presentTimingMutex);
    if (g_lastSuccessfulPresent) {
      interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - *g_lastSuccessfulPresent);
    }
    g_lastSuccessfulPresent = now;
    g_presentTimingSamples[g_presentTimingWriteIndex] = {
        .presentedAt = now,
        .interval = interval,
        .duplicated = duplicated,
    };
    g_presentTimingWriteIndex = (g_presentTimingWriteIndex + 1) % g_presentTimingSamples.size();
    g_presentTimingSampleCount = std::min(g_presentTimingSampleCount + 1, g_presentTimingSamples.size());
    ++g_totalPresentCount;
  }
}

AuroraPresentTiming snapshot_present_timing() noexcept {
  constexpr auto kWindow = std::chrono::seconds(1);
  const auto cutoff = PresentClock::now() - kWindow;
  std::vector<double> milliseconds;
  uint64_t totalPresentCount = 0;
  size_t newMotionSamples = 0;
  {
    std::lock_guard lock(g_presentTimingMutex);
    totalPresentCount = g_totalPresentCount;
    milliseconds.reserve(g_presentTimingSampleCount);
    for (size_t i = 0; i < g_presentTimingSampleCount; ++i) {
      const auto& sample = g_presentTimingSamples[i];
      if (sample.presentedAt >= cutoff && sample.interval.count() > 0) {
        milliseconds.push_back(
            std::chrono::duration<double, std::milli>(sample.interval).count());
        if (!sample.duplicated) {
          ++newMotionSamples;
        }
      }
    }
  }

  AuroraPresentTiming result{
      .totalPresentCount = totalPresentCount,
      .sampleCount = static_cast<uint32_t>(milliseconds.size()),
  };
  if (milliseconds.empty()) {
    return result;
  }

  double totalMilliseconds = 0.0;
  for (const double value : milliseconds) {
    totalMilliseconds += value;
  }
  result.averageFrameTimeMs = totalMilliseconds / static_cast<double>(milliseconds.size());
  result.framesPerSecond = result.averageFrameTimeMs > 0.0 ? 1000.0 / result.averageFrameTimeMs : 0.0;
  // Duplicated presentation slots keep the presented cadence but carry no new
  // motion; scale them out so this reads as the rate the eye actually sees.
  result.effectiveFramesPerSecond =
      result.framesPerSecond *
      (static_cast<double>(newMotionSamples) / static_cast<double>(milliseconds.size()));
  for (const double value : milliseconds) {
    const double difference = value - result.averageFrameTimeMs;
    result.jitterMs += difference * difference;
  }
  result.jitterMs = std::sqrt(result.jitterMs / static_cast<double>(milliseconds.size()));
  std::sort(milliseconds.begin(), milliseconds.end());
  result.p95FrameTimeMs =
      milliseconds[static_cast<size_t>(std::floor(static_cast<double>(milliseconds.size() - 1) * 0.95))];
  return result;
}

// ImGui draw data lives in the shared context, so starting the next ImGui frame destroys draw
// lists the sealed slots still replay. That is why ImGui callers wait for DONE, not SEALED.
enum class ImGuiFramePolicy {
  // Start ImGui's next frame as part of renderer preparation, as the
  // synchronous path always has.
  Immediate,
  // Leave it unstarted; the caller owes an imgui::new_frame() once it has
  // finished replaying this frame's draw data.
  Deferred,
};

bool begin_frame_impl(bool pumpEvents, ImGuiFramePolicy imguiPolicy = ImGuiFramePolicy::Immediate,
                      bool* imguiNewFrameOwed = nullptr);
bool begin_frame_render_state_impl(ImGuiFramePolicy imguiPolicy, bool* imguiNewFrameOwed);
void end_frame_impl(bool pumpEvents, bool drainFifo);

// The two publication points of a frame-worker cycle, cleared together under `mutex`. Sealed:
// producer-shared renderer state is free again. Done: slots encoded, presented, ImGui restarted.
enum class FrameWorkerPhase {
  Sealed,
  Done,
};

struct FrameWorkerState {
  std::mutex mutex;
  std::condition_variable cv;
  std::thread thread;
  std::thread::id threadId{};
  bool started = false;
  bool stop = false;
  bool jobPending = false;
  // Readiness is polled thousands of times per frame, so these flags double as a publication
  // barrier. `sealed` is released before `ready`, and both are cleared under `mutex`.
  std::atomic_bool sealed{true};
  std::atomic_bool ready{true};
  bool framePrepared = false;
  bool prepareAllowed = false;
};

FrameWorkerState g_frameWorker;

bool frame_worker_requested() noexcept {
#ifdef AURORA_ENABLE_GX
  static const bool enabled = [] {
#if defined(__APPLE__)
    // ImGui's SDL backend may raise an SDL window from ImGui::NewFrame(). On
    // macOS that reaches AppKit, whose window operations are main-thread-only;
    // doing it on the frame worker terminates the process with EXC_BREAKPOINT.
    // Keep all SDL/ImGui work on the calling thread until the worker no longer
    // owns frame preparation on Apple platforms.
    return false;
#endif
#if defined(_WIN32)
    // RenderDoc's D3D12 layer is injected before Aurora starts and needs device and command
    // ownership on one thread, so keep frame submission synchronous there.
    if (::GetModuleHandleW(L"renderdoc.dll") != nullptr) {
      return false;
    }
#endif
    return true;
  }();
#if defined(_WIN32)
  static const bool renderDocLoaded = ::GetModuleHandleW(L"renderdoc.dll") != nullptr;
  if (renderDocLoaded) {
    static const bool logged = [] {
      Log.info("Disabled asynchronous frame submission worker for RenderDoc capture");
      return true;
    }();
    (void)logged;
  }
#endif
  return enabled;
#else
  return false;
#endif
}

#ifdef AURORA_ENABLE_GX
// Returns false when a stop request was observed mid-cycle.
bool run_frame_worker_cycle(gfx::SealedFrame& sealedFrame);
#endif

void frame_worker_main() noexcept {
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.threadId = std::this_thread::get_id();
  }

#ifdef AURORA_ENABLE_GX
  // Owned by the worker for its whole lifetime so the sealed pass vector and
  // its pooled command lists keep their capacity across frames.
  gfx::SealedFrame sealedFrame;
#endif

  for (;;) {
    {
      std::unique_lock lock(g_frameWorker.mutex);
      g_frameWorker.cv.wait(lock, [] { return g_frameWorker.stop || g_frameWorker.jobPending; });
      if (g_frameWorker.stop) {
        break;
      }
      g_frameWorker.jobPending = false;
    }

    // The CPU already decoded the sealed frame at its GX boundary; the worker only owns
    // encode/submit/present, so it never touches the producer's next FIFO buffer.
#ifdef AURORA_ENABLE_GX
    try {
      if (!run_frame_worker_cycle(sealedFrame)) break;
    } catch (...) {
      if (!window_owner::enabled()) std::terminate();
      window_owner::report_failure(std::current_exception());
      std::lock_guard lock(g_frameWorker.mutex);
      g_frameWorker.stop = true;
      g_frameWorker.framePrepared = false;
      break;
    }
#endif
  }

  // A stop request can unblock either wait above mid-cycle, so release both phases before
  // stop_frame_worker() joins.
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.sealed.store(true, std::memory_order_release);
    g_frameWorker.ready.store(true, std::memory_order_release);
  }
  g_frameWorker.cv.notify_all();
}

void ensure_frame_worker_started() noexcept {
  if (!frame_worker_requested()) {
    return;
  }
  std::lock_guard lock(g_frameWorker.mutex);
  if (g_frameWorker.started) {
    return;
  }
  g_frameWorker.stop = false;
  g_frameWorker.jobPending = false;
  g_frameWorker.sealed.store(true, std::memory_order_release);
  g_frameWorker.ready.store(true, std::memory_order_release);
  g_frameWorker.prepareAllowed = false;
  g_frameWorker.started = true;
  g_frameWorker.thread = std::thread(frame_worker_main);
  Log.info("Enabled bounded asynchronous frame submission worker");
}

bool frame_worker_phase_reached(FrameWorkerPhase phase) noexcept {
  return phase == FrameWorkerPhase::Sealed ? g_frameWorker.sealed.load(std::memory_order_acquire)
                                           : g_frameWorker.ready.load(std::memory_order_acquire);
}

bool wait_for_frame_worker_private_for(FrameWorkerPhase phase, std::chrono::microseconds timeout);

void wait_for_frame_worker_private(FrameWorkerPhase phase) {
  constexpr auto kWaitServiceInterval = std::chrono::milliseconds(1);
  while (!wait_for_frame_worker_private_for(phase, kWaitServiceInterval)) {
  }
}

bool wait_for_frame_worker_private_for(FrameWorkerPhase phase, std::chrono::microseconds timeout) {
  // Each phase's state is published before its flag is set, which keeps the common
  // display-list path out of the worker control mutex.
  if (frame_worker_phase_reached(phase)) {
    return true;
  }

  std::unique_lock lock(g_frameWorker.mutex);
  if (!g_frameWorker.started || g_frameWorker.threadId == std::this_thread::get_id()) {
    return true;
  }
  if (timeout <= std::chrono::microseconds::zero()) {
    return frame_worker_phase_reached(phase);
  }

  const bool reached =
      g_frameWorker.cv.wait_for(lock, timeout, [phase] { return frame_worker_phase_reached(phase); });
  if (reached) {
    return true;
  }

  lock.unlock();
  // Both phases service the guest's alarm/retrace pump identically; the
  // producer must keep its own timing alive however long it waits.
  if (const auto callback = g_frameWorkerWaitCallback.load(std::memory_order_acquire)) {
    callback();
  }
  return false;
}

void stop_frame_worker() noexcept {
  {
    std::lock_guard lock(g_frameWorker.mutex);
    if (!g_frameWorker.started) {
      return;
    }
    g_frameWorker.stop = true;
    g_frameWorker.prepareAllowed = true;
  }
  g_frameWorker.cv.notify_all();
  if (g_frameWorker.thread.joinable()) {
    g_frameWorker.thread.join();
  }
  std::lock_guard lock(g_frameWorker.mutex);
  g_frameWorker.started = false;
  g_frameWorker.threadId = {};
  g_frameWorker.framePrepared = false;
}

uint32_t align_to(uint32_t value, uint32_t alignment) noexcept {
  return (value + alignment - 1) & ~(alignment - 1);
}

void append_u16(std::ofstream& out, uint16_t value) {
  const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8)};
  out.write(bytes, sizeof(bytes));
}

void append_u32(std::ofstream& out, uint32_t value) {
  const char bytes[] = {static_cast<char>(value), static_cast<char>(value >> 8),
                        static_cast<char>(value >> 16), static_cast<char>(value >> 24)};
  out.write(bytes, sizeof(bytes));
}

bool write_bmp(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height,
               uint32_t bytesPerRow, bool bgra) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  constexpr uint32_t pixelOffset = 14 + 40;
  const uint32_t imageSize = width * height * 4;
  out.write("BM", 2);
  append_u32(out, pixelOffset + imageSize);
  append_u16(out, 0);
  append_u16(out, 0);
  append_u32(out, pixelOffset);
  append_u32(out, 40);
  append_u32(out, width);
  append_u32(out, height);
  append_u16(out, 1);
  append_u16(out, 32);
  append_u32(out, 0);
  append_u32(out, imageSize);
  append_u32(out, 2835);
  append_u32(out, 2835);
  append_u32(out, 0);
  append_u32(out, 0);
  std::vector<uint8_t> row(width * 4);
  for (uint32_t y = height; y-- > 0;) {
    const auto* source = pixels + static_cast<size_t>(y) * bytesPerRow;
    if (bgra) {
      out.write(reinterpret_cast<const char*>(source), row.size());
    } else {
      for (uint32_t x = 0; x < width; ++x) {
        row[x * 4 + 0] = source[x * 4 + 2];
        row[x * 4 + 1] = source[x * 4 + 1];
        row[x * 4 + 2] = source[x * 4 + 0];
        row[x * 4 + 3] = source[x * 4 + 3];
      }
      out.write(reinterpret_cast<const char*>(row.data()), row.size());
    }
  }
  return out.good();
}

#ifdef AURORA_ENABLE_GX
// GPU
using webgpu::g_device;
using webgpu::g_instance;
using webgpu::g_queue;
using webgpu::g_surface;
std::recursive_mutex g_rendererGpuMutex;
std::mutex g_queueSubmitMutex;
// Surface ownership, held apart from the renderer mutex: a presentation slot touches the surface,
// its image and the queue, nothing recorded. Lock order is surface then renderer.
std::mutex g_surfaceMutex;
std::atomic<bool> g_surfaceReconfigurePending{false};
std::atomic<bool> g_surfaceRecreatePending{false};

void request_surface_reconfigure() noexcept {
  g_surfaceReconfigurePending.store(true, std::memory_order_release);
}

void request_surface_recreate() noexcept {
  g_surfaceRecreatePending.store(true, std::memory_order_release);
  g_surfaceReconfigurePending.store(true, std::memory_order_release);
}

struct PendingFrameCapture {
  wgpu::Buffer buffer;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bytesPerRow = 0;
  uint64_t bufferSize = 0;
  bool bgra = false;
  bool screenshot = false;
  std::string path;
};

// FrightWorks captures with the overlay choice (aurora_set_capture_overlay) and video frames (aurora_set_video_capture):
// the window's picture is drawn again into a capture texture (the game scaled as on screen, then a subset of this
// frame's ImGui lists) and copied into a buffer that is read back.
struct CaptureTarget {
  webgpu::TextureWithSampler texture;
  uint32_t width = 0;
  uint32_t height = 0;
};

bool capture_format_supported(wgpu::TextureFormat format) noexcept {
  return format == wgpu::TextureFormat::BGRA8Unorm || format == wgpu::TextureFormat::BGRA8UnormSrgb ||
         format == wgpu::TextureFormat::RGBA8Unorm || format == wgpu::TextureFormat::RGBA8UnormSrgb;
}

bool capture_format_bgra(wgpu::TextureFormat format) noexcept {
  return format == wgpu::TextureFormat::BGRA8Unorm || format == wgpu::TextureFormat::BGRA8UnormSrgb;
}

// scale = width / the window's framebuffer width (ImGui's coordinates are the window's).
void encode_capture_image(const wgpu::CommandEncoder& encoder, const webgpu::PresentSource& source,
                          CaptureTarget& target, uint32_t width, uint32_t height, float scale, bool stampOnly) {
  if (!target.texture.texture || target.width != width || target.height != height) {
    target.texture = webgpu::create_render_texture(width, height, false);
    target.width = width;
    target.height = height;
  }
  const uint32_t targetWidth = target.texture.size.width;
  const uint32_t targetHeight = target.texture.size.height;
  auto viewport =
      webgpu::calculate_present_viewport(targetWidth, targetHeight, source.size.width, source.size.height);
  float presentAspect = 0.f;
  if (window::get_present_aspect_ratio(presentAspect)) {
    viewport = webgpu::calculate_present_viewport_for_aspect(targetWidth, targetHeight, presentAspect);
  }
  const wgpu::RenderPassColorAttachment attachment{
      .view = target.texture.view,
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
  };
  const wgpu::RenderPassDescriptor descriptor{
      .label = "Capture image pass",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
  };
  const auto pass = encoder.BeginRenderPass(&descriptor);
  pass.SetPipeline(webgpu::g_CopyPipeline);
  pass.SetBindGroup(0, source.bindGroup, 0, nullptr);
  pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height, viewport.znear, viewport.zfar);
  pass.Draw(3);
  pass.SetViewport(0.f, 0.f, static_cast<float>(targetWidth), static_cast<float>(targetHeight), 0.f, 1.f);
  imgui::render_capture(pass, stampOnly, scale);
  pass.End();
}

CaptureTarget g_screenshotTarget;
CaptureTarget g_videoTarget;

int64_t unix_micros_now() noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// Video readback slots: Free -> Copied (copy recorded) -> Mapping (after submit) -> callback -> Free.
struct VideoSlot {
  enum class State { Free, Copied, Mapping };
  wgpu::Buffer buffer;
  uint64_t bufferSize = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t bytesPerRow = 0;
  bool bgra = false;
  int64_t unixMicros = 0;
  State state = State::Free;
};
std::mutex g_videoMutex;  // guards everything below; held while the callback runs
AuroraVideoFrameCallback g_videoCallback = nullptr;
void* g_videoUser = nullptr;
uint32_t g_videoMaxHeight = 1080;
std::array<VideoSlot, 4> g_videoSlots;
std::vector<uint8_t> g_videoPacked;

void encode_video_frame(const wgpu::CommandEncoder& encoder, const webgpu::PresentSource& source,
                        uint32_t windowWidth, uint32_t windowHeight) {
  if (!source.texture || windowWidth == 0 || windowHeight == 0) return;
  VideoSlot* slot = nullptr;
  uint32_t maxHeight = 0;
  {
    std::lock_guard lock(g_videoMutex);
    if (g_videoCallback == nullptr) return;
    for (auto& candidate : g_videoSlots) {
      if (candidate.state == VideoSlot::State::Free) {
        slot = &candidate;
        break;
      }
    }
    maxHeight = g_videoMaxHeight;
  }
  if (slot == nullptr) return;  // the readback is behind: skip this frame
  const auto format = webgpu::g_graphicsConfig.surfaceConfiguration.format;
  if (!capture_format_supported(format)) return;
  const float scale = windowHeight > maxHeight ? static_cast<float>(maxHeight) / static_cast<float>(windowHeight) : 1.f;
  const uint32_t width = (std::max)(2u, static_cast<uint32_t>(static_cast<float>(windowWidth) * scale) & ~1u);
  const uint32_t height = (std::max)(2u, static_cast<uint32_t>(static_cast<float>(windowHeight) * scale) & ~1u);
  const bool stampOnly = g_captureOverlay.load(std::memory_order_acquire) != AURORA_CAPTURE_OVERLAY_ALL;
  encode_capture_image(encoder, source, g_videoTarget, width, height, scale, stampOnly);
  const auto& texture = g_videoTarget.texture;
  const uint32_t bytesPerRow = align_to(texture.size.width * 4, 256);
  const uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * texture.size.height;
  if (!slot->buffer || slot->bufferSize != bufferSize) {
    const wgpu::BufferDescriptor descriptor{
        .label = "Video frame readback",
        .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
        .size = bufferSize,
    };
    slot->buffer = g_device.CreateBuffer(&descriptor);
    slot->bufferSize = bufferSize;
  }
  const wgpu::TexelCopyTextureInfo sourceInfo{
      .texture = texture.texture,
      .mipLevel = 0,
      .origin = {0, 0, 0},
      .aspect = wgpu::TextureAspect::All,
  };
  const wgpu::TexelCopyBufferInfo destinationInfo{
      .layout = {.offset = 0, .bytesPerRow = bytesPerRow, .rowsPerImage = texture.size.height},
      .buffer = slot->buffer,
  };
  encoder.CopyTextureToBuffer(&sourceInfo, &destinationInfo, &texture.size);
  std::lock_guard lock(g_videoMutex);
  slot->width = texture.size.width;
  slot->height = texture.size.height;
  slot->bytesPerRow = bytesPerRow;
  slot->bgra = capture_format_bgra(format);
  slot->unixMicros = unix_micros_now();
  slot->state = VideoSlot::State::Copied;
}

void complete_video_slot(size_t index, wgpu::MapAsyncStatus status) {
  std::lock_guard lock(g_videoMutex);
  auto& slot = g_videoSlots[index];
  if (status == wgpu::MapAsyncStatus::Success) {
    const auto* mapped = static_cast<const uint8_t*>(slot.buffer.GetConstMappedRange(0, slot.bufferSize));
    if (mapped != nullptr && g_videoCallback != nullptr) {
      const uint32_t rowBytes = slot.width * 4;
      const uint8_t* pixels = mapped;
      if (slot.bytesPerRow != rowBytes) {
        g_videoPacked.resize(static_cast<size_t>(rowBytes) * slot.height);
        for (uint32_t y = 0; y < slot.height; ++y) {
          std::memcpy(g_videoPacked.data() + static_cast<size_t>(y) * rowBytes,
                      mapped + static_cast<size_t>(y) * slot.bytesPerRow, rowBytes);
        }
        pixels = g_videoPacked.data();
      }
      g_videoCallback(pixels, slot.width, slot.height, slot.bgra, slot.unixMicros, g_videoUser);
    }
    slot.buffer.Unmap();
  } else if (status != wgpu::MapAsyncStatus::CallbackCancelled && status != wgpu::MapAsyncStatus::Aborted) {
    Log.warn("Video frame readback failed: {}", magic_enum::enum_name(status));
  }
  slot.state = VideoSlot::State::Free;
}

// After the submit that holds the copies: start reading them back. Completion runs on whatever thread Dawn reports
// it from (AllowSpontaneous), at the latest in the next ProcessEvents.
void start_video_readbacks() {
  std::vector<size_t> indices;
  {
    std::lock_guard lock(g_videoMutex);
    for (size_t i = 0; i < g_videoSlots.size(); ++i) {
      if (g_videoSlots[i].state == VideoSlot::State::Copied) {
        g_videoSlots[i].state = VideoSlot::State::Mapping;
        indices.push_back(i);
      }
    }
  }
  for (const size_t i : indices) {
    g_videoSlots[i].buffer.MapAsync(wgpu::MapMode::Read, 0, g_videoSlots[i].bufferSize,
                                    wgpu::CallbackMode::AllowSpontaneous,
                                    [i](wgpu::MapAsyncStatus status, wgpu::StringView) { complete_video_slot(i, status); });
  }
}

std::optional<PendingFrameCapture> encode_frame_capture(const wgpu::CommandEncoder& encoder,
                                                        const webgpu::PresentSource& source, uint32_t windowWidth,
                                                        uint32_t windowHeight) {
  uint32_t requestedFrame = g_captureFrame.load(std::memory_order_acquire);
  const uint32_t currentFrame = gfx::current_frame();
  if (requestedFrame == kCaptureNextFrame) requestedFrame = currentFrame;
  if (requestedFrame == UINT32_MAX || currentFrame < requestedFrame) return std::nullopt;
  g_captureFrame.store(UINT32_MAX, std::memory_order_release);
  std::string path;
  bool screenshot = false;
  {
    std::lock_guard lock(g_captureMutex);
    path = g_captureOutputPath;
    screenshot = g_captureScreenshot;
  }
  if (currentFrame != requestedFrame) {
    Log.error("Missed requested frame capture {} (current frame {})", requestedFrame, currentFrame);
    return std::nullopt;
  }
  if (!source.texture || source.size.width == 0 || source.size.height == 0) {
    Log.error("Frame {} capture has no present-source texture", currentFrame);
    if (screenshot) g_screenshotResult.store(-1, std::memory_order_release);
    return std::nullopt;
  }
  // A screenshot with an overlay choice is the window's picture; otherwise the game frame at its render size.
  wgpu::Texture texture = source.texture;
  wgpu::Extent3D size = source.size;
  wgpu::TextureFormat format = source.format;
  const auto overlay = static_cast<AuroraCaptureOverlay>(g_captureOverlay.load(std::memory_order_acquire));
  if (screenshot && overlay != AURORA_CAPTURE_OVERLAY_NONE && windowWidth != 0 && windowHeight != 0 &&
      capture_format_supported(webgpu::g_graphicsConfig.surfaceConfiguration.format)) {
    encode_capture_image(encoder, source, g_screenshotTarget, windowWidth, windowHeight, 1.f,
                         overlay == AURORA_CAPTURE_OVERLAY_STAMP);
    texture = g_screenshotTarget.texture.texture;
    size = g_screenshotTarget.texture.size;
    format = g_screenshotTarget.texture.format;
  }
  const bool bgra = capture_format_bgra(format);
  if (!capture_format_supported(format)) {
    Log.error("Frame {} capture does not support texture format {}", currentFrame, magic_enum::enum_name(format));
    if (screenshot) g_screenshotResult.store(-1, std::memory_order_release);
    return std::nullopt;
  }
  const uint32_t bytesPerRow = align_to(size.width * 4, 256);
  const uint64_t bufferSize = static_cast<uint64_t>(bytesPerRow) * size.height;
  const wgpu::BufferDescriptor descriptor{
      .label = "Visual validation frame capture",
      .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
      .size = bufferSize,
  };
  auto buffer = g_device.CreateBuffer(&descriptor);
  const wgpu::TexelCopyTextureInfo sourceInfo{
      .texture = texture,
      .mipLevel = 0,
      .origin = {0, 0, 0},
      .aspect = wgpu::TextureAspect::All,
  };
  const wgpu::TexelCopyBufferInfo destinationInfo{
      .layout = {.offset = 0, .bytesPerRow = bytesPerRow, .rowsPerImage = size.height},
      .buffer = buffer,
  };
  encoder.CopyTextureToBuffer(&sourceInfo, &destinationInfo, &size);
  return PendingFrameCapture{
      .buffer = std::move(buffer),
      .width = size.width,
      .height = size.height,
      .bytesPerRow = bytesPerRow,
      .bufferSize = bufferSize,
      .bgra = bgra,
      .screenshot = screenshot,
      .path = std::move(path),
  };
}

void complete_frame_capture(PendingFrameCapture& capture) {
  wgpu::MapAsyncStatus mapStatus = wgpu::MapAsyncStatus::CallbackCancelled;
  wgpu::StringView mapMessage{};
  const auto future = capture.buffer.MapAsync(
      wgpu::MapMode::Read, 0, capture.bufferSize, wgpu::CallbackMode::WaitAnyOnly,
      [&mapStatus, &mapMessage](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        mapStatus = status;
        mapMessage = message;
      });
  const auto waitStatus = g_instance.WaitAny(future, 5000000000);
  if (waitStatus != wgpu::WaitStatus::Success || mapStatus != wgpu::MapAsyncStatus::Success) {
    Log.error("Frame capture readback failed wait={} map={} message={}", magic_enum::enum_name(waitStatus),
              magic_enum::enum_name(mapStatus), mapMessage);
    if (capture.screenshot) g_screenshotResult.store(-1, std::memory_order_release);
    return;
  }
  const auto* pixels = static_cast<const uint8_t*>(capture.buffer.GetConstMappedRange(0, capture.bufferSize));
  if (capture.screenshot) {
    // Copy out as opaque RGB and encode the PNG off the presentation thread.
    std::vector<uint8_t> rgb(static_cast<size_t>(capture.width) * capture.height * 3);
    const int r = capture.bgra ? 2 : 0, b = capture.bgra ? 0 : 2;
    for (uint32_t y = 0; y < capture.height; ++y) {
      const uint8_t* src = pixels + static_cast<size_t>(y) * capture.bytesPerRow;
      uint8_t* dst = rgb.data() + static_cast<size_t>(y) * capture.width * 3;
      for (uint32_t x = 0; x < capture.width; ++x, src += 4, dst += 3) {
        dst[0] = src[r];
        dst[1] = src[1];
        dst[2] = src[b];
      }
    }
    capture.buffer.Unmap();
    std::thread([rgb = std::move(rgb), path = capture.path, width = capture.width, height = capture.height] {
      const bool ok = aurora_write_png_rgb(path.c_str(), rgb.data(), width, height);
      if (ok) {
        Log.info("Screenshot saved to '{}' ({}x{})", path, width, height);
      } else {
        Log.error("Failed to write screenshot '{}'", path);
      }
      g_screenshotResult.store(ok ? 1 : -1, std::memory_order_release);
    }).detach();
    return;
  }
  if (write_bmp(capture.path.c_str(), pixels, capture.width, capture.height, capture.bytesPerRow, capture.bgra)) {
    Log.info("Captured rendered frame to '{}' ({}x{})", capture.path, capture.width, capture.height);
  } else {
    Log.error("Failed to write rendered frame capture to '{}'", capture.path);
  }
  capture.buffer.Unmap();
}

// ----- Motion-vector dump (AURORA_MV_DUMP_DIR): the EFB color and its motion vectors every
// AURORA_MV_DUMP_INTERVAL frames (default 60) from frame AURORA_MV_DUMP_START (default 0), up to
// AURORA_MV_DUMP_MAX dumps (default 30). With AURORA_MV_DUMP_TRIGGER=<file>, nothing is dumped until that file
// appears; it is then deleted and the count restarts from that frame (touch it again for another series).
// Writes efb_<frame>.png, mv_<frame>.png (grey = still, hue = direction, saturation = speed, full at 16 px)
// and mv_<frame>.rg16f (raw: u32 width, u32 height, then RG half floats per pixel), and logs statistics.
struct PendingMotionDump {
  wgpu::Buffer motion;
  wgpu::Buffer color;
  wgpu::Buffer exported;  // the export texture as last presented (RG32F), when the export is on
  uint32_t exportWidth = 0;
  uint32_t exportHeight = 0;
  uint32_t exportPitch = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t motionPitch = 0;
  uint32_t colorPitch = 0;
  uint64_t motionSize = 0;
  uint64_t colorSize = 0;
  bool bgra = false;
  uint32_t frame = 0;
  std::string dir;
};

float half_to_float(uint16_t h) noexcept {
  const uint32_t sign = (h & 0x8000u) << 16;
  const uint32_t exponent = (h >> 10) & 0x1Fu;
  uint32_t mantissa = h & 0x3FFu;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      int e = -1;
      do { ++e; mantissa <<= 1; } while ((mantissa & 0x400u) == 0);
      bits = sign | static_cast<uint32_t>(127 - 15 - e) << 23 | (mantissa & 0x3FFu) << 13;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | mantissa << 13;
  } else {
    bits = sign | (exponent + 127 - 15) << 23 | mantissa << 13;
  }
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// ----- Display-space motion vectors and the motion-vector view.
// The game presents a display copy of part of the EFB (480 of its 528 lines here), scaled; the display-space texture
// holds, for each presented pixel, its motion in presented pixels (prev = pos + motion, y down): what a temporal
// upscaler or frame generator that sees the presented picture needs. The view draws either as color instead of the
// picture.
std::atomic_int g_motionViewMode{-1};  // -1 = not read from AURORA_MV_VIEW yet
std::atomic<float> g_motionViewScale{16.f};
struct MotionPass {
  wgpu::RenderPipeline pipeline;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  webgpu::TextureWithSampler target;
  wgpu::Buffer params;
};
MotionPass g_displayMotionPass;
MotionPass g_motionViewPass;

int motion_view_mode() noexcept {
  int mode = g_motionViewMode.load(std::memory_order_acquire);
  if (mode < 0) {
    const char* env = std::getenv("AURORA_MV_VIEW");
    int expected = -1;
    g_motionViewMode.compare_exchange_strong(expected, env != nullptr ? std::clamp(std::atoi(env), 0, 2) : 0,
                                             std::memory_order_acq_rel);
    mode = g_motionViewMode.load(std::memory_order_acquire);
  }
  return mode;
}

constexpr const char* kFullscreenTriangleWgsl = R"""(
@vertex
fn vs_main(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = array(vec2f(-1.0, 1.0), vec2f(-1.0, -3.0), vec2f(3.0, 1.0));
    return vec4f(p[i], 0.0, 1.0);
}
)""";

// Creates the pass's pipeline (for `format`), parameter buffer and `width` x `height` target when missing or stale.
void prepare_motion_pass(MotionPass& pass, const char* label, const std::string& fragmentWgsl, wgpu::TextureFormat format,
                         uint32_t width, uint32_t height, bool sampled) {
  if (!pass.pipeline || pass.format != format) {
    const std::string code = std::string(kFullscreenTriangleWgsl) + fragmentWgsl;
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = code.c_str();
    const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = label};
    const auto module = g_device.CreateShaderModule(&moduleDescriptor);
    const wgpu::ColorTargetState target{.format = format};
    const wgpu::FragmentState fragment{.module = module, .entryPoint = "fs_main", .targetCount = 1, .targets = &target};
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = label,
        .vertex = {.module = module, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .fragment = &fragment,
    };
    pass.pipeline = g_device.CreateRenderPipeline(&descriptor);
    pass.format = format;
    pass.target = {};
    const wgpu::BufferDescriptor params{
        .label = label,
        .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
        .size = sizeof(std::array<float, 12>),
    };
    pass.params = g_device.CreateBuffer(&params);
  }
  auto& target = pass.target;
  if (width != 0 && (!target.texture || target.size.width != width || target.size.height != height)) {
    const wgpu::TextureDescriptor descriptor{
        .label = label,
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopySrc,
        .size = {width, height, 1},
        .format = format,
    };
    target.texture = g_device.CreateTexture(&descriptor);
    target.view = target.texture.CreateView();
    target.size = descriptor.size;
    target.format = format;
    if (sampled) {
      const wgpu::SamplerDescriptor sampler{
          .addressModeU = wgpu::AddressMode::ClampToEdge,
          .addressModeV = wgpu::AddressMode::ClampToEdge,
          .magFilter = wgpu::FilterMode::Linear,
          .minFilter = wgpu::FilterMode::Linear,
      };
      target.sampler = g_device.CreateSampler(&sampler);
    }
  }
}

// Every pass's parameters: up to 12 floats (3 vec4f), padded with zeros.
using MotionPassParams = std::array<float, 12>;

// Bindings: input0, input1, the parameters, then `more` from binding 3 on.
void run_motion_pass(const wgpu::CommandEncoder& encoder, MotionPass& pass, const MotionPassParams& params,
                     wgpu::TextureView input0, wgpu::TextureView input1, const wgpu::TextureView* target = nullptr,
                     std::initializer_list<wgpu::TextureView> more = {}) {
  g_queue.WriteBuffer(pass.params, 0, params.data(), sizeof(params));
  std::vector<wgpu::BindGroupEntry> entries{
      wgpu::BindGroupEntry{.binding = 0, .textureView = std::move(input0)},
      wgpu::BindGroupEntry{.binding = 1, .textureView = std::move(input1)},
      wgpu::BindGroupEntry{.binding = 2, .buffer = pass.params, .size = sizeof(params)},
  };
  for (const auto& view : more) {
    entries.push_back(wgpu::BindGroupEntry{.binding = static_cast<uint32_t>(entries.size()), .textureView = view});
  }
  const wgpu::BindGroupDescriptor bindGroupDescriptor{
      .layout = pass.pipeline.GetBindGroupLayout(0),
      .entryCount = entries.size(),
      .entries = entries.data(),
  };
  const auto bindGroup = g_device.CreateBindGroup(&bindGroupDescriptor);
  const wgpu::RenderPassColorAttachment attachment{
      .view = target != nullptr ? *target : pass.target.view,
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
      .clearValue = {0.0, 0.0, 0.0, 1.0},
  };
  const wgpu::RenderPassDescriptor passDescriptor{.colorAttachmentCount = 1, .colorAttachments = &attachment};
  const auto renderPass = encoder.BeginRenderPass(&passDescriptor);
  renderPass.SetPipeline(pass.pipeline);
  renderPass.SetBindGroup(0, bindGroup);
  renderPass.Draw(3);
  renderPass.End();
}

// This frame's motion vectors in presented pixels, the size of `source` (nullptr without motion vectors).
const webgpu::TextureWithSampler* encode_display_motion(const wgpu::CommandEncoder& encoder,
                                                        const webgpu::PresentSource& source) {
  const auto& motion = webgpu::g_motionVectors;
  if (!gfx::motion_vectors_active() || !motion.texture || !source.texture || source.size.width == 0 ||
      source.size.height == 0) {
    return nullptr;
  }
  auto rect = source.efbRect;
  if (rect[2] <= 0.f || rect[3] <= 0.f) {
    rect = {0.f, 0.f, static_cast<float>(motion.size.width), static_cast<float>(motion.size.height)};
  }
  static std::array<float, 6> s_lastMapping{};
  const std::array<float, 6> mapping{rect[0], rect[1], rect[2], rect[3], static_cast<float>(source.size.width),
                                     static_cast<float>(source.size.height)};
  if (mapping != s_lastMapping) {
    s_lastMapping = mapping;
    Log.info("Display motion vectors: EFB ({}, {}) {}x{} of {}x{} -> picture {}x{}", rect[0], rect[1], rect[2],
             rect[3], motion.size.width, motion.size.height, source.size.width, source.size.height);
  }
  prepare_motion_pass(g_displayMotionPass, "Display motion vectors", R"""(
struct Params {
    rect: vec4f,      // EFB rectangle the picture was copied from (render pixels)
    size: vec2f,      // presented picture size
    pad: vec2f,
};
@group(0) @binding(0) var motion: texture_2d<f32>;
@group(0) @binding(1) var unused: texture_2d<f32>;
@group(0) @binding(2) var<uniform> params: Params;

@fragment
fn fs_main(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let toEfb = params.rect.zw / params.size;
    let efb = params.rect.xy + pos.xy * toEfb;
    let limit = vec2f(textureDimensions(motion)) - 1.0;
    let mv = textureLoad(motion, vec2i(clamp(floor(efb), vec2f(0.0), limit)), 0).xy;
    _ = textureDimensions(unused);
    return vec4f(mv / toEfb, 0.0, 0.0);
}
)""", wgpu::TextureFormat::RG16Float, source.size.width, source.size.height, false);
  run_motion_pass(encoder, g_displayMotionPass,
                  {rect[0], rect[1], rect[2], rect[3], static_cast<float>(source.size.width),
                   static_cast<float>(source.size.height), 0.f, 0.f},
                  motion.view, motion.view);
  return &g_displayMotionPass.target;
}

// The view (motion_view_mode() != 0): the present source to use instead of `source`.
webgpu::PresentSource encode_motion_view(const wgpu::CommandEncoder& encoder, const webgpu::PresentSource& source,
                                         const webgpu::TextureWithSampler* displayMotion) {
  const int mode = motion_view_mode();
  if (mode == 0 || displayMotion == nullptr) {
    return source;
  }
  prepare_motion_pass(g_motionViewPass, "Motion vector view", R"""(
struct Params {
    mode: f32,
    scale: f32,
    pad: vec2f,
    pad2: vec4f,
};
@group(0) @binding(0) var motion: texture_2d<f32>;
@group(0) @binding(1) var picture: texture_2d<f32>;
@group(0) @binding(2) var<uniform> params: Params;

fn hue(h: f32) -> vec3f {
    return clamp(abs(fract(vec3f(h) - vec3f(0.0, 1.0 / 3.0, 2.0 / 3.0)) * 2.0 - 1.0) * 3.0 - 1.0, vec3f(0.0), vec3f(1.0));
}

@fragment
fn fs_main(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    let xy = vec2i(pos.xy);
    let mv = textureLoad(motion, xy, 0).xy;
    let base = select(vec3f(0.5), textureLoad(picture, xy, 0).rgb * 0.5 + 0.25, params.mode > 1.5);
    let speed = length(mv);
    if (!(speed > 0.05)) {
        return vec4f(base, 1.0);
    }
    let t = min(speed / params.scale, 1.0);
    return vec4f(mix(base, hue(atan2(mv.y, mv.x) / 6.2831853), t), 1.0);
}
)""", source.format, source.size.width, source.size.height, true);
  run_motion_pass(encoder, g_motionViewPass,
                  {static_cast<float>(mode), std::max(g_motionViewScale.load(), 0.25f), 0.f, 0.f, 0.f, 0.f, 0.f, 0.f},
                  displayMotion->view, source.texture.CreateView());
  const auto& target = g_motionViewPass.target;
  return {
      .bindGroup = webgpu::create_copy_bind_group(target),
      .texture = target.texture,
      .size = target.size,
      .format = target.format,
      .efbRect = source.efbRect,  // same size as the picture it replaces
  };
}

// ----- Motion-vector export (AURORA_MV_EXPORT=1 with motion vectors on), for a ReShade add-on feeding a temporal
// upscaler or frame generator: every presentation image gets its own motion and depth (drawn with the picture's
// viewport, so they match what is presented), and right before Present() the presented image's are drawn into one
// fixed texture the size of the swapchain: RGBA32Float (the only one on the device, so the add-on can find it by its
// description). x: the depth of the surface the vectors belong to, raw as the depth buffer holds it (reversed: 1 near,
// 0 far; first, so the texture also works as a depth buffer for effects that read .x); y, z: delta UV of the presented
// picture (previous position = uv + motion, y down); w: 0 outside the picture (motion 0, depth far), inside 1 + why the
// pixel should use the current frame (0 = it should not; bits: 1 the HUD, 2 a surface hidden last frame, 4 transparent
// layers moving differently from what is behind them).
// Drawn, not copied, so it is in the render-target state after every frame.
bool motion_export_active() noexcept {
  static const bool requested = [] {
    const char* env = std::getenv("AURORA_MV_EXPORT");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return requested && gfx::motion_vectors_active();
}
MotionPass g_snapshotMotionPass;
webgpu::TextureWithSampler g_previousMotionDepth;
webgpu::TextureWithSampler g_previousMotionTransparency;

void ensure_previous_copy(webgpu::TextureWithSampler& copy, const webgpu::TextureWithSampler& source,
                          const char* label) {
  if (copy.texture && copy.size.width == source.size.width && copy.size.height == source.size.height) {
    return;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = label,
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {source.size.width, source.size.height, 1},
      .format = source.format,
  };
  copy.texture = g_device.CreateTexture(&descriptor);
  copy.view = copy.texture.CreateView();
  copy.size = descriptor.size;
  copy.format = descriptor.format;
}

// After this frame's snapshots: its depth and transparency become next frame's "last frame".
void encode_previous_motion_depth(const wgpu::CommandEncoder& encoder) {
  const auto copy = [&](const webgpu::TextureWithSampler& from, const webgpu::TextureWithSampler& to) {
    if (!from.texture || !to.texture || to.size.width != from.size.width || to.size.height != from.size.height) {
      return;
    }
    const wgpu::TexelCopyTextureInfo source{.texture = from.texture};
    const wgpu::TexelCopyTextureInfo destination{.texture = to.texture};
    encoder.CopyTextureToTexture(&source, &destination, &from.size);
  };
  copy(webgpu::g_motionDepth, g_previousMotionDepth);
  copy(webgpu::g_motionTransparency, g_previousMotionTransparency);
}
MotionPass g_presentedMotionPass;

// Draws this frame's EFB motion and depth into the image's export texture: image pixel -> place in the picture
// (viewport) -> EFB pixel (the rectangle the picture was copied from); motion scaled from EFB to image pixels.
void encode_snapshot_motion(const wgpu::CommandEncoder& encoder, webgpu::TextureWithSampler& imageMotion,
                            const wgpu::Extent3D& imageSize, const webgpu::Viewport& viewport,
                            const std::array<float, 4>& efbRect) {
  const auto& motion = webgpu::g_motionVectors;
  // Not the depth buffer itself: the game clears it before its HUD, so by now it holds "far" everywhere.
  const auto& depth = webgpu::g_motionDepth;
  if (!imageMotion.texture || imageMotion.size.width != imageSize.width ||
      imageMotion.size.height != imageSize.height) {
    const wgpu::TextureDescriptor descriptor{
        .label = "Presentation image motion vectors and depth",
        .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding,
        .size = {imageSize.width, imageSize.height, 1},
        // The floats' bits as RGBA32Uint: the export texture must stay the only RGBA32Float one on the device.
        .format = wgpu::TextureFormat::RGBA32Uint,
    };
    imageMotion.texture = g_device.CreateTexture(&descriptor);
    imageMotion.view = imageMotion.texture.CreateView();
    imageMotion.size = descriptor.size;
    imageMotion.format = descriptor.format;
  }
  auto rect = efbRect;
  if (rect[2] <= 0.f || rect[3] <= 0.f) {
    rect = {0.f, 0.f, static_cast<float>(motion.size.width), static_cast<float>(motion.size.height)};
  }
  static std::array<float, 8> s_lastMapping{};
  const std::array<float, 8> mapping{viewport.left, viewport.top, viewport.width, viewport.height,
                                     rect[0], rect[1], rect[2], rect[3]};
  if (mapping != s_lastMapping) {
    s_lastMapping = mapping;
    Log.info("Motion vector export: EFB ({}, {}) {}x{} drawn at ({}, {}) {}x{} in the {}x{} presented image", rect[0],
             rect[1], rect[2], rect[3], viewport.left, viewport.top, viewport.width, viewport.height,
             imageSize.width, imageSize.height);
  }
  // Last frame's depth and transparency (copied after its snapshots, see encode_previous_motion_depth).
  ensure_previous_copy(g_previousMotionDepth, depth, "Motion vector depth, last frame");
  ensure_previous_copy(g_previousMotionTransparency, webgpu::g_motionTransparency,
                       "Motion vector transparency, last frame");
  prepare_motion_pass(g_snapshotMotionPass, "Presentation image motion vectors and depth", R"""(
struct Params {
    viewport: vec4f,  // where the picture is drawn in the image (pixels)
    rect: vec4f,      // EFB rectangle the picture was copied from (render pixels)
    image: vec4f,     // image size (pixels), reversed Z (1 or 0), unused
};
@group(0) @binding(0) var motion: texture_2d<f32>;
@group(0) @binding(1) var depth: texture_2d<f32>;          // x: depth now, y: this surface's depth last frame
@group(0) @binding(2) var<uniform> params: Params;
@group(0) @binding(3) var mark: texture_2d<f32>;           // the 2D layer's "use the current frame"
@group(0) @binding(4) var depthBefore: texture_2d<f32>;    // last frame's depth (x)
@group(0) @binding(5) var transparency: texture_2d<f32>;   // transparent layers: motion * coverage (xy), coverage (z)
@group(0) @binding(6) var transparencyBefore: texture_2d<f32>;  // the same, last frame

@fragment
fn fs_main(@builtin(position) pos: vec4f) -> @location(0) vec4u {
    let local = (pos.xy - params.viewport.xy) / params.viewport.zw;
    if (any(local < vec2f(0.0)) || any(local >= vec2f(1.0))) {
        return vec4u(0u);  // outside the picture: no motion, far depth (reversed Z), w 0
    }
    let size = vec2f(textureDimensions(motion));
    let texel = vec2i(clamp(floor(params.rect.xy + local * params.rect.zw), vec2f(0.0), size - 1.0));
    let efbMotion = textureLoad(motion, texel, 0).xy;
    let d = textureLoad(depth, texel, 0).xy;
    // Uncovered: where this surface was last frame, something nearer was visible (it was hidden), so there is no
    // history for it. Nearer = larger depth with reversed Z; 1% plus a small bias keeps depth noise out.
    var uncovered = 0.0;
    var layerLeft = 0.0;
    let layers = textureLoad(transparency, texel, 0);
    let before = vec2f(texel) + 0.5 + efbMotion;
    if (all(before >= vec2f(0.0)) && all(before < size)) {
        // A transparent layer that was here last frame (where this surface was), moved differently from the surface,
        // and has gone or faded: the history still shows it (a tree drawn as a cut-out sprite leaves its branches over
        // the sky as the camera moves). One moving with the surface (a cut-out fringe on a rock) only changes coverage
        // by sampling noise.
        let layersBefore = textureLoad(transparencyBefore, vec2i(before), 0);
        if (layersBefore.z > 0.35 && layers.z < layersBefore.z - 0.3 &&
            length(layersBefore.xy / layersBefore.z - efbMotion) > 1.0) {
            layerLeft = 1.0;
        }
        // All four texels around where it was must have been nearer: one texel alone flags bumpy surfaces
        // (a rock's own relief) wherever the lookup rounds onto a neighbouring bump.
        let corner = vec2i(floor(before - 0.5));
        var nearer = true;
        for (var i = 0; i < 4; i++) {
            let at = clamp(corner + vec2i(i & 1, i >> 1), vec2i(0), vec2i(size) - 1);
            let seen = textureLoad(depthBefore, at, 0).x;
            let nearerForward = (seen < d.y * 0.99 - 1e-5);
            let nearerReversed = (seen > d.y * 1.01 + 1e-5);
            nearer = nearer && select(nearerForward, nearerReversed, params.image.z > 0.5);
        }
        uncovered = max(uncovered, select(0.0, 1.0, nearer));
    }
    // Transparent layers that move differently from the surface behind them (particles, a glow on something moving,
    // a shadow following a character, leaf edges against the sky while the camera moves): more than a pixel apart.
    var drifting = 0.0;
    if (layers.z > 0.1) {
        drifting = select(0.0, 1.0, length(layers.xy / layers.z - efbMotion) > 1.0);
    }
    // w = 1 + why the pixel should use the current frame (bits: 1 the 2D layer, 2 uncovered, 4 transparency moving
    // differently); a consumer reads any w above 1 as "use the current frame".
    let why = select(0.0, 1.0, textureLoad(mark, texel, 0).x > 0.5) + 2.0 * uncovered + 4.0 * max(drifting, layerLeft);
    // EFB pixels -> image pixels -> delta UV of the image
    let mv = efbMotion * (params.viewport.zw / params.rect.zw) / params.image.xy;
    return bitcast<vec4u>(vec4f(d.x, mv, 1.0 + why));
}
)""", wgpu::TextureFormat::RGBA32Uint, 0, 0, false);
  run_motion_pass(encoder, g_snapshotMotionPass,
                  {viewport.left, viewport.top, viewport.width, viewport.height, rect[0], rect[1], rect[2], rect[3],
                   static_cast<float>(imageSize.width), static_cast<float>(imageSize.height),
                   gx::UseReversedZ ? 1.f : 0.f, 0.f},
                  motion.view, depth.view, &imageMotion.view,
                  {webgpu::g_motionMark.view, g_previousMotionDepth.view, webgpu::g_motionTransparency.view,
                   g_previousMotionTransparency.view});
}

// Right before Present(): the presented image's motion and depth into the fixed export texture.
void encode_presented_motion(const wgpu::CommandEncoder& encoder, const webgpu::TextureWithSampler& imageMotion) {
  prepare_motion_pass(g_presentedMotionPass, "Presented motion vectors (export)", R"""(
struct Params {
    pad: vec4f,
    pad2: vec4f,
    pad3: vec4f,
};
@group(0) @binding(0) var motion: texture_2d<u32>;
@group(0) @binding(1) var unused: texture_2d<u32>;
@group(0) @binding(2) var<uniform> params: Params;

@fragment
fn fs_main(@builtin(position) pos: vec4f) -> @location(0) vec4f {
    _ = textureDimensions(unused);
    _ = params.pad;
    return bitcast<vec4f>(textureLoad(motion, vec2i(pos.xy), 0));
}
)""", wgpu::TextureFormat::RGBA32Float, imageMotion.size.width, imageMotion.size.height, false);
  run_motion_pass(encoder, g_presentedMotionPass, {}, imageMotion.view, imageMotion.view);
}

std::optional<PendingMotionDump> encode_motion_dump(const wgpu::CommandEncoder& encoder) {
  static const char* dir = std::getenv("AURORA_MV_DUMP_DIR");
  if (dir == nullptr || dir[0] == '\0' || !gfx::motion_vectors_active() || !webgpu::g_motionVectors.texture ||
      webgpu::g_graphicsConfig.msaaSamples > 1) {
    return std::nullopt;
  }
  static const uint32_t interval = [] {
    const char* v = std::getenv("AURORA_MV_DUMP_INTERVAL");
    return v != nullptr ? std::max(1u, static_cast<uint32_t>(std::strtoul(v, nullptr, 10))) : 60u;
  }();
  static const uint32_t maxDumps = [] {
    const char* v = std::getenv("AURORA_MV_DUMP_MAX");
    return v != nullptr ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 30u;
  }();
  static const uint32_t start = [] {
    const char* v = std::getenv("AURORA_MV_DUMP_START");
    return v != nullptr ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
  }();
  static const char* trigger = std::getenv("AURORA_MV_DUMP_TRIGGER");
  static uint32_t dumps = 0;
  static uint32_t seriesStart = trigger != nullptr && trigger[0] != '\0' ? UINT32_MAX : start;
  const uint32_t frame = gfx::current_frame();
  if (trigger != nullptr && trigger[0] != '\0' && frame % 10 == 0 && std::remove(trigger) == 0) {
    seriesStart = frame + 1;
    dumps = 0;
    Log.info("Motion vector dump triggered at frame {}", frame);
  }
  if (frame < seriesStart || (frame - seriesStart) % interval != 0 || dumps >= maxDumps) {
    return std::nullopt;
  }
  const auto& motion = webgpu::g_motionVectors;
  const auto& color = webgpu::g_frameBuffer;
  if (!color.texture || color.size.width != motion.size.width || color.size.height != motion.size.height ||
      !capture_format_supported(color.format)) {
    return std::nullopt;
  }
  ++dumps;
  PendingMotionDump dump{
      .width = motion.size.width,
      .height = motion.size.height,
      .motionPitch = align_to(motion.size.width * 4, 256),
      .colorPitch = align_to(color.size.width * 4, 256),
      .bgra = capture_format_bgra(color.format),
      .frame = frame,
      .dir = dir,
  };
  dump.motionSize = static_cast<uint64_t>(dump.motionPitch) * dump.height;
  dump.colorSize = static_cast<uint64_t>(dump.colorPitch) * dump.height;
  const auto makeBuffer = [](uint64_t size) {
    const wgpu::BufferDescriptor descriptor{
        .label = "Motion vector dump",
        .usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
        .size = size,
    };
    return g_device.CreateBuffer(&descriptor);
  };
  dump.motion = makeBuffer(dump.motionSize);
  dump.color = makeBuffer(dump.colorSize);
  const wgpu::Extent3D size{dump.width, dump.height, 1};
  const auto copy = [&](const wgpu::Texture& texture, const wgpu::Buffer& buffer, uint32_t pitch) {
    const wgpu::TexelCopyTextureInfo source{.texture = texture, .mipLevel = 0, .origin = {0, 0, 0},
                                            .aspect = wgpu::TextureAspect::All};
    const wgpu::TexelCopyBufferInfo destination{
        .layout = {.offset = 0, .bytesPerRow = pitch, .rowsPerImage = dump.height}, .buffer = buffer};
    encoder.CopyTextureToBuffer(&source, &destination, &size);
  };
  copy(motion.texture, dump.motion, dump.motionPitch);
  copy(color.texture, dump.color, dump.colorPitch);
  if (const auto& exported = g_presentedMotionPass.target; exported.texture) {
    dump.exportWidth = exported.size.width;
    dump.exportHeight = exported.size.height;
    dump.exportPitch = align_to(exported.size.width * 16, 256);
    dump.exported = makeBuffer(static_cast<uint64_t>(dump.exportPitch) * dump.exportHeight);
    const wgpu::Extent3D exportSize{dump.exportWidth, dump.exportHeight, 1};
    const wgpu::TexelCopyTextureInfo source{.texture = exported.texture};
    const wgpu::TexelCopyBufferInfo destination{
        .layout = {.offset = 0, .bytesPerRow = dump.exportPitch, .rowsPerImage = dump.exportHeight},
        .buffer = dump.exported};
    encoder.CopyTextureToBuffer(&source, &destination, &exportSize);
  }
  return dump;
}

void complete_motion_dump(PendingMotionDump& dump) {
  const auto map = [](wgpu::Buffer& buffer, uint64_t size) {
    wgpu::MapAsyncStatus status = wgpu::MapAsyncStatus::CallbackCancelled;
    const auto future = buffer.MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::WaitAnyOnly,
                                        [&status](wgpu::MapAsyncStatus s, wgpu::StringView) { status = s; });
    const auto wait = g_instance.WaitAny(future, 5000000000);
    return wait == wgpu::WaitStatus::Success && status == wgpu::MapAsyncStatus::Success;
  };
  if (!map(dump.motion, dump.motionSize) || !map(dump.color, dump.colorSize)) {
    Log.error("Motion vector dump {} readback failed", dump.frame);
    return;
  }
  const auto* mv = static_cast<const uint8_t*>(dump.motion.GetConstMappedRange(0, dump.motionSize));
  const auto* px = static_cast<const uint8_t*>(dump.color.GetConstMappedRange(0, dump.colorSize));
  const size_t pixels = static_cast<size_t>(dump.width) * dump.height;
  std::vector<uint8_t> colorRgb(pixels * 3), motionRgb(pixels * 3);
  std::vector<uint16_t> raw(pixels * 2);
  const int r = dump.bgra ? 2 : 0, b = dump.bgra ? 0 : 2;
  double sum = 0.0;
  float maxSpeed = 0.f;
  size_t moving = 0, invalid = 0;
  for (uint32_t y = 0; y < dump.height; ++y) {
    const uint8_t* srcColor = px + static_cast<size_t>(y) * dump.colorPitch;
    const auto* srcMotion = reinterpret_cast<const uint16_t*>(mv + static_cast<size_t>(y) * dump.motionPitch);
    for (uint32_t x = 0; x < dump.width; ++x) {
      const size_t i = static_cast<size_t>(y) * dump.width + x;
      colorRgb[i * 3 + 0] = srcColor[x * 4 + r];
      colorRgb[i * 3 + 1] = srcColor[x * 4 + 1];
      colorRgb[i * 3 + 2] = srcColor[x * 4 + b];
      raw[i * 2 + 0] = srcMotion[x * 2 + 0];
      raw[i * 2 + 1] = srcMotion[x * 2 + 1];
      const float mx = half_to_float(srcMotion[x * 2 + 0]);
      const float my = half_to_float(srcMotion[x * 2 + 1]);
      float rgb[3] = {0.5f, 0.5f, 0.5f};
      if (!std::isfinite(mx) || !std::isfinite(my)) {
        ++invalid;
        rgb[0] = 1.f; rgb[1] = 0.f; rgb[2] = 1.f;
      } else {
        const float speed = std::sqrt(mx * mx + my * my);
        if (speed > 0.05f) {
          ++moving;
          sum += speed;
          maxSpeed = std::max(maxSpeed, speed);
          const float angle = std::atan2(my, mx) / 6.2831853f;
          const float t = std::min(speed / 16.f, 1.f);
          for (int c = 0; c < 3; ++c) {
            float f = angle - static_cast<float>(c) / 3.f;
            f -= std::floor(f);
            const float hue = std::clamp(3.f * std::fabs(2.f * f - 1.f) - 1.f, 0.f, 1.f);
            rgb[c] = 0.5f + (hue - 0.5f) * t;
          }
        }
      }
      for (int c = 0; c < 3; ++c) {
        motionRgb[i * 3 + c] = static_cast<uint8_t>(std::clamp(rgb[c], 0.f, 1.f) * 255.f + 0.5f);
      }
    }
  }
  dump.motion.Unmap();
  dump.color.Unmap();
  aurora_write_png_rgb(fmt::format("{}/efb_{:06}.png", dump.dir, dump.frame).c_str(), colorRgb.data(), dump.width,
                       dump.height);
  aurora_write_png_rgb(fmt::format("{}/mv_{:06}.png", dump.dir, dump.frame).c_str(), motionRgb.data(), dump.width,
                       dump.height);
  if (FILE* f = std::fopen(fmt::format("{}/mv_{:06}.rg16f", dump.dir, dump.frame).c_str(), "wb")) {
    std::fwrite(&dump.width, sizeof(uint32_t), 1, f);
    std::fwrite(&dump.height, sizeof(uint32_t), 1, f);
    std::fwrite(raw.data(), sizeof(uint16_t), raw.size(), f);
    std::fclose(f);
  }
  if (dump.exported && map(dump.exported, static_cast<uint64_t>(dump.exportPitch) * dump.exportHeight)) {
    const auto* ex = static_cast<const uint8_t*>(
        dump.exported.GetConstMappedRange(0, static_cast<uint64_t>(dump.exportPitch) * dump.exportHeight));
    double exSum = 0.0, depthSum = 0.0;
    size_t exMoving = 0, inside = 0;
    float centre[2] = {0.f, 0.f};
    float depthMin = 1e30f, depthMax = -1e30f, centreDepth = 0.f;
    std::vector<uint8_t> depthImage(static_cast<size_t>(dump.exportWidth) * dump.exportHeight * 3);
    std::vector<uint8_t> maskImage(depthImage.size());  // colored where the mask says "use the current frame"
    size_t masked = 0;
    std::array<size_t, 8> whyCount{};
    for (uint32_t y = 0; y < dump.exportHeight; ++y) {
      const auto* row = reinterpret_cast<const float*>(ex + static_cast<size_t>(y) * dump.exportPitch);
      for (uint32_t x = 0; x < dump.exportWidth; ++x) {
        const float d = row[x * 4];
        // Depth picture: reversed Z (near 1), shown with a square root so distant ground keeps some contrast.
        const uint8_t shade = static_cast<uint8_t>(std::clamp(std::sqrt(std::max(d, 0.f)), 0.f, 1.f) * 255.f);
        const size_t o = (static_cast<size_t>(y) * dump.exportWidth + x) * 3;
        depthImage[o] = depthImage[o + 1] = depthImage[o + 2] = shade;
        if (row[x * 4 + 3] > 1.5f) {
          // red: the 2D layer, green: uncovered, blue: transparency moving differently
          const int why = static_cast<int>(row[x * 4 + 3] - 1.f + 0.5f);
          const size_t o = (static_cast<size_t>(y) * dump.exportWidth + x) * 3;
          maskImage[o] = (why & 1) ? 255 : 0;
          maskImage[o + 1] = (why & 2) ? 255 : 0;
          maskImage[o + 2] = (why & 4) ? 255 : 0;
          ++masked;
          ++whyCount[why & 7];
        }
        if (row[x * 4 + 3] > 0.5f) {
          ++inside;
          depthSum += d;
          depthMin = std::min(depthMin, d);
          depthMax = std::max(depthMax, d);
        }
        if (x == dump.exportWidth / 2 && y == dump.exportHeight / 2) {
          centreDepth = d;
        }
        const float px = row[x * 4 + 1] * dump.exportWidth, py = row[x * 4 + 2] * dump.exportHeight;
        const float speed = std::sqrt(px * px + py * py);
        if (speed > 0.05f) {
          ++exMoving;
          exSum += speed;
        }
        if (x == dump.exportWidth / 2 && y == dump.exportHeight / 2) {
          centre[0] = px;
          centre[1] = py;
        }
      }
    }
    dump.exported.Unmap();
    aurora_write_png_rgb(fmt::format("{}/depth_{:06}.png", dump.dir, dump.frame).c_str(), depthImage.data(),
                         dump.exportWidth, dump.exportHeight);
    for (size_t i = 0; i < maskImage.size(); i += 3) {
      if (maskImage[i] == 0 && maskImage[i + 1] == 0 && maskImage[i + 2] == 0) {  // unmasked: the depth picture, dimmed
        maskImage[i] = maskImage[i + 1] = maskImage[i + 2] = depthImage[i] / 2;
      }
    }
    aurora_write_png_rgb(fmt::format("{}/mask_{:06}.png", dump.dir, dump.frame).c_str(), maskImage.data(),
                         dump.exportWidth, dump.exportHeight);
    Log.info("Motion vector dump frame {}: export {}x{} (last presented), moving {:.1f}%, mean {:.2f} px, centre "
             "({:.2f}, {:.2f}) px; depth min {:.5f} max {:.5f} mean {:.5f} centre {:.5f}, picture {:.1f}% of the image, "
             "use-current-frame mask {:.2f}% of the picture (2D {:.2f}%, uncovered {:.2f}%, transparency {:.2f}%)",
             dump.frame, dump.exportWidth, dump.exportHeight,
             100.0 * exMoving / std::max<size_t>(size_t{dump.exportWidth} * dump.exportHeight, 1),
             exMoving ? exSum / exMoving : 0.0, centre[0], centre[1], inside ? depthMin : 0.f,
             inside ? depthMax : 0.f, inside ? depthSum / inside : 0.0, centreDepth,
             100.0 * inside / std::max<size_t>(size_t{dump.exportWidth} * dump.exportHeight, 1),
             100.0 * masked / std::max<size_t>(inside, 1),
             100.0 * (whyCount[1] + whyCount[3] + whyCount[5] + whyCount[7]) / std::max<size_t>(inside, 1),
             100.0 * (whyCount[2] + whyCount[3] + whyCount[6] + whyCount[7]) / std::max<size_t>(inside, 1),
             100.0 * (whyCount[4] + whyCount[5] + whyCount[6] + whyCount[7]) / std::max<size_t>(inside, 1));
  }
  {
    const auto* centreMv = reinterpret_cast<const uint16_t*>(
        raw.data() + (static_cast<size_t>(dump.height / 2) * dump.width + dump.width / 2) * 2);
    Log.info("Motion vector dump frame {}: EFB centre ({:.2f}, {:.2f}) px", dump.frame, half_to_float(centreMv[0]),
             half_to_float(centreMv[1]));
  }
  Log.info("Motion vector dump frame {}: {}x{}, moving {:.1f}%, mean {:.2f} px, max {:.1f} px, invalid {}",
           dump.frame, dump.width, dump.height, 100.0 * moving / std::max<size_t>(pixels, 1),
           moving ? sum / moving : 0.0, maxSpeed, invalid);
}

#endif

// AuroraBackend is an anonymous C typedef enum, which magic_enum cannot
// reflect under this toolchain -- name it by hand for diagnostics.
constexpr const char* backend_name(AuroraBackend backend) noexcept {
  switch (backend) {
  case BACKEND_AUTO:
    return "Auto";
  case BACKEND_D3D11:
    return "D3D11";
  case BACKEND_D3D12:
    return "D3D12";
  case BACKEND_METAL:
    return "Metal";
  case BACKEND_VULKAN:
    return "Vulkan";
  case BACKEND_OPENGL:
    return "OpenGL";
  case BACKEND_OPENGLES:
    return "OpenGLES";
  case BACKEND_WEBGPU:
    return "WebGPU";
  case BACKEND_NULL:
    return "Null";
  }
  return "Unknown";
}

#ifdef AURORA_ENABLE_GX
constexpr std::array PreferredBackendOrder{
#ifdef ENABLE_BACKEND_WEBGPU
    BACKEND_WEBGPU,
#endif
#ifdef DAWN_ENABLE_BACKEND_D3D12
    BACKEND_D3D12,
#endif
#if defined(_WIN32) && defined(DAWN_ENABLE_BACKEND_D3D11)
    BACKEND_D3D11,
#endif
#ifdef DAWN_ENABLE_BACKEND_METAL
    BACKEND_METAL,
#endif
#ifdef DAWN_ENABLE_BACKEND_VULKAN
    BACKEND_VULKAN,
#endif
#if !defined(_WIN32) && defined(DAWN_ENABLE_BACKEND_D3D11)
    BACKEND_D3D11,
#endif
// #ifdef DAWN_ENABLE_BACKEND_DESKTOP_GL
//     BACKEND_OPENGL,
// #endif
// #ifdef DAWN_ENABLE_BACKEND_OPENGLES
//     BACKEND_OPENGLES,
// #endif
#ifdef DAWN_ENABLE_BACKEND_NULL
    BACKEND_NULL,
#endif
};
#else
constexpr std::array<AuroraBackend, 0> PreferredBackendOrder{};
#endif

bool g_initialFrame = false;


AuroraInfo initialize(int argc, char* argv[], const AuroraConfig& config) {
  g_config = config;
  Log.info("Aurora initializing");
  log_system_information();
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  } else {
    g_config.appName = strdup(g_config.appName);
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.userPath = strdup(g_config.userPath);
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = SDL_GetPrefPath(nullptr, g_config.appName);
  } else {
    g_config.cachePath = strdup(g_config.cachePath);
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  } else {
    g_config.resourcesPath = strdup(g_config.resourcesPath);
  }
  if (g_config.pipelineCachePath == nullptr) {
    g_config.pipelineCachePath = g_config.cachePath;
  } else {
    g_config.pipelineCachePath = strdup(g_config.pipelineCachePath);
  }
  if (g_config.msaa == 0) {
    g_config.msaa = 1;
  }
  if (g_config.maxTextureAnisotropy == 0) {
    g_config.maxTextureAnisotropy = 16;
  }
  ASSERT(window::initialize(), "Error initializing window");

  g_sdlCustomEventsStart = SDL_RegisterEvents(2);
  ASSERT(g_sdlCustomEventsStart, "Failed to allocate user events: {}", SDL_GetError());
  ASSERT(window::initialize_event_watch(), "Error initializing SDL event watch");

#ifdef AURORA_ENABLE_GX
  /* Attempt to create a window using the calling application's desired backend */
  const AuroraBackend requestedBackend = config.desiredBackend;
  AuroraBackend selectedBackend = requestedBackend;
  bool windowCreated = false;
  std::string firstGraphicsError;
  const auto rememberGraphicsError = [&] {
    if (firstGraphicsError.empty() && SDL_GetError()[0] != '\0') {
      firstGraphicsError = SDL_GetError();
    }
  };
  if (selectedBackend != BACKEND_AUTO) {
    Log.info("Requested graphics backend: {}", backend_name(selectedBackend));
    if (window::create_window(selectedBackend)) {
      if (webgpu::initialize(selectedBackend)) {
        windowCreated = true;
      } else {
        rememberGraphicsError();
        window::destroy_window();
      }
    } else {
      rememberGraphicsError();
      Log.error("Failed to create a window for backend {}: {}", backend_name(selectedBackend),
                SDL_GetError());
    }
    if (!windowCreated) {
      /* An explicitly requested backend that cannot be brought up falls back to the BACKEND_AUTO
       * search instead of aborting, and the substitution is always reported. */
      Log.error("Requested graphics backend {} is unavailable on this system; "
                "falling back to automatic selection",
                backend_name(requestedBackend));
    }
  }

  if (!windowCreated) {
    for (const auto backendType : PreferredBackendOrder) {
      selectedBackend = backendType;
      if (!window::create_window(selectedBackend)) {
        rememberGraphicsError();
        continue;
      }
      if (webgpu::initialize(selectedBackend)) {
        windowCreated = true;
        break;
      } else {
        rememberGraphicsError();
        window::destroy_window();
      }
    }
  }

  if (!windowCreated) {
    if (firstGraphicsError.empty()) firstGraphicsError = "No supported graphics backend is available";
    SDL_SetError("%s", firstGraphicsError.c_str());
    Log.error("Graphics initialization failed: {}", firstGraphicsError);
    return {
        .initializationStatus = AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE,
        .initializationError = SDL_GetError(),
    };
  }
  if (requestedBackend != BACKEND_AUTO && selectedBackend != requestedBackend) {
    Log.error("Graphics backend fallback in effect: video.graphics_api requested {}, "
              "running on {}",
              backend_name(requestedBackend), backend_name(selectedBackend));
  }

  // Initialize SDL_Renderer for ImGui when we can't use a Dawn backend
  if (webgpu::g_backendType == wgpu::BackendType::Null) {
    ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
  }
#else
  AuroraBackend selectedBackend = BACKEND_NULL;
  ASSERT(window::create_window(BACKEND_NULL), "Error creating window: {}", SDL_GetError());
  ASSERT(window::create_renderer(), "Failed to initialize SDL renderer: {}", SDL_GetError());
#endif

  window::show_window();

#ifdef AURORA_ENABLE_GX
  gfx::initialize();

  g_metalfxSupported.store(webgpu::metalfx::supported(g_device, webgpu::g_backendType));
  g_metalfxStatus.store(AURORA_METALFX_DISABLED);

  imgui::create_context();
#endif
  const auto size = window::get_window_size();
  Log.info("Using framebuffer size {}x{} scale {}", size.fb_width, size.fb_height, size.scale);
#ifdef AURORA_ENABLE_GX
  if (g_config.imGuiInitCallback != nullptr) {
    g_config.imGuiInitCallback(&size);
  }
  imgui::initialize();
#endif

  g_initialFrame = true;
  g_config.desiredBackend = selectedBackend;
  return {
      .backend = selectedBackend,
      .userPath = g_config.userPath,
      .cachePath = g_config.cachePath,
      .window = window::get_sdl_window(),
      .windowSize = size,
#ifdef AURORA_ENABLE_GX
      .gpuName = webgpu::adapter_name(),
      .gpuCompatActive = webgpu::g_safeVertexFetch,
#endif
  };
}

#ifdef AURORA_ENABLE_GX
struct AcquiredSurfaceTexture {
  wgpu::Texture texture;
  wgpu::TextureView view;
};

std::optional<AcquiredSurfaceTexture> acquire_surface_texture() noexcept {
  if (!window::is_presentable()) {
    request_surface_reconfigure();
    return std::nullopt;
  }
  if (!g_surface) {
    request_surface_reconfigure();
    return std::nullopt;
  }

  wgpu::SurfaceTexture surfaceTexture;
  g_surface.GetCurrentTexture(&surfaceTexture);
  switch (surfaceTexture.status) {
  case wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal:
    return AcquiredSurfaceTexture{
        .texture = surfaceTexture.texture,
        .view = surfaceTexture.texture.CreateView(),
    };
  case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
    Log.info("Surface texture is suboptimal, deferring swapchain reconfiguration");
    request_surface_reconfigure();
    return AcquiredSurfaceTexture{
        .texture = surfaceTexture.texture,
        .view = surfaceTexture.texture.CreateView(),
    };
  case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
    Log.warn("Surface texture acquisition timed out");
    return std::nullopt;
  case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
    Log.info("Surface texture is {}, reconfiguring swapchain", magic_enum::enum_name(surfaceTexture.status));
    request_surface_reconfigure();
    return std::nullopt;
  case wgpu::SurfaceGetCurrentTextureStatus::Lost:
    Log.warn("Surface texture is {}, requesting surface recreation", magic_enum::enum_name(surfaceTexture.status));
    request_surface_recreate();
    return std::nullopt;
  case wgpu::SurfaceGetCurrentTextureStatus::Error:
    Log.warn("Surface texture is {}, deferring surface recovery", magic_enum::enum_name(surfaceTexture.status));
    request_surface_reconfigure();
    return std::nullopt;
  default:
    Log.error("Failed to get surface texture: {}", magic_enum::enum_name(surfaceTexture.status));
    return std::nullopt;
  }
}

struct PresentationImage {
  webgpu::TextureWithSampler texture;
  wgpu::BindGroup bindGroup;
  // Motion-vector export: this image's motion (see motion_export_active); written by the snapshot that draws it.
  mutable webgpu::TextureWithSampler motion;
  mutable bool hasMotion = false;
};

struct PresentationJob {
  std::shared_ptr<PresentationImage> image;
  uint32_t logicalFrame = 0;
  // Absolute deadline on PresentClock stamped by the producer's schedule.
  // Default (epoch) means "present as soon as ready" (no software pacing).
  PresentClock::time_point presentAt{};
  bool interpolated = false;
  // Slot carries a copy of the native image rather than a replayed interpolation. It counts as a
  // present but not toward effectiveFramesPerSecond.
  bool duplicated = false;
  // Whole display periods the group slid forward at encode time (late group).
  uint32_t slidPeriods = 0;
};

std::array<std::vector<std::shared_ptr<PresentationImage>>, gx::MaxInterpolatedFrames + 1>
    g_presentationImagePools;

std::shared_ptr<PresentationImage> acquire_presentation_image(size_t slot, uint32_t width,
                                                              uint32_t height) {
  auto& pool = g_presentationImagePools.at(slot);
  // A use count of one means only the pool holds the image, so no job can be reading it. Idle
  // images from an older surface size are dropped here instead of leaking for the run.
  for (auto it = pool.begin(); it != pool.end();) {
    const auto& image = *it;
    if (image->texture.size.width == width && image->texture.size.height == height) {
      if (image.use_count() == 1) {
        return image;
      }
      ++it;
      continue;
    }
    if (image.use_count() == 1) {
      it = pool.erase(it);
    } else {
      ++it;
    }
  }

  auto image = std::make_shared<PresentationImage>();
  image->texture = webgpu::create_render_texture(width, height, false);
  image->bindGroup = webgpu::create_copy_bind_group(image->texture);
  pool.emplace_back(image);
  return image;
}

// NSMBW V-Sync: how long the last presentation waited to acquire a swapchain image. A long wait
// means the flip queue is backed up (frames queued while V-Sync was switched on); with the game's
// clock locked to the display that backlog never drains by itself, so the presenter drops a frame.
std::atomic<int64_t> g_lastAcquireNanos{0};

bool present_presentation_job(const PresentationJob& job) {
  ZoneScoped;
  const auto submissionStarted = PresentClock::now();
  // Keep the threshold far above compositor and scheduling jitter. The timings below separate a
  // real surface stall from a bad deadline, and only the former needs a rebuild.
  constexpr auto kSurfaceStallThreshold = std::chrono::milliseconds(250);
  std::chrono::nanoseconds surfaceLockDuration{};
  std::chrono::nanoseconds acquireDuration{};
  std::chrono::nanoseconds encodeDuration{};
  std::chrono::nanoseconds finishDuration{};
  std::chrono::nanoseconds submitDuration{};
  std::chrono::nanoseconds scheduleWaitDuration{};
  std::chrono::nanoseconds presentDuration{};
  bool presented = false;
  if (g_surfaceReconfigurePending.load(std::memory_order_acquire) ||
      window::native_resize_pending() || !window::is_presentable()) {
    return false;
  }
  std::chrono::nanoseconds lateBy{};
  if (job.presentAt != PresentClock::time_point{}) {
    // How expired the deadline already is at dequeue. Positive values mean the
    // slot cannot be paced and fires immediately, which is a burst symptom.
    lateBy = std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() -
                                                                  job.presentAt);
  }
  {
    window::SurfaceLock surfaceLock;
    // Acquire, encode, submit and present are one unit against a configured swapchain, so the
    // surface lock covers all of them. The renderer mutex is deliberately not taken.
    const auto surfaceLockStarted = PresentClock::now();
    std::unique_lock surfaceOwnership(g_surfaceMutex);
    surfaceLockDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
        PresentClock::now() - surfaceLockStarted);
    // Surface contention is waiting, not encoding, so keep it out of the encode timing where a
    // reconfigure would look like GPU command recording.
    const auto workStarted = PresentClock::now();
    bool surfaceSizeChanged = window::native_resize_pending();
    if (!surfaceSizeChanged &&
        !g_surfaceReconfigurePending.load(std::memory_order_acquire) &&
        window::is_presentable() && g_surface) {
      // native_window_size_matches compares the OS client size with the configured swapchain, which is
      // what native_fb_* reports. One window query instead of SDL's per-call ones.
      surfaceSizeChanged = !window::native_window_size_matches(
          webgpu::g_graphicsConfig.surfaceConfiguration.width,
          webgpu::g_graphicsConfig.surfaceConfiguration.height);
    }
    if (!surfaceSizeChanged && window::is_presentable()) {
      const auto acquireStarted = PresentClock::now();
      auto acquired = acquire_surface_texture();
      acquireDuration =
          std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - acquireStarted);
      g_lastAcquireNanos.store(acquireDuration.count(), std::memory_order_relaxed);
      if (acquired) {
        const wgpu::CommandEncoderDescriptor encoderDescriptor{
            .label = "Presentation encoder",
        };
        const auto encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
        const std::array attachments{
            wgpu::RenderPassColorAttachment{
                .view = acquired->view,
                .loadOp = wgpu::LoadOp::Clear,
                .storeOp = wgpu::StoreOp::Store,
            },
        };
        const wgpu::RenderPassDescriptor renderPassDescriptor{
            .label = "Presentation copy pass",
            .colorAttachmentCount = attachments.size(),
            .colorAttachments = attachments.data(),
        };
        const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
        pass.SetPipeline(webgpu::g_PresentPipeline);
        pass.SetBindGroup(0, job.image->bindGroup, 0, nullptr);
        pass.SetViewport(0.f, 0.f,
                         static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.width),
                         static_cast<float>(webgpu::g_graphicsConfig.surfaceConfiguration.height),
                         0.f, 1.f);
        pass.Draw(3);
        pass.End();
        if (job.image->hasMotion && job.image->motion.texture) {
          encode_presented_motion(encoder, job.image->motion);
        }
        const auto encodeFinished = PresentClock::now();
        encodeDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            encodeFinished - workStarted - acquireDuration);
        const wgpu::CommandBufferDescriptor cmdBufDescriptor{
            .label = "Presentation command buffer",
        };
        const auto finishStarted = PresentClock::now();
        const auto buffer = encoder.Finish(&cmdBufDescriptor);
        finishDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            PresentClock::now() - finishStarted);
        const auto submitStarted = PresentClock::now();
        {
          std::lock_guard submitLock(g_queueSubmitMutex);
          g_queue.Submit(1, &buffer);
        }
        submitDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            PresentClock::now() - submitStarted);
        // Pace the Present() call itself, not the whole unit, so acquire/encode/submit variance stays out
        // of the cadence. Holding the image across the wait is safe while the surface lock is held.
        if (job.presentAt != PresentClock::time_point{}) {
          const auto scheduleWaitStarted = PresentClock::now();
          wait_until_precise(job.presentAt);
          scheduleWaitDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
              PresentClock::now() - scheduleWaitStarted);
        }
        // A native resize can arrive after acquisition, so drop the obsolete image and let the render
        // worker reconfigure at its ordered frame boundary.
        if (!g_surfaceReconfigurePending.load(std::memory_order_acquire) &&
            !window::native_resize_pending() && window::is_presentable() &&
            window::native_window_size_matches(
                webgpu::g_graphicsConfig.surfaceConfiguration.width,
                webgpu::g_graphicsConfig.surfaceConfiguration.height)) {
          const auto presentStarted = PresentClock::now();
          wgpu::Status presentStatus;
          {
            std::lock_guard submitLock(g_queueSubmitMutex);
            presentStatus = g_surface.Present();
          }
          presentDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(
              PresentClock::now() - presentStarted);
          if (presentStatus == wgpu::Status::Success) {
            presented = true;
            record_successful_present(
                job.interpolated, job.logicalFrame, acquireDuration, encodeDuration,
                finishDuration, submitDuration, presentDuration,
                std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() -
                                                                     submissionStarted),
                job.duplicated, job.slidPeriods, lateBy);
          } else {
            Log.warn("Surface present failed: {}", static_cast<int>(presentStatus));
            request_surface_reconfigure();
          }
        }
        acquired.reset();
      }
    }
  }
  const auto totalDuration =
      std::chrono::duration_cast<std::chrono::nanoseconds>(PresentClock::now() - submissionStarted);
  constexpr int kStallRebuildThreshold = 3;
  constexpr auto kStallRebuildCooldown = std::chrono::seconds(5);
  static int s_consecutiveStalledPresents = 0;
  static PresentClock::time_point s_lastStallRebuild{};
  if (totalDuration >= kSurfaceStallThreshold) {
    const auto surfaceWorkDuration =
        acquireDuration + encodeDuration + finishDuration + submitDuration + presentDuration;
    const bool surfaceStalled = surfaceWorkDuration >= kSurfaceStallThreshold;
    bool rebuildRequested = false;
    if (surfaceStalled) {
      ++s_consecutiveStalledPresents;
      const auto now = PresentClock::now();
      if (s_consecutiveStalledPresents >= kStallRebuildThreshold &&
          (s_lastStallRebuild == PresentClock::time_point{} ||
           now - s_lastStallRebuild >= kStallRebuildCooldown)) {
        s_lastStallRebuild = now;
        s_consecutiveStalledPresents = 0;
        rebuildRequested = true;
      }
    } else {
      s_consecutiveStalledPresents = 0;
    }
    Log.warn("Presentation job took {:.1f} ms (surface lock {:.1f}, acquire {:.1f}, encode {:.1f}, "
             "finish {:.1f}, submit {:.1f}, schedule wait {:.1f}, present {:.1f}){}",
             std::chrono::duration<double, std::milli>(totalDuration).count(),
             std::chrono::duration<double, std::milli>(surfaceLockDuration).count(),
             std::chrono::duration<double, std::milli>(acquireDuration).count(),
             std::chrono::duration<double, std::milli>(encodeDuration).count(),
             std::chrono::duration<double, std::milli>(finishDuration).count(),
             std::chrono::duration<double, std::milli>(submitDuration).count(),
             std::chrono::duration<double, std::milli>(scheduleWaitDuration).count(),
             std::chrono::duration<double, std::milli>(presentDuration).count(),
             rebuildRequested ? "; rebuilding the surface" : "");
    if (rebuildRequested) {
      request_surface_reconfigure();
    }
  } else {
    s_consecutiveStalledPresents = 0;
    // Shorter hitches than the stall warning, which still cost a 60 Hz frame: logged (at most 30 a
    // minute) so dropped frames on a player's machine show where the presenter lost the time.
    static PresentClock::time_point s_hitchWindow{};
    static int s_hitchesLogged = 0;
    const auto hitchNow = PresentClock::now();
    if (hitchNow - s_hitchWindow > std::chrono::minutes(1)) {
      s_hitchWindow = hitchNow;
      s_hitchesLogged = 0;
    }
    // The schedule wait is the pacing itself, not lost time.
    const auto workDuration = totalDuration - scheduleWaitDuration;
    if ((workDuration >= std::chrono::milliseconds(12) || lateBy >= std::chrono::milliseconds(8) || !presented) &&
        s_hitchesLogged++ < 30) {
      Log.info("Presentation hitch: {:.1f} ms{} (late at dequeue {:.1f}, acquire {:.1f}, encode {:.1f}, submit "
               "{:.1f}, schedule wait {:.1f}, present {:.1f})",
               std::chrono::duration<double, std::milli>(totalDuration).count(), presented ? "" : ", not presented",
               std::chrono::duration<double, std::milli>(lateBy).count(),
               std::chrono::duration<double, std::milli>(acquireDuration).count(),
               std::chrono::duration<double, std::milli>(encodeDuration).count(),
               std::chrono::duration<double, std::milli>(submitDuration).count(),
               std::chrono::duration<double, std::milli>(scheduleWaitDuration).count(),
               std::chrono::duration<double, std::milli>(presentDuration).count());
    }
  }
  return presented;
}

struct PresenterState {
  std::mutex mutex;
  std::condition_variable cv;
  std::thread thread;
  std::deque<PresentationJob> jobs;
  bool started = false;
  bool stop = false;
  bool presenting = false;
};

PresenterState g_presenter;
std::atomic<bool> g_presenterStarted{false};

void presenter_main() noexcept {
  if (!SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_HIGH)) {
    Log.warn("Could not raise the asynchronous presenter thread priority: {}", SDL_GetError());
  }
  for (;;) {
    PresentationJob job;
    {
      std::unique_lock lock(g_presenter.mutex);
      g_presenter.cv.wait(lock, [] { return g_presenter.stop || !g_presenter.jobs.empty(); });
      if (g_presenter.stop && g_presenter.jobs.empty()) {
        break;
      }
      job = std::move(g_presenter.jobs.front());
      g_presenter.jobs.pop_front();
      // V-Sync only, never for an inserted (interpolated) slot: drop this frame when the swapchain
      // is backed up, or when it is already late and a newer one waits. A slow scene with no
      // backlog presents every frame, late or not.
      if (webgpu::vsync_enabled() && !job.interpolated && job.presentAt != PresentClock::time_point{}) {
        constexpr auto kBacklog = std::chrono::milliseconds(8);
        const bool backedUp = g_lastAcquireNanos.exchange(0, std::memory_order_relaxed) >
                              std::chrono::duration_cast<std::chrono::nanoseconds>(kBacklog).count();
        const bool lateWithNewer = !g_presenter.jobs.empty() && PresentClock::now() - job.presentAt > kBacklog;
        if (backedUp || lateWithNewer) {
          lock.unlock();
          g_presenter.cv.notify_all();  // wait_for_presenter_idle may be waiting for the queue to empty
          continue;
        }
      }
      g_presenter.presenting = true;
    }
    g_presenter.cv.notify_all();

    present_presentation_job(job);

    {
      std::lock_guard lock(g_presenter.mutex);
      g_presenter.presenting = false;
    }
    g_presenter.cv.notify_all();
  }
}

void ensure_presenter_started() {
  std::lock_guard lock(g_presenter.mutex);
  if (g_presenter.started) {
    return;
  }
  g_presenter.stop = false;
  g_presenter.presenting = false;
  g_presenter.thread = std::thread(presenter_main);
  g_presenter.started = true;
  g_presenterStarted.store(true, std::memory_order_release);
  Log.info("Enabled bounded asynchronous presentation worker");
}

void wait_for_presenter_idle() noexcept {
  if (!g_presenterStarted.load(std::memory_order_acquire)) {
    return;
  }
  std::unique_lock lock(g_presenter.mutex);
  g_presenter.cv.wait(
      lock, [] { return g_presenter.jobs.empty() && !g_presenter.presenting; });
}

void enqueue_presentations(std::vector<PresentationJob>&& jobs) {
  ensure_presenter_started();
  // Scale the bound with the group being handed over: a 240 Hz frame enqueues four jobs at once,
  // and a bound sized for two groups blocked the frame worker on a draining one.
  const size_t maximumQueuedJobs =
      (std::max)(static_cast<size_t>(2 * (gx::MaxInterpolatedFrames + 1)), 3 * jobs.size());
  std::unique_lock lock(g_presenter.mutex);
  if (g_presenter.stop) {
    return;
  }
  if (g_presenter.jobs.size() + jobs.size() > maximumQueuedJobs) {
    // Presentation is a real-time stream, not a lossless queue: waiting for room couples the guest
    // and audio clocks to a blocked Present(). Keep the newest group and drop obsolete images.
    const size_t dropped = g_presenter.jobs.size();
    g_presenter.jobs.clear();
    if (dropped != 0) {
      // A stuck Present can keep replacement mode active for a while.  Aggregate
      // the warning instead of turning a driver stall into a log flood.
      static size_t droppedSinceWarning = 0;
      static PresentClock::time_point lastWarning{};
      droppedSinceWarning += dropped;
      const auto now = PresentClock::now();
      if (lastWarning == PresentClock::time_point{} || now - lastWarning >= std::chrono::seconds(1)) {
        Log.warn("Presenter fell behind; dropped {} stale presentation jobs", droppedSinceWarning);
        droppedSinceWarning = 0;
        lastWarning = now;
      }
    }
  }
  for (auto& job : jobs) {
    g_presenter.jobs.emplace_back(std::move(job));
  }
  lock.unlock();
  g_presenter.cv.notify_all();
}

void stop_presenter() noexcept {
  if (!g_presenterStarted.load(std::memory_order_acquire)) {
    return;
  }
  {
    std::lock_guard lock(g_presenter.mutex);
    g_presenter.stop = true;
  }
  g_presenter.cv.notify_all();
  if (g_presenter.thread.joinable()) {
    g_presenter.thread.join();
  }
  {
    std::lock_guard lock(g_presenter.mutex);
    g_presenter.jobs.clear();
    g_presenter.started = false;
    g_presenter.presenting = false;
  }
  g_presenterStarted.store(false, std::memory_order_release);
}

struct MetalFXSlot {
  webgpu::metalfx::Size size{};
  std::unique_ptr<webgpu::metalfx::SpatialScaler> scaler;
  wgpu::BindGroup bindGroup;
};
std::array<MetalFXSlot, gx::MaxInterpolatedFrames + 1> g_metalfxSlots;
size_t g_metalfxNextSlot = 0;
webgpu::metalfx::SpatialScaler* g_metalfxPendingOutput = nullptr;
bool g_metalfxFailed = false;

void metalfx_failed(const std::string& reason) {
  Log.warn("MetalFX spatial upscaling disabled: {}; using normal presentation", reason);
  g_metalfxFailed = true;
  g_metalfxStatus.store(AURORA_METALFX_ERROR);
  g_metalfxPendingOutput = nullptr;
  g_metalfxSlots = {};
}

wgpu::BindGroup upscale_presentation(wgpu::CommandEncoder& encoder,
                                     const webgpu::PresentSource& source,
                                     const webgpu::Viewport& viewport, bool enabled) {
  if (!enabled) {
    g_metalfxSlots = {};
    g_metalfxFailed = false;
    g_metalfxStatus.store(AURORA_METALFX_DISABLED);
    return {};
  }
  if (!g_metalfxSupported.load()) {
    g_metalfxStatus.store(AURORA_METALFX_UNSUPPORTED);
    return {};
  }
  if (g_metalfxFailed) return {};
  const webgpu::metalfx::Size size{
      source.size.width, source.size.height,
      static_cast<uint32_t>(viewport.width), static_cast<uint32_t>(viewport.height),
      webgpu::g_graphicsConfig.surfaceConfiguration.format,
  };
  // The existing copy path samples perceptual values from unorm game images.
  // Do not introduce implicit sRGB decoding or downscaling into MetalFX.
  if (!size.inputWidth || !size.inputHeight || size.inputWidth >= size.outputWidth ||
      size.inputHeight >= size.outputHeight ||
      (source.format != wgpu::TextureFormat::RGBA8Unorm && source.format != wgpu::TextureFormat::BGRA8Unorm)) {
    g_metalfxStatus.store(AURORA_METALFX_NOT_UPSCALING);
    return {};
  }
  auto& slot = g_metalfxSlots[g_metalfxNextSlot++ % g_metalfxSlots.size()];
  if (!slot.scaler || !(slot.size == size)) {
    slot = {};
    std::string error;
    slot.scaler = webgpu::metalfx::create(g_instance, g_device, size, error);
    if (!slot.scaler) {
      if (error.empty()) g_metalfxStatus.store(AURORA_METALFX_NOT_UPSCALING);
      else metalfx_failed(error);
      return {};
    }
    slot.size = size;
    wgpu::SamplerDescriptor samplerDescriptor{};
    samplerDescriptor.magFilter = wgpu::FilterMode::Linear;
    samplerDescriptor.minFilter = wgpu::FilterMode::Linear;
    slot.bindGroup = webgpu::create_copy_bind_group(slot.scaler->output_view(),
                                                   g_device.CreateSampler(&samplerDescriptor));
    Log.info("MetalFX spatial slot: {}x{} -> {}x{}", size.inputWidth, size.inputHeight,
             size.outputWidth, size.outputHeight);
  }
  if (!slot.scaler->begin_input()) {
    metalfx_failed(slot.scaler->error());
    return {};
  }
  const wgpu::RenderPassColorAttachment attachment{
      .view = slot.scaler->input_view(),
      .loadOp = wgpu::LoadOp::Clear,
      .storeOp = wgpu::StoreOp::Store,
  };
  const wgpu::RenderPassDescriptor descriptor{
      .label = "MetalFX input copy",
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
  };
  auto pass = encoder.BeginRenderPass(&descriptor);
  pass.SetPipeline(webgpu::g_CopyPipeline);
  pass.SetBindGroup(0, source.bindGroup);
  pass.SetViewport(0, 0, static_cast<float>(size.inputWidth), static_cast<float>(size.inputHeight), 0, 1);
  pass.Draw(3);
  pass.End();
  // Submit the sealed scene and input copy before crossing to the native queue.
  // The replacement encoder composites the upscaled image and ImGui normally.
  auto buffer = encoder.Finish();
  {
    std::lock_guard submitLock(g_queueSubmitMutex);
    g_queue.Submit(1, &buffer);
  }
  encoder = g_device.CreateCommandEncoder();
  if (!slot.scaler->upscale()) {
    metalfx_failed(slot.scaler->error());
    return {};
  }
  g_metalfxPendingOutput = slot.scaler.get();
  g_metalfxStatus.store(AURORA_METALFX_ACTIVE);
  return slot.bindGroup;
}

// `presentSource` is latched in the seal prologue: by the time this encodes, the producer's next
// gfx::begin_frame() may already have cleared the display-copy override.
wgpu::BindGroup encode_presentation_snapshot(wgpu::CommandEncoder& encoder,
                                              const webgpu::PresentSource& presentSource,
                                              const PresentationImage& image,
                                              bool includeImGui, bool metalfxEnabled,
                                              const wgpu::BindGroup* cachedMetalFXOutput = nullptr,
                                              bool exportMotion = false) {
  ZoneScoped;
  auto viewport = webgpu::calculate_present_viewport(
      image.texture.size.width, image.texture.size.height, presentSource.size.width,
      presentSource.size.height);
  float presentAspect = 0.f;
  if (window::get_present_aspect_ratio(presentAspect)) {
    viewport = webgpu::calculate_present_viewport_for_aspect(
        image.texture.size.width, image.texture.size.height, presentAspect);
  }
  wgpu::BindGroup presentBindGroup = presentSource.bindGroup;
  wgpu::BindGroup newMetalFXOutput;
  if (cachedMetalFXOutput && *cachedMetalFXOutput) {
    presentBindGroup = *cachedMetalFXOutput;
  } else if (auto upscaled = upscale_presentation(encoder, presentSource, viewport, metalfxEnabled)) {
    presentBindGroup = std::move(upscaled);
    newMetalFXOutput = presentBindGroup;
  }
  {
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = image.texture.view,
            .loadOp = wgpu::LoadOp::Clear,
            .storeOp = wgpu::StoreOp::Store,
        },
    };
    const wgpu::RenderPassDescriptor renderPassDescriptor{
        .label = "Interpolation snapshot pass",
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
    };
    const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
    pass.SetPipeline(webgpu::g_CopyPipeline);
    pass.SetBindGroup(0, presentBindGroup, 0, nullptr);
    pass.SetViewport(viewport.left, viewport.top, viewport.width, viewport.height,
                     viewport.znear, viewport.zfar);
    pass.Draw(3);
    pass.End();
  }
  image.hasMotion = exportMotion;
  if (exportMotion) {
    encode_snapshot_motion(encoder, image.motion, image.texture.size, viewport, presentSource.efbRect);
  }
  if (includeImGui) {
    const std::array attachments{
        wgpu::RenderPassColorAttachment{
            .view = image.texture.view,
            .loadOp = wgpu::LoadOp::Load,
            .storeOp = wgpu::StoreOp::Store,
        },
    };
    const wgpu::RenderPassDescriptor renderPassDescriptor{
        .label = "Snapshot ImGui pass",
        .colorAttachmentCount = attachments.size(),
        .colorAttachments = attachments.data(),
    };
    const auto pass = encoder.BeginRenderPass(&renderPassDescriptor);
    pass.SetViewport(0.f, 0.f, static_cast<float>(image.texture.size.width),
                     static_cast<float>(image.texture.size.height), 0.f, 1.f);
    imgui::render(pass, image.texture.size.width, image.texture.size.height);
    pass.End();
  }
  return newMetalFXOutput;
}
#endif

void shutdown() {
  stop_frame_worker();
#ifdef AURORA_ENABLE_GX
  stop_presenter();
  g_presentationImagePools = {};
  g_metalfxSlots = {};
  g_metalfxPendingOutput = nullptr;
  g_metalfxNextSlot = 0;
  g_metalfxFailed = false;
  g_metalfxRequested.store(false);
  g_metalfxSupported.store(false);
  g_metalfxStatus.store(AURORA_METALFX_DISABLED);
  imgui::shutdown();
  gfx::shutdown();
  webgpu::shutdown();
#endif
  input::shutdown();
  window::shutdown();
}

const AuroraEvent* update() {
  ZoneScoped;
  if (g_initialFrame) {
    g_initialFrame = false;
    input::initialize();
  }
  return window::poll_events();
}

bool begin_frame_impl(bool pumpEvents, ImGuiFramePolicy imguiPolicy, bool* imguiNewFrameOwed) {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  webgpu::fail_if_device_lost();
  if (pumpEvents) {
    window::pump_events();
  }
  const bool surfaceReconfigurePending =
      g_surfaceReconfigurePending.load(std::memory_order_acquire);
  const bool surfaceMutationRequired =
      surfaceReconfigurePending || webgpu::vsync_change_pending() || !window::is_presentable() || !g_surface ||
      window::native_resize_pending() ||
      !window::native_window_size_matches(
          webgpu::g_graphicsConfig.surfaceConfiguration.width,
          webgpu::g_graphicsConfig.surfaceConfiguration.height);
  if (surfaceMutationRequired) {
    wait_for_presenter_idle();
    window::SurfaceLock surfaceLock;
    // Reconfiguring invalidates any image the presenter holds, so take the surface exclusively; a
    // drained queue does not stop a later job. Order is surface before renderer everywhere.
    std::lock_guard surfaceOwnership(g_surfaceMutex);
    std::lock_guard gpuLock(g_rendererGpuMutex);
    // Surface configuration belongs to the render thread; the SDL thread must not reconfigure Dawn
    // while this worker is acquiring or presenting.
    if (!window::is_presentable()) {
      webgpu::release_surface();
      return false;
    }
    if (window::is_paused()) {
      return false;
    }
    const bool consumeSurfaceReconfigure =
        g_surfaceReconfigurePending.exchange(false, std::memory_order_acq_rel);
    const bool consumeSurfaceRecreate =
        g_surfaceRecreatePending.exchange(false, std::memory_order_acq_rel);
    if (!g_surface || consumeSurfaceReconfigure) {
      // Reconfigure in place unless the surface was actually lost. See
      // g_surfaceRecreatePending for why destroying a live surface here is fatal
      // while a capture overlay is attached.
      webgpu::refresh_surface(consumeSurfaceRecreate);
      if (!g_surface) {
        return false;
      }
    }
    if (window::native_resize_pending() ||
        !window::native_window_size_matches(webgpu::g_graphicsConfig.surfaceConfiguration.width,
                                            webgpu::g_graphicsConfig.surfaceConfiguration.height)) {
      window::sync_frame_buffer_size();
    }
    // A V-Sync change reconfigures the swapchain at this same ordered frame boundary.
    if (webgpu::take_vsync_change()) {
      const auto size = window::get_window_size();
      webgpu::resize_swapchain(size.fb_width, size.fb_height, size.native_fb_width, size.native_fb_height, true);
    }
  } else if (window::is_paused()) {
    return false;
  }

  return begin_frame_render_state_impl(imguiPolicy, imguiNewFrameOwed);
#else
  (void)imguiPolicy;
  (void)imguiNewFrameOwed;
  return true;
#endif
}

bool begin_frame_render_state_impl(ImGuiFramePolicy imguiPolicy, bool* imguiNewFrameOwed) {
#ifdef AURORA_ENABLE_GX
  std::lock_guard gpuLock(g_rendererGpuMutex);
  // Note the debt before gfx::begin_frame() can fail: the synchronous path always started the
  // ImGui frame here, and the runtime's retry loop depends on that pairing.
  if (imguiPolicy == ImGuiFramePolicy::Immediate) {
    imgui::new_frame(window::get_window_size());
  } else if (imguiNewFrameOwed != nullptr) {
    *imguiNewFrameOwed = true;
  }
  if (!gfx::begin_frame()) {
    return false;
  }
#else
  (void)imguiPolicy;
  (void)imguiNewFrameOwed;
#endif
  return true;
}

#ifdef AURORA_ENABLE_GX
// Everything the mutex-free encode phase needs, latched while the renderer GPU mutex is held.
// None of it may be re-read from a global later; the producer has already begun the next frame.
struct SealedFrameContext {
  wgpu::CommandEncoder encoder; // slot 0's encoder; already holds the staging copies
  webgpu::PresentSource presentSource{};
  uint64_t scheduleBaseNanos = 0;
  uint64_t scheduleIntervalNanos = 0;
  uint32_t interpolatedFrameCount = 0;
  uint32_t snapshotWidth = 1;
  uint32_t snapshotHeight = 1;
  uint32_t logicalFrame = 0;
  bool interpolationActive = false;
  bool replayInterpolatedFrames = false;
  bool metalfxEnabled = false;
};

// Phase 1: everything that touches producer-shared renderer state. Needs g_rendererGpuMutex and
// a FIFO already drained into the recorded pass list.
void seal_frame_locked(gfx::SealedFrame& sealedFrame, SealedFrameContext& ctx) {
  ZoneScopedN("Seal frame");
  const auto encoderDescriptor = wgpu::CommandEncoderDescriptor{
      .label = "Redraw encoder",
  };
  ctx.encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
  // Probe-sized CPU-consumed copies read back asynchronously. Their downscale blits push uniforms,
  // so prepare them while the producer's staging buffers are still mapped.
  gfx::efb_ram::seal_async_downloads();
  gfx::end_frame(ctx.encoder);
  gfx::g_stats.presentedFrameCount = 0;
  gfx::g_stats.interpolatedFrameCount = 0;
  // Latched before the producer's next gfx::begin_frame() calls
  // gx::begin_frame_interpolation(), which resets both of these.
  ctx.interpolatedFrameCount = gx::interpolated_frame_count();
  ctx.interpolationActive = ctx.interpolatedFrameCount != 0;
  ctx.replayInterpolatedFrames = ctx.interpolationActive && gx::frame_interpolation_replay_safe();
  ctx.scheduleBaseNanos = g_presentScheduleBaseNanos.load(std::memory_order_acquire);
  ctx.scheduleIntervalNanos = g_presentScheduleIntervalNanos.load(std::memory_order_acquire);
  const auto windowSize = window::get_window_size();
  ctx.snapshotWidth = (std::max)(windowSize.native_fb_width, 1u);
  ctx.snapshotHeight = (std::max)(windowSize.native_fb_height, 1u);
  ctx.logicalFrame = gfx::current_frame();
  ctx.metalfxEnabled = g_metalfxRequested.load();
  // Latched before webgpu::clear_present_source_override() in the producer's
  // next gfx::begin_frame().
  ctx.presentSource = webgpu::current_present_source();
  // ImGui draw lists are built once per frame and replayed by each slot's ImGui pass, which is why
  // the next ImGui frame cannot start until the encode phase is done.
  imgui::render_frame_data();
  // Drop the sealed frame's lazy RAM-readback requests while the producer is still excluded; it
  // starts registering the next frame's as soon as SEALED is published.
  gfx::efb_ram::cancel();
  // Detach the recorded passes. From here the producer's list is empty and the
  // encode phase reads only worker-private state.
  gfx::seal_frame(sealedFrame);
  gfx::expire_bind_group_cache();
}

// Phase 2: encode every presentation slot. Reads only `ctx` and the sealed passes, so it runs
// without the renderer GPU mutex while the producer records the next frame.
std::vector<PresentationJob> encode_sealed_frame(gfx::SealedFrame& sealedFrame, SealedFrameContext& ctx) {
  ZoneScopedN("Encode sealed frame");
  auto encoder = std::move(ctx.encoder);
  const auto encoderDescriptor = wgpu::CommandEncoderDescriptor{
      .label = "Redraw encoder",
  };
  // Absolute slot deadlines: slot k of jobCount presents at base + k * interval / jobCount. With
  // interpolation off, pace to the boundary that just passed plus 6.5 ms; late frames free-run.
  constexpr uint64_t kNativePresentOffsetNanos = 6'500'000;
  const uint32_t presentationJobCount = ctx.interpolatedFrameCount + 1;
  const auto slotPresentDeadline = [&](uint32_t slot) -> PresentClock::time_point {
    if (ctx.scheduleBaseNanos == 0 || ctx.scheduleIntervalNanos == 0) {
      return {};
    }
    if (!ctx.interpolationActive) {
      return PresentClock::time_point{std::chrono::nanoseconds{
          ctx.scheduleBaseNanos - ctx.scheduleIntervalNanos + kNativePresentOffsetNanos}};
    }
    const uint64_t offsetNanos =
        (ctx.scheduleIntervalNanos * static_cast<uint64_t>(slot)) / presentationJobCount;
    return PresentClock::time_point{std::chrono::nanoseconds{ctx.scheduleBaseNanos + offsetNanos}};
  };
  std::vector<PresentationJob> presentationJobs;
  presentationJobs.reserve(presentationJobCount);

  // Each slot is submitted as soon as it is encoded, so the GPU starts slot 0 while slot 1 is still
  // recording. Queue order preserves the ordering the single batched buffer gave.
  const wgpu::CommandBufferDescriptor cmdBufDescriptor{
      .label = "Presentation slot command buffer",
  };
  const auto submitEncodedSlot = [&](wgpu::CommandEncoder& target, bool releaseMetalFXOutput = true) {
    const auto buffer = target.Finish(&cmdBufDescriptor);
    std::lock_guard submitLock(g_queueSubmitMutex);
    g_queue.Submit(1, &buffer);
    if (releaseMetalFXOutput && g_metalfxPendingOutput) {
      if (!g_metalfxPendingOutput->end_output()) metalfx_failed(g_metalfxPendingOutput->error());
      g_metalfxPendingOutput = nullptr;
    }
  };

  if (ctx.replayInterpolatedFrames) {
    for (uint32_t interpolatedFrame = 0; interpolatedFrame < ctx.interpolatedFrameCount;
         ++interpolatedFrame) {
      gfx::render(sealedFrame, encoder, static_cast<int32_t>(interpolatedFrame), false);
      auto image =
          acquire_presentation_image(interpolatedFrame, ctx.snapshotWidth, ctx.snapshotHeight);
      encode_presentation_snapshot(encoder, ctx.presentSource, *image, true, ctx.metalfxEnabled);
      presentationJobs.push_back({
          .image = std::move(image),
          .logicalFrame = ctx.logicalFrame,
          .presentAt = slotPresentDeadline(interpolatedFrame),
          .interpolated = true,
      });
      submitEncodedSlot(encoder);
      encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
    }
  }

  // A demanded CPU-visible EFB readback submits a prefix of the frame, so replaying the resumed
  // stream would mutate an already-rendered EFB. Render once, then duplicate into the slots.
  gfx::render(sealedFrame, encoder, -1, true);
  auto pendingMotionDump = encode_motion_dump(encoder);
  const bool exportMotion = motion_export_active() && webgpu::g_motionVectors.texture &&
                            webgpu::g_motionDepth.texture && webgpu::g_motionMark.texture &&
                            webgpu::g_motionTransparency.texture;
  if (motion_view_mode() != 0) {
    ctx.presentSource =
        encode_motion_view(encoder, ctx.presentSource, encode_display_motion(encoder, ctx.presentSource));
  }
  // The copy targets now hold this frame's resolves, so queue their readbacks on the same encoder;
  // completion is harvested in gfx::after_submit, never waited on here.
  gfx::efb_ram::encode_async_downloads(encoder);
  wgpu::BindGroup duplicatedMetalFXOutput;
  if (!ctx.replayInterpolatedFrames) {
    for (uint32_t interpolatedFrame = 0; interpolatedFrame < ctx.interpolatedFrameCount;
         ++interpolatedFrame) {
      auto image =
          acquire_presentation_image(interpolatedFrame, ctx.snapshotWidth, ctx.snapshotHeight);
      const auto newMetalFXOutput = encode_presentation_snapshot(
          encoder, ctx.presentSource, *image, true, ctx.metalfxEnabled,
          duplicatedMetalFXOutput ? &duplicatedMetalFXOutput : nullptr, exportMotion);
      if (!duplicatedMetalFXOutput && newMetalFXOutput) {
        duplicatedMetalFXOutput = newMetalFXOutput;
      }
      presentationJobs.push_back({
          .image = std::move(image),
          .logicalFrame = ctx.logicalFrame,
          .presentAt = slotPresentDeadline(interpolatedFrame),
          .interpolated = true,
          .duplicated = true,
      });
      submitEncodedSlot(encoder, false);
      encoder = g_device.CreateCommandEncoder(&encoderDescriptor);
    }
  }
  auto finalImage =
      acquire_presentation_image(ctx.interpolatedFrameCount, ctx.snapshotWidth, ctx.snapshotHeight);
  encode_presentation_snapshot(encoder, ctx.presentSource, *finalImage, true, ctx.metalfxEnabled,
                               duplicatedMetalFXOutput ? &duplicatedMetalFXOutput : nullptr, exportMotion);
  if (exportMotion) {
    encode_previous_motion_depth(encoder);
  }
  auto pendingFrameCapture =
      encode_frame_capture(encoder, ctx.presentSource, ctx.snapshotWidth, ctx.snapshotHeight);
  encode_video_frame(encoder, ctx.presentSource, ctx.snapshotWidth, ctx.snapshotHeight);
  presentationJobs.push_back({
      .image = std::move(finalImage),
      .logicalFrame = ctx.logicalFrame,
      .presentAt = slotPresentDeadline(ctx.interpolatedFrameCount),
      .interpolated = false,
  });
  submitEncodedSlot(encoder);

  // A group that finished encoding past its anchor slides forward by whole display periods, never
  // per slot. The cursor keeps two groups off one anchor, which bursts then holds for a period.
  static PresentClock::time_point s_lastGroupAnchor{};
  if (ctx.interpolationActive && ctx.scheduleIntervalNanos != 0 && !presentationJobs.empty() &&
      presentationJobs.front().presentAt != PresentClock::time_point{}) {
    const std::chrono::nanoseconds interval{static_cast<int64_t>(ctx.scheduleIntervalNanos)};
    auto anchor = presentationJobs.front().presentAt;
    const auto now = PresentClock::now();
    if (now > anchor) {
      const auto behind = std::chrono::duration_cast<std::chrono::nanoseconds>(now - anchor);
      const uint64_t periods =
          static_cast<uint64_t>(behind.count()) / ctx.scheduleIntervalNanos + 1u;
      anchor += std::chrono::nanoseconds{static_cast<int64_t>(periods * ctx.scheduleIntervalNanos)};
    }
    if (s_lastGroupAnchor != PresentClock::time_point{} && anchor <= s_lastGroupAnchor) {
      anchor = s_lastGroupAnchor + interval;
    }
    const auto shift =
        std::chrono::duration_cast<std::chrono::nanoseconds>(anchor - presentationJobs.front().presentAt);
    if (shift.count() > 0) {
      const uint32_t slidPeriods = static_cast<uint32_t>(
          (static_cast<uint64_t>(shift.count()) + ctx.scheduleIntervalNanos - 1u) /
          ctx.scheduleIntervalNanos);
      for (auto& job : presentationJobs) {
        job.presentAt += shift;
        job.slidPeriods = slidPeriods;
      }
    }
    s_lastGroupAnchor = anchor;
  } else {
    // No schedule (interpolation off, boot/black presents): the grid is gone,
    // so the cursor must not constrain the next scheduled group.
    s_lastGroupAnchor = {};
  }

  start_video_readbacks();
  if (pendingFrameCapture.has_value()) {
    complete_frame_capture(*pendingFrameCapture);
  }
  if (pendingMotionDump.has_value()) {
    complete_motion_dump(*pendingMotionDump);
  }
  gfx::after_submit();
  gfx::g_stats.presentedFrameCount = static_cast<uint32_t>(presentationJobs.size());
  gfx::g_stats.interpolatedFrameCount = ctx.interpolatedFrameCount;
  return presentationJobs;
}

// Phase 3: hand the encoded group to whoever owns presentation.
void publish_presentations(std::vector<PresentationJob>&& presentationJobs, bool interpolationActive) {
#if defined(__APPLE__)
  (void)interpolationActive;
  // Presenting reaches SDL/AppKit, whose window operations must stay on the
  // main thread. Interpolation normally starts the presenter worker, so keep
  // its jobs synchronous on Apple platforms.
  for (const auto& job : presentationJobs) {
    present_presentation_job(job);
  }
#else
  // Keep presentation on the presenter whenever the async frame worker runs, even with
  // interpolation off, so every mode shares one surface/resize path. RenderDoc keeps the sync path.
  if (frame_worker_requested() || interpolationActive ||
      g_presenterStarted.load(std::memory_order_acquire)) {
    enqueue_presentations(std::move(presentationJobs));
  } else {
    for (const auto& job : presentationJobs) {
      present_presentation_job(job);
    }
  }
#endif
}

void record_frame_telemetry() {
  TracyPlotConfig("aurora: lastVertSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastUniformSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastIndexSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastStorageSize", tracy::PlotFormatType::Memory, false, true, 0);
  TracyPlotConfig("aurora: lastTextureUploadSize", tracy::PlotFormatType::Memory, false, true, 0);

  TracyPlot("aurora: queuedPipelines", static_cast<int64_t>(gfx::g_stats.queuedPipelines));
  TracyPlot("aurora: createdPipelines", static_cast<int64_t>(gfx::g_stats.createdPipelines));
  TracyPlot("aurora: drawCallCount", static_cast<int64_t>(gfx::g_stats.drawCallCount));
  TracyPlot("aurora: mergedDrawCallCount", static_cast<int64_t>(gfx::g_stats.mergedDrawCallCount));
  TracyPlot("aurora: lastVertSize", static_cast<int64_t>(gfx::g_stats.lastVertSize));
  TracyPlot("aurora: lastUniformSize", static_cast<int64_t>(gfx::g_stats.lastUniformSize));
  TracyPlot("aurora: lastIndexSize", static_cast<int64_t>(gfx::g_stats.lastIndexSize));
  TracyPlot("aurora: lastStorageSize", static_cast<int64_t>(gfx::g_stats.lastStorageSize));
  TracyPlot("aurora: lastTextureUploadSize", static_cast<int64_t>(gfx::g_stats.lastTextureUploadSize));
  TracyPlot("aurora: frameIndex", static_cast<int64_t>(gfx::current_frame()));
#if defined(TRACY_ENABLE) && defined(_WIN32)
  const auto fileTimeValue = [](const FILETIME& value) noexcept {
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
  };
  FILETIME creation{}, exit{}, processKernel{}, processUser{}, threadKernel{}, threadUser{};
  const bool processTimesAvailable =
      GetProcessTimes(GetCurrentProcess(), &creation, &exit, &processKernel, &processUser) != FALSE;
  const bool threadTimesAvailable =
      GetThreadTimes(GetCurrentThread(), &creation, &exit, &threadKernel, &threadUser) != FALSE;
  const uint64_t processCpu100ns =
      processTimesAvailable ? fileTimeValue(processKernel) + fileTimeValue(processUser) : 0;
  const uint64_t threadCpu100ns =
      threadTimesAvailable ? fileTimeValue(threadKernel) + fileTimeValue(threadUser) : 0;
  static uint64_t previousProcessCpu100ns = processCpu100ns;
  static uint64_t previousThreadCpu100ns = threadCpu100ns;
  TracyPlot("aurora: processCpuUsPerFrame",
            static_cast<int64_t>((processCpu100ns - previousProcessCpu100ns) / 10));
  TracyPlot("aurora: mainThreadCpuUsPerFrame",
            static_cast<int64_t>((threadCpu100ns - previousThreadCpu100ns) / 10));
  previousProcessCpu100ns = processCpu100ns;
  previousThreadCpu100ns = threadCpu100ns;
#endif
  FrameMarkNamed("Aurora frame");
}

// One complete frame-worker cycle. The scene encode only leaves the renderer mutex when
// interpolation actually inserts slots; otherwise both phases publish together.
bool run_frame_worker_cycle(gfx::SealedFrame& sealedFrame) {
  ZoneScopedN("Frame worker cycle");
  webgpu::fail_if_device_lost();
  SealedFrameContext ctx;
  std::vector<PresentationJob> presentationJobs;
  bool overlapEncode = false;
  // Stage times of a cycle, logged when it runs long (the producer waits on it in GXDrawDone).
  using WorkerClock = std::chrono::steady_clock;
  const auto cycleStart = WorkerClock::now();
  WorkerClock::time_point sealed{}, encoded{};
  {
    std::lock_guard gpuLock(g_rendererGpuMutex);
    seal_frame_locked(sealedFrame, ctx);
    sealed = WorkerClock::now();
    overlapEncode = ctx.interpolationActive;
    if (!overlapEncode) {
      presentationJobs = encode_sealed_frame(sealedFrame, ctx);
    }
    encoded = WorkerClock::now();
  }
  if (!overlapEncode) {
    publish_presentations(std::move(presentationJobs), ctx.interpolationActive);
  }
  const auto published = WorkerClock::now();

  {
    std::unique_lock lock(g_frameWorker.mutex);
    g_frameWorker.cv.wait(lock, [] { return g_frameWorker.stop || g_frameWorker.prepareAllowed; });
    if (g_frameWorker.stop) {
      return false;
    }
    g_frameWorker.prepareAllowed = false;
  }

  // Preparing the next frame belongs to the SEALED phase: without a fresh pass 0 and mapped
  // staging buffers the producer's drain has nowhere to put its commands.
  bool imguiNewFrameOwed = false;
  const auto prepareStart = WorkerClock::now();
  const bool prepared = begin_frame_impl(
      false, overlapEncode ? ImGuiFramePolicy::Deferred : ImGuiFramePolicy::Immediate,
      &imguiNewFrameOwed);
  {
    const auto prepareEnd = WorkerClock::now();
    const auto msOf = [](WorkerClock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
    const double busy = msOf(published - cycleStart) + msOf(prepareEnd - prepareStart);
    static WorkerClock::time_point s_window{};
    static int s_logged = 0;
    if (prepareEnd - s_window > std::chrono::minutes(1)) {
      s_window = prepareEnd;
      s_logged = 0;
    }
    if (busy >= 16.7 && s_logged++ < 30) {
      Log.info("Slow render frame: {:.1f} ms (seal {:.1f}, encode {:.1f}, publish {:.1f}, prepare next {:.1f}; "
               "{} pipelines queued)",
               busy, msOf(sealed - cycleStart), msOf(encoded - sealed), msOf(published - encoded),
               msOf(prepareEnd - prepareStart), gfx::g_stats.queuedPipelines);
    }
  }

  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.framePrepared = prepared;
    g_frameWorker.sealed.store(true, std::memory_order_release);
    if (!overlapEncode) {
      g_frameWorker.ready.store(true, std::memory_order_release);
    }
  }
  g_frameWorker.cv.notify_all();

  if (overlapEncode) {
    // Mutex-free: the producer drains and records the next frame in parallel, taking the renderer
    // mutex per drain, and this phase never takes it.
    presentationJobs = encode_sealed_frame(sealedFrame, ctx);
    publish_presentations(std::move(presentationJobs), ctx.interpolationActive);
    if (imguiNewFrameOwed) {
      // Safe only now: every slot has replayed this frame's ImGui draw lists.
      std::lock_guard gpuLock(g_rendererGpuMutex);
      imgui::new_frame(window::get_window_size());
    }
    {
      std::lock_guard lock(g_frameWorker.mutex);
      g_frameWorker.ready.store(true, std::memory_order_release);
    }
    g_frameWorker.cv.notify_all();
  }

  record_frame_telemetry();
  return true;
}
#endif

// Synchronous frame submission: seal, encode and present inline on the calling thread. Used when
// the frame worker is disabled (RenderDoc captures) and on the boot path.
void end_frame_impl(bool pumpEvents, bool drainFifo) {
  ZoneScoped;
#ifdef AURORA_ENABLE_GX
  webgpu::fail_if_device_lost();
  if (pumpEvents) {
    window::pump_events();
  }
  gfx::SealedFrame sealedFrame;
  SealedFrameContext ctx;
  std::vector<PresentationJob> presentationJobs;
  if (drainFifo) gx::fifo::drain();
  {
    std::lock_guard gpuLock(g_rendererGpuMutex);
    seal_frame_locked(sealedFrame, ctx);
    presentationJobs = encode_sealed_frame(sealedFrame, ctx);
  }
  publish_presentations(std::move(presentationJobs), ctx.interpolationActive);
  record_frame_telemetry();
#else
  (void)pumpEvents;
  (void)drainFifo;
#endif
}

bool begin_frame() {
  if (window_owner::enabled()) window_owner::check();
#ifdef AURORA_ENABLE_GX
  // The asynchronous fast path below can return a logically prepared frame
  // without entering begin_frame_impl(), so loss must be checked before it.
  webgpu::fail_if_device_lost();
#endif
  if (!frame_worker_requested()) {
    return begin_frame_impl(true);
  }

  ensure_frame_worker_started();
  // SDL needs event pumping on the window-owning producer thread, and the worker passes
  // pumpEvents=false, so keep it here even when the fast path returns early.
  window::pump_events();
  bool waitForSurfacePreparation = false;
#ifdef AURORA_ENABLE_GX
  // A surface mutation can legitimately fail preparation, and optimistic success would let GX/ImGui
  // record into a frame that was never begun, so join this path and return its real result.
  waitForSurfacePreparation =
      !window::is_presentable() || !g_surface ||
      window::native_resize_pending() || window::is_paused() ||
      !window::native_window_size_matches(
          webgpu::g_graphicsConfig.surfaceConfiguration.width,
          webgpu::g_graphicsConfig.surfaceConfiguration.height);
#endif
  bool workerPreparationPending = false;
  {
    std::lock_guard lock(g_frameWorker.mutex);
    // The runtime begins right after an asynchronous end, so treat the worker's pending begin as an
    // active frame unless a resize needs the real preparation result.
    if (!g_frameWorker.ready.load(std::memory_order_acquire)) {
      g_frameWorker.prepareAllowed = true;
      g_frameWorker.cv.notify_one();
      if (!waitForSurfacePreparation) {
        return true;
      }
      workerPreparationPending = true;
    } else if (g_frameWorker.framePrepared) {
      return true;
    }
  }

  if (workerPreparationPending) {
    // DONE, not SEALED: this only runs while the window is changing and the caller is about to act on
    // the surface, so keep the resize path fully serialized.
    wait_for_frame_worker_private(FrameWorkerPhase::Done);
    std::lock_guard lock(g_frameWorker.mutex);
    return g_frameWorker.framePrepared;
  }

  {
    std::lock_guard lock(g_frameWorker.mutex);
    if (g_frameWorker.framePrepared) {
      return true;
    }
  }

  const bool prepared = begin_frame_impl(false);
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.framePrepared = prepared;
  }
  return prepared;
}

void end_frame() {
#ifdef AURORA_ENABLE_GX
  webgpu::fail_if_device_lost();
#endif
  if (!frame_worker_requested()) {
    end_frame_impl(true, true);
    return;
  }

  ensure_frame_worker_started();
  // DONE: this seals another frame, which means reusing the worker's encoder
  // state and its SealedFrame. The previous cycle must be completely finished.
  wait_for_frame_worker_private(FrameWorkerPhase::Done);

  // Seal all current GX work on the CPU while the renderer is known ready.
  // Later FIFO writes belong exclusively to the next frame.
  gx::fifo::drain();
  {
    std::lock_guard lock(g_frameWorker.mutex);
    g_frameWorker.framePrepared = false;
    g_frameWorker.sealed.store(false, std::memory_order_release);
    g_frameWorker.ready.store(false, std::memory_order_release);
    g_frameWorker.jobPending = true;
    g_frameWorker.prepareAllowed = false;
  }
  g_frameWorker.cv.notify_one();
}
} // namespace

void wait_for_frame_worker() {
  wait_for_frame_worker_private(FrameWorkerPhase::Done);
  if (window_owner::enabled()) window_owner::check();
}
std::chrono::nanoseconds wait_for_frame_worker_sealed() {
  if (window_owner::enabled()) window_owner::check();
  if (g_frameWorker.sealed.load(std::memory_order_acquire)) {
    return std::chrono::nanoseconds::zero();
  }
  const auto started = std::chrono::steady_clock::now();
  wait_for_frame_worker_private(FrameWorkerPhase::Sealed);
  if (window_owner::enabled()) window_owner::check();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
}
bool wait_for_frame_worker_for(std::chrono::microseconds timeout) {
  const bool result = wait_for_frame_worker_private_for(FrameWorkerPhase::Done, timeout);
  if (window_owner::enabled()) window_owner::check();
  return result;
}
std::recursive_mutex& renderer_gpu_mutex() noexcept { return g_rendererGpuMutex; }
void submit_staging_commands(const wgpu::CommandBuffer& commands) {
  std::lock_guard submitLock(g_queueSubmitMutex);
  webgpu::g_queue.Submit(1, &commands);
}
} // namespace aurora

// C API bindings
AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config) {
  return aurora::initialize(argc, argv, *config);
}
void aurora_shutdown() { aurora::shutdown(); }
const AuroraEvent* aurora_update() { return aurora::update(); }
bool aurora_begin_frame() { return aurora::begin_frame(); }
void aurora_end_frame() { aurora::end_frame(); }
void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback callback) {
  aurora::g_frameWorkerWaitCallback.store(callback, std::memory_order_release);
}
void aurora_wait_for_frame_worker() { aurora::wait_for_frame_worker(); }
bool aurora_wait_for_frame_worker_for(uint32_t timeoutMicros) {
  return aurora::wait_for_frame_worker_for(std::chrono::microseconds(timeoutMicros));
}
void aurora_set_present_schedule(uint64_t baseNanos, uint64_t intervalNanos) {
  aurora::g_presentScheduleBaseNanos.store(baseNanos, std::memory_order_release);
  aurora::g_presentScheduleIntervalNanos.store(intervalNanos, std::memory_order_release);
}
void aurora_report_producer_paced(bool paced) {
#ifdef AURORA_ENABLE_GX
  aurora::gx::report_producer_paced(paced);
#else
  (void)paced;
#endif
}
void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* diagnostics) {
  if (diagnostics != nullptr) {
    aurora::gx::get_frame_interpolation_diagnostics(*diagnostics);
  }
}

void aurora_get_present_timing(AuroraPresentTiming* timing) {
  if (timing != nullptr) {
    *timing = aurora::snapshot_present_timing();
  }
}
void aurora_set_vsync(bool enabled) { aurora::webgpu::request_vsync(enabled); }
void aurora_set_hdr(bool enabled, float paperWhiteNits, float peakNits) {
  aurora::webgpu::request_hdr(enabled, paperWhiteNits, peakNits);
}
bool aurora_get_vsync() { return aurora::webgpu::vsync_enabled(); }
void aurora_set_frame_interpolation_fps(uint32_t targetFps) {
#ifdef AURORA_ENABLE_GX
  aurora::gx::set_frame_interpolation_fps(targetFps);
#else
  (void)targetFps;
#endif
}
uint32_t aurora_get_frame_interpolation_fps() {
#ifdef AURORA_ENABLE_GX
  return aurora::gx::frame_interpolation_fps();
#else
  return 0;
#endif
}
void aurora_request_frame_capture(uint32_t frame, const char* outputPath) {
  {
    std::lock_guard lock(aurora::g_captureMutex);
    aurora::g_captureOutputPath = outputPath != nullptr ? outputPath : "frame_capture.bmp";
    aurora::g_captureScreenshot = false;
  }
  aurora::g_captureFrame.store(frame, std::memory_order_release);
}
void aurora_request_screenshot(const char* outputPath) {
  if (outputPath == nullptr) return;
  {
    std::lock_guard lock(aurora::g_captureMutex);
    aurora::g_captureOutputPath = outputPath;
    aurora::g_captureScreenshot = true;
  }
  aurora::g_captureFrame.store(aurora::kCaptureNextFrame, std::memory_order_release);
}
int aurora_take_screenshot_result() {
  return aurora::g_screenshotResult.exchange(0, std::memory_order_acq_rel);
}
void aurora_set_capture_overlay(AuroraCaptureOverlay overlay) {
  aurora::g_captureOverlay.store(overlay, std::memory_order_release);
}
bool aurora_set_video_capture(AuroraVideoFrameCallback callback, void* user, uint32_t maxHeight) {
#ifdef AURORA_ENABLE_GX
  if (callback != nullptr &&
      !aurora::capture_format_supported(aurora::webgpu::g_graphicsConfig.surfaceConfiguration.format)) {
    return false;
  }
  std::lock_guard lock(aurora::g_videoMutex);
  aurora::g_videoCallback = callback;
  aurora::g_videoUser = user;
  aurora::g_videoMaxHeight = maxHeight != 0 ? maxHeight : 1080;
  return true;
#else
  (void)callback;
  (void)user;
  (void)maxHeight;
  return false;
#endif
}
bool aurora_flush_efb_copies_to_ram() {
#ifdef AURORA_ENABLE_GX
  if (!aurora::gfx::efb_ram::has_pending()) {
    return true;
  }
  // This finalizes the frame still being recorded, on the producer thread, so join the whole cycle
  // first: the encode phase owns the previous passes, EFB targets and image pool.
  aurora::wait_for_frame_worker();
  // The renderer is about to submit a prefix of the active frame. Its resumed
  // suffix cannot safely be replayed against the same mutable EFB resources.
  aurora::gx::mark_frame_interpolation_replay_unsafe();
  aurora::gx::fifo::drain();
  if (!aurora::gfx::efb_ram::prepare_downloads()) return false;
  const wgpu::CommandEncoderDescriptor encoderDescriptor{
      .label = "GX CPU-visible EFB copy encoder",
  };
  auto encoder = aurora::webgpu::g_device.CreateCommandEncoder(&encoderDescriptor);
  aurora::gfx::end_batch(encoder);
  aurora::gfx::render(encoder);
  aurora::gfx::efb_ram::encode_downloads(encoder);
  const wgpu::CommandBufferDescriptor commandDescriptor{
      .label = "GX CPU-visible EFB copy command buffer",
  };
  const auto commandBuffer = encoder.Finish(&commandDescriptor);
  {
    std::lock_guard submitLock(aurora::g_queueSubmitMutex);
    aurora::webgpu::g_queue.Submit(1, &commandBuffer);
  }
  const bool copied = aurora::gfx::efb_ram::complete_downloads();
  aurora::gfx::after_submit();
  const bool resumed = aurora::gfx::resume_frame();
  return copied && resumed;
#else
  return true;
#endif
}
bool aurora_flush_efb_copy_to_ram(void* dest) {
#ifdef AURORA_ENABLE_GX
  if (dest == nullptr || !aurora::gfx::efb_ram::has_pending(dest)) {
    return false;
  }

  // See aurora_flush_efb_copies_to_ram: this encodes the in-progress frame on
  // the producer thread, so the worker's overlapped encode has to be finished.
  aurora::wait_for_frame_worker();
  // Preserve the requested output cadence by duplicating the completed native
  // image instead of replaying this split frame.
  aurora::gx::mark_frame_interpolation_replay_unsafe();
  aurora::gx::fifo::drain();
  if (!aurora::gfx::efb_ram::prepare_downloads(dest)) return false;
  const wgpu::CommandEncoderDescriptor encoderDescriptor{
      .label = "GX demanded EFB copy encoder",
  };
  auto encoder = aurora::webgpu::g_device.CreateCommandEncoder(&encoderDescriptor);
  aurora::gfx::end_batch(encoder);
  aurora::gfx::render(encoder);
  aurora::gfx::efb_ram::encode_downloads(encoder, dest);
  const wgpu::CommandBufferDescriptor commandDescriptor{
      .label = "GX demanded EFB copy command buffer",
  };
  const auto commandBuffer = encoder.Finish(&commandDescriptor);
  {
    std::lock_guard submitLock(aurora::g_queueSubmitMutex);
    aurora::webgpu::g_queue.Submit(1, &commandBuffer);
  }
  const bool copied = aurora::gfx::efb_ram::complete_downloads();
  aurora::gfx::after_submit();
  const bool resumed = aurora::gfx::resume_frame();
  return copied && resumed;
#else
  (void)dest;
  return true;
#endif
}
AuroraBackend aurora_get_backend() { return aurora::g_config.desiredBackend; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  if (count != nullptr) {
    *count = aurora::PreferredBackendOrder.size();
  }
  return aurora::PreferredBackendOrder.data();
}
void aurora_set_log_level(AuroraLogLevel level) { aurora::g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { aurora::g_config.pauseOnFocusLost = value; }
void aurora_set_disable_copy_filter(bool disabled) { aurora::g_config.disableCopyFilter = disabled; }
bool aurora_get_disable_copy_filter() { return aurora::g_config.disableCopyFilter; }
void aurora_set_background_input(bool value) {
  aurora::g_config.allowJoystickBackgroundEvents = value;
  aurora::window::set_background_input(value);
}
void aurora_set_display_mode(AuroraDisplayMode mode) { aurora::window::set_display_mode(mode); }
AuroraDisplayMode aurora_get_display_mode() { return aurora::window::get_display_mode(); }
uint32_t aurora_get_monitors(AuroraMonitor* monitors, uint32_t maxCount) {
  return aurora::window::get_monitors(monitors, maxCount);
}
void aurora_set_fullscreen_monitor(const char* id) { aurora::window::set_fullscreen_monitor(id); }
bool aurora_motion_vectors_active() {
#ifdef AURORA_ENABLE_GX
  return aurora::gfx::motion_vectors_active();
#else
  return false;
#endif
}
void aurora_set_motion_vector_view(int mode, float scale) {
#ifdef AURORA_ENABLE_GX
  aurora::g_motionViewMode.store(std::clamp(mode, 0, 2), std::memory_order_release);
  if (scale > 0.f) aurora::g_motionViewScale.store(scale);
#endif
}
int aurora_get_motion_vector_view() {
#ifdef AURORA_ENABLE_GX
  return aurora::motion_view_mode();
#else
  return 0;
#endif
}
float aurora_get_motion_vector_view_scale() {
#ifdef AURORA_ENABLE_GX
  return aurora::g_motionViewScale.load();
#else
  return 16.f;
#endif
}
void aurora_set_cutout_mode(int mode) {
#ifdef AURORA_ENABLE_GX
  if (mode >= 0 && mode <= static_cast<int>(aurora::gx::CutoutMode::TwoPassAll))
    aurora::gx::set_cutout_mode(static_cast<aurora::gx::CutoutMode>(mode));
#endif
}
int aurora_get_cutout_mode() {
#ifdef AURORA_ENABLE_GX
  return static_cast<int>(aurora::gx::cutout_mode());
#else
  return 0;
#endif
}
void aurora_set_metalfx_spatial(bool enabled) { aurora::g_metalfxRequested.store(enabled); }
void aurora_set_button_icons(int family, const char* const buttons[4]) {
#ifdef AURORA_ENABLE_GX
  aurora::gfx::texture_replacement::ButtonIcons icons{.family = family};
  for (size_t i = 0; buttons != nullptr && i < icons.buttons.size(); ++i) {
    if (buttons[i] != nullptr) icons.buttons[i] = buttons[i];
  }
  aurora::gfx::texture_replacement::set_button_icons(icons);
#endif
}
size_t aurora_get_button_icon_folder(char* out, size_t size) {
  std::string folder;
#ifdef AURORA_ENABLE_GX
  folder = aurora::gfx::texture_replacement::button_icon_folder();
#endif
  if (out != nullptr && size > 0) {
    const size_t n = std::min(folder.size(), size - 1);
    std::memcpy(out, folder.data(), n);
    out[n] = '\0';
  }
  return folder.size();
}
bool aurora_texture_replacement_used(const char* name) {
#ifdef AURORA_ENABLE_GX
  return aurora::gfx::texture_replacement::replacement_used(name);
#else
  return false;
#endif
}
void aurora_texture_replacement_reset_used() {
#ifdef AURORA_ENABLE_GX
  aurora::gfx::texture_replacement::reset_used();
#endif
}
void aurora_texture_replacement_highlight(const char* name) {
#ifdef AURORA_ENABLE_GX
  aurora::gfx::texture_replacement::set_highlight(name != nullptr ? name : "");
#endif
}
void aurora_texture_log(const char* scene) {
#ifdef AURORA_ENABLE_GX
  aurora::gfx::texture_replacement::set_texture_log_scene(scene != nullptr ? scene : "");
#endif
}
bool aurora_get_metalfx_spatial() { return aurora::g_metalfxRequested.load(); }
bool aurora_is_metalfx_spatial_supported() { return aurora::g_metalfxSupported.load(); }
AuroraMetalFXStatus aurora_get_metalfx_status() { return aurora::g_metalfxStatus.load(); }
