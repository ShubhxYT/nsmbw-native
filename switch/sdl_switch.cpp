// SDL3 surface for the Switch build, implemented on libnx.
//
// Aurora and the runtime are written against SDL3, which has no Switch backend. Rather than stub
// the calls out, this provides the parts a console actually needs: one fullscreen window, one
// virtual gamepad driven by HidNpad, a clock, an event queue, and the filesystem paths the runtime
// uses for its data. Everything else (renderer, audio stream, mouse, keyboard text input) is
// present and harmless so the runtime's feature detection sees a coherent platform.
//
// Signatures are taken from the real SDL3 headers, so this file is checked against them.

#include "data_root.h"

#include <SDL3/SDL.h>

#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" NWindow* switchGetMainWindow();

// ---------------------------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------------------------

namespace {

constexpr uint32_t kWindowWidth = 1280;
constexpr uint32_t kWindowHeight = 720;
constexpr SDL_DisplayID kDisplayId = 1;

std::atomic<bool> g_initialized{false};
std::string g_error;
std::mutex g_eventMutex;
std::deque<SDL_Event> g_events;
std::vector<SDL_EventFilter> g_eventFilters;
std::mutex g_padMutex;
std::once_flag g_padInit;
PadState g_pad;
uint64_t g_startTicks = 0;
uint64_t g_eventWatchUserId = 0;

// The single virtual gamepad. libnx exposes one handheld/controller pair through PadState; the rest
// of the console's controllers would each need their own PadState, which the port adds later.
constexpr SDL_JoystickID kGamepadId = 1;
bool g_padConnected = false;
uint32_t g_buttons = 0;
std::array<int16_t, SDL_GAMEPAD_AXIS_COUNT> g_axes{};

void set_error(const char* message) {
  g_error = message ? message : "";
  SDL_Log("%s", g_error.c_str());
}

uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void push_event(const SDL_Event& event) {
  std::lock_guard lock(g_eventMutex);
  g_events.push_back(event);
}

// libnx button bits -> SDL gamepad buttons.
struct ButtonMap {
  uint64_t hid;
  SDL_GamepadButton button;
};

constexpr ButtonMap kButtonMap[] = {
    {HidNpadButton_A, SDL_GAMEPAD_BUTTON_EAST},
    {HidNpadButton_B, SDL_GAMEPAD_BUTTON_SOUTH},
    {HidNpadButton_X, SDL_GAMEPAD_BUTTON_NORTH},
    {HidNpadButton_Y, SDL_GAMEPAD_BUTTON_WEST},
    {HidNpadButton_StickL, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {HidNpadButton_StickR, SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {HidNpadButton_L, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {HidNpadButton_R, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {HidNpadButton_Plus, SDL_GAMEPAD_BUTTON_START},
    {HidNpadButton_Minus, SDL_GAMEPAD_BUTTON_BACK},
    {HidNpadButton_Up, SDL_GAMEPAD_BUTTON_DPAD_UP},
    {HidNpadButton_Down, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {HidNpadButton_Left, SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {HidNpadButton_Right, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
};

uint32_t map_buttons(uint64_t held) {
  uint32_t mask = 0;
  for (const auto& entry : kButtonMap) {
    if ((held & entry.hid) != 0) {
      mask |= 1u << entry.button;
    }
  }
  return mask;
}

int16_t axis_from_stick(int32_t value, int32_t deadzone) {
  if (value > -deadzone && value < deadzone) {
    return 0;
  }
  return static_cast<int16_t>(std::clamp(value, -32767, 32767));
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Error handling, hints, clock
// ---------------------------------------------------------------------------------------------

extern "C" {

void SDL_Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

const char* SDL_GetError(void) { return g_error.c_str(); }

bool SDL_SetError(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  char buffer[512];
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  g_error = buffer;
  return false;
}

bool SDL_ClearError(void) {
  g_error.clear();
  return true;
}

bool SDL_SetHint(const char* name, const char* value) {
  (void)name;
  (void)value;
  return true;
}

const char* SDL_GetHint(const char* name) {
  (void)name;
  return nullptr;
}

uint64_t SDL_GetTicks(void) { return (now_ns() - g_startTicks) / 1000000ull; }

uint64_t SDL_GetTicksNS(void) { return now_ns() - g_startTicks; }

uint64_t SDL_GetPerformanceCounter(void) { return now_ns(); }

uint64_t SDL_GetPerformanceFrequency(void) { return 1000000000ull; }

void SDL_Delay(uint32_t ms) { svcSleepThread(static_cast<int64_t>(ms) * 1000000ll); }

void SDL_DelayNS(uint64_t ns) { svcSleepThread(static_cast<int64_t>(ns)); }

bool SDL_SetCurrentThreadPriority(SDL_ThreadPriority priority) {
  (void)priority;
  return true;
}

const char* SDL_GetPlatform(void) { return "Switch"; }

const char* SDL_GetCurrentVideoDriver(void) { return "switch"; }

const char* SDL_getenv(const char* name) {
  (void)name;
  return nullptr;
}

char* SDL_strstr(const char* haystack, const char* needle) {
  return const_cast<char*>(std::strstr(haystack, needle));
}

void SDL_free(void* mem) { std::free(mem); }

void* SDL_malloc(size_t size) { return std::malloc(size); }

void* SDL_calloc(size_t nmemb, size_t size) { return std::calloc(nmemb, size); }

void* SDL_realloc(void* mem, size_t size) { return std::realloc(mem, size); }

char* SDL_strdup(const char* str) { return str ? ::strdup(str) : nullptr; }

bool SDL_DisableScreenSaver(void) { return true; }
bool SDL_EnableScreenSaver(void) { return true; }

void SDL_SetLogOutputFunction(SDL_LogOutputFunction callback, void* userdata) {
  (void)callback;
  (void)userdata;
}

void SDL_SetLogPriorities(SDL_LogPriority priority) { (void)priority; }

void SDL_SetLogPriority(int category, SDL_LogPriority priority) {
  (void)category;
  (void)priority;
}

SDL_LogPriority SDL_GetLogPriority(int category) {
  (void)category;
  return SDL_LOG_PRIORITY_INFO;
}

// ---------------------------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------------------------

bool SDL_Init(uint32_t flags) { return SDL_InitSubSystem(flags); }

bool SDL_InitSubSystem(uint32_t flags) {
  (void)flags;
  if (g_initialized.load()) {
    return true;
  }
  g_startTicks = now_ns();
  // Console services are brought up by libnx before main() runs; nothing else to do here.
  g_initialized.store(true);
  return true;
}

void SDL_QuitSubSystem(uint32_t flags) { (void)flags; }

void SDL_Quit(void) {
  g_initialized.store(false);
  std::lock_guard lock(g_padMutex);
  g_padConnected = false;
  g_buttons = 0;
  g_axes.fill(0);
}

SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) {
  (void)flags;
  return g_initialized.load() ? flags : 0;
}

// ---------------------------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------------------------

struct PropertyBag {
  std::map<std::string, std::string> strings;
  std::map<std::string, int64_t> numbers;
  std::map<std::string, float> floats;
  std::map<std::string, void*> pointers;
  std::map<std::string, bool> booleans;
};

namespace {
std::mutex g_propertiesMutex;
std::map<SDL_PropertiesID, std::unique_ptr<PropertyBag>> g_properties;
SDL_PropertiesID g_nextPropertiesId = 1;

PropertyBag* bag(SDL_PropertiesID id) {
  std::lock_guard lock(g_propertiesMutex);
  const auto it = g_properties.find(id);
  return it == g_properties.end() ? nullptr : it->second.get();
}
} // namespace

SDL_PropertiesID SDL_CreateProperties(void) {
  std::lock_guard lock(g_propertiesMutex);
  const SDL_PropertiesID id = g_nextPropertiesId++;
  g_properties.emplace(id, std::make_unique<PropertyBag>());
  return id;
}

bool SDL_CopyProperties(SDL_PropertiesID src, SDL_PropertiesID dst) {
  auto* from = bag(src);
  auto* to = bag(dst);
  if (from == nullptr || to == nullptr) {
    return false;
  }
  std::lock_guard lock(g_propertiesMutex);
  to->strings = from->strings;
  to->numbers = from->numbers;
  to->floats = from->floats;
  to->pointers = from->pointers;
  to->booleans = from->booleans;
  return true;
}

void SDL_DestroyProperties(SDL_PropertiesID id) {
  std::lock_guard lock(g_propertiesMutex);
  g_properties.erase(id);
}

bool SDL_SetStringProperty(SDL_PropertiesID id, const char* name, const char* value) {
  auto* b = bag(id);
  if (b == nullptr) {
    return false;
  }
  std::lock_guard lock(g_propertiesMutex);
  b->strings[name] = value ? value : "";
  return true;
}

const char* SDL_GetStringProperty(SDL_PropertiesID id, const char* name, const char* defaultValue) {
  auto* b = bag(id);
  if (b == nullptr) {
    return defaultValue;
  }
  std::lock_guard lock(g_propertiesMutex);
  const auto it = b->strings.find(name);
  return it == b->strings.end() ? defaultValue : it->second.c_str();
}

bool SDL_SetNumberProperty(SDL_PropertiesID id, const char* name, int64_t value) {
  auto* b = bag(id);
  if (b == nullptr) {
    return false;
  }
  std::lock_guard lock(g_propertiesMutex);
  b->numbers[name] = value;
  return true;
}

int64_t SDL_GetNumberProperty(SDL_PropertiesID id, const char* name, int64_t defaultValue) {
  auto* b = bag(id);
  if (b == nullptr) {
    return defaultValue;
  }
  std::lock_guard lock(g_propertiesMutex);
  const auto it = b->numbers.find(name);
  return it == b->numbers.end() ? defaultValue : it->second;
}

bool SDL_SetFloatProperty(SDL_PropertiesID id, const char* name, float value) {
  auto* b = bag(id);
  if (b == nullptr) {
    return false;
  }
  std::lock_guard lock(g_propertiesMutex);
  b->floats[name] = value;
  return true;
}

float SDL_GetFloatProperty(SDL_PropertiesID id, const char* name, float defaultValue) {
  auto* b = bag(id);
  if (b == nullptr) {
    return defaultValue;
  }
  std::lock_guard lock(g_propertiesMutex);
  const auto it = b->floats.find(name);
  return it == b->floats.end() ? defaultValue : it->second;
}

bool SDL_SetBooleanProperty(SDL_PropertiesID id, const char* name, bool value) {
  auto* b = bag(id);
  if (b == nullptr) {
    return false;
  }
  std::lock_guard lock(g_propertiesMutex);
  b->booleans[name] = value;
  return true;
}

bool SDL_GetBooleanProperty(SDL_PropertiesID id, const char* name, bool defaultValue) {
  auto* b = bag(id);
  if (b == nullptr) {
    return defaultValue;
  }
  std::lock_guard lock(g_propertiesMutex);
  const auto it = b->booleans.find(name);
  return it == b->booleans.end() ? defaultValue : it->second;
}

bool SDL_SetPointerProperty(SDL_PropertiesID id, const char* name, void* value) {
  auto* b = bag(id);
  if (b == nullptr) {
    return false;
  }
  std::lock_guard lock(g_propertiesMutex);
  b->pointers[name] = value;
  return true;
}

void* SDL_GetPointerProperty(SDL_PropertiesID id, const char* name, void* defaultValue) {
  auto* b = bag(id);
  if (b == nullptr) {
    return defaultValue;
  }
  std::lock_guard lock(g_propertiesMutex);
  const auto it = b->pointers.find(name);
  return it == b->pointers.end() ? defaultValue : it->second;
}

bool SDL_HasProperty(SDL_PropertiesID id, const char* name) {
  auto* b = bag(id);
  if (b == nullptr) {
    return false;
  }
  std::lock_guard lock(g_propertiesMutex);
  return b->strings.contains(name) || b->numbers.contains(name) || b->floats.contains(name) ||
         b->pointers.contains(name) || b->booleans.contains(name);
}

SDL_PropertyType SDL_GetPropertyType(SDL_PropertiesID id, const char* name) {
  auto* b = bag(id);
  if (b == nullptr) {
    return SDL_PROPERTY_TYPE_INVALID;
  }
  std::lock_guard lock(g_propertiesMutex);
  if (b->strings.contains(name)) {
    return SDL_PROPERTY_TYPE_STRING;
  }
  if (b->numbers.contains(name)) {
    return SDL_PROPERTY_TYPE_NUMBER;
  }
  if (b->floats.contains(name)) {
    return SDL_PROPERTY_TYPE_FLOAT;
  }
  if (b->pointers.contains(name)) {
    return SDL_PROPERTY_TYPE_POINTER;
  }
  if (b->booleans.contains(name)) {
    return SDL_PROPERTY_TYPE_BOOLEAN;
  }
  return SDL_PROPERTY_TYPE_INVALID;
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------------------------

struct SDL_Window {
  std::string title;
  int width = static_cast<int>(kWindowWidth);
  int height = static_cast<int>(kWindowHeight);
  std::atomic<SDL_WindowFlags> flags{0};
  SDL_PropertiesID properties = 0;
  SDL_DisplayID display = kDisplayId;
  NWindow* native = nullptr;
};

extern "C" {

SDL_Window* SDL_CreateWindowWithProperties(SDL_PropertiesID props) {
  auto* window = new SDL_Window();
  window->properties = SDL_CreateProperties();
  if (const char* title = SDL_GetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, nullptr)) {
    window->title = title;
  }
  window->width = static_cast<int>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, kWindowWidth));
  window->height = static_cast<int>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, kWindowHeight));
  window->flags = static_cast<SDL_WindowFlags>(
      SDL_GetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, SDL_WINDOW_FULLSCREEN));
  // SDL and Dawn's EGL surface use the same application window.
  window->native = switchGetMainWindow();
  if (window->native == nullptr) {
    set_error("No valid Switch NWindow");
    SDL_DestroyWindow(window);
    return nullptr;
  }
  const Result result = nwindowSetDimensions(window->native, kWindowWidth, kWindowHeight);
  if (R_FAILED(result)) {
    SDL_Log("Switch NWindow dimensions: result=0x%08x", static_cast<unsigned>(result));
  }
  SDL_Log("SDL window: %dx%d, native NWindow %p valid=%d", window->width, window->height,
          window->native, static_cast<int>(nwindowIsValid(window->native)));
  return window;
}

SDL_Window* SDL_CreateWindow(const char* title, int w, int h, SDL_WindowFlags flags) {
  const SDL_PropertiesID props = SDL_CreateProperties();
  SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, title);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, w);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, h);
  SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_FLAGS_NUMBER, static_cast<int64_t>(flags));
  SDL_Window* window = SDL_CreateWindowWithProperties(props);
  SDL_DestroyProperties(props);
  return window;
}

void SDL_DestroyWindow(SDL_Window* window) {
  if (window == nullptr) {
    return;
  }
  SDL_DestroyProperties(window->properties);
  delete window;
}

SDL_PropertiesID SDL_GetWindowProperties(SDL_Window* window) { return window ? window->properties : 0; }

SDL_WindowFlags SDL_GetWindowFlags(SDL_Window* window) {
  return window ? window->flags.load(std::memory_order_relaxed) : 0;
}

bool SDL_GetWindowSize(SDL_Window* window, int* w, int* h) {
  if (w) {
    *w = window ? window->width : static_cast<int>(kWindowWidth);
  }
  if (h) {
    *h = window ? window->height : static_cast<int>(kWindowHeight);
  }
  return true;
}

bool SDL_GetWindowSizeInPixels(SDL_Window* window, int* w, int* h) {
  return SDL_GetWindowSize(window, w, h);
}

float SDL_GetWindowDisplayScale(SDL_Window* window) {
  (void)window;
  return 1.0f;
}

SDL_DisplayID SDL_GetDisplayForWindow(SDL_Window* window) { return window ? window->display : kDisplayId; }

bool SDL_SetWindowTitle(SDL_Window* window, const char* title) {
  if (window && title) {
    window->title = title;
  }
  return true;
}

const char* SDL_GetWindowTitle(SDL_Window* window) { return window ? window->title.c_str() : ""; }

bool SDL_SetWindowMinimumSize(SDL_Window* window, int w, int h) {
  (void)window;
  (void)w;
  (void)h;
  return true;
}

bool SDL_SetWindowSize(SDL_Window* window, int w, int h) {
  (void)window;
  (void)w;
  (void)h;
  return true;
}

bool SDL_SetWindowPosition(SDL_Window* window, int x, int y) {
  (void)window;
  (void)x;
  (void)y;
  return true;
}

bool SDL_SetWindowIcon(SDL_Window* window, SDL_Surface* icon) {
  (void)window;
  (void)icon;
  return true;
}

bool SDL_ShowWindow(SDL_Window* window) {
  if (window == nullptr) {
    return false;
  }
  window->flags.fetch_and(~SDL_WINDOW_HIDDEN, std::memory_order_relaxed);
  return true;
}

bool SDL_HideWindow(SDL_Window* window) {
  if (window == nullptr) {
    return false;
  }
  window->flags.fetch_or(SDL_WINDOW_HIDDEN, std::memory_order_relaxed);
  return true;
}
bool SDL_RestoreWindow(SDL_Window* window) { return window != nullptr; }
bool SDL_SyncWindow(SDL_Window* window) { return window != nullptr; }

bool SDL_SetWindowFullscreen(SDL_Window* window, bool fullscreen) {
  (void)window;
  (void)fullscreen;
  return true;
}

bool SDL_SetWindowFullscreenMode(SDL_Window* window, const SDL_DisplayMode* mode) {
  (void)window;
  (void)mode;
  return true;
}

const SDL_DisplayMode* SDL_GetWindowFullscreenMode(SDL_Window* window) {
  (void)window;
  return nullptr;
}

bool SDL_GetClosestFullscreenDisplayMode(SDL_DisplayID display, int w, int h, float refresh,
                                         bool includeHighDensityModes, SDL_DisplayMode* closest) {
  (void)display;
  (void)w;
  (void)h;
  (void)refresh;
  (void)includeHighDensityModes;
  if (closest) {
    std::memset(closest, 0, sizeof(*closest));
    closest->w = static_cast<int>(kWindowWidth);
    closest->h = static_cast<int>(kWindowHeight);
    closest->refresh_rate = 60.0f;
  }
  return closest != nullptr;
}

const SDL_DisplayMode* SDL_GetDesktopDisplayMode(SDL_DisplayID display) {
  (void)display;
  static SDL_DisplayMode mode{};
  mode.w = static_cast<int>(kWindowWidth);
  mode.h = static_cast<int>(kWindowHeight);
  mode.refresh_rate = 60.0f;
  return &mode;
}

const SDL_DisplayMode* SDL_GetCurrentDisplayMode(SDL_DisplayID display) {
  return SDL_GetDesktopDisplayMode(display);
}

// ---------------------------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------------------------

uint32_t SDL_RegisterEvents(int numevents) {
  static uint32_t next = SDL_EVENT_USER;
  const uint32_t base = next;
  next += static_cast<uint32_t>(numevents);
  return base;
}

void SDL_PumpEvents(void) {
  if (!g_initialized.load()) {
    return;
  }
  std::call_once(g_padInit, [] {
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pad);
    hidInitializeTouchScreen();
  });

  // Both the SDL events and the polled state come from the same HID sample.
  std::lock_guard lock(g_padMutex);
  padUpdate(&g_pad);
  const bool connected = padIsConnected(&g_pad);
  const uint64_t timestamp = SDL_GetTicksNS();
  if (connected != g_padConnected) {
    g_padConnected = connected;
    SDL_Event event{};
    event.type = connected ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED;
    event.gdevice.timestamp = timestamp;
    event.gdevice.which = kGamepadId;
    push_event(event);
  }
  if (!connected) {
    g_buttons = 0;
    g_axes.fill(0);
    return;
  }

  const uint64_t held = padGetButtons(&g_pad);
  const uint32_t buttons = map_buttons(held);
  const uint32_t pressed = buttons & ~g_buttons;
  const uint32_t released = ~buttons & g_buttons;
  g_buttons = buttons;
  for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i) {
    if ((pressed & (1u << i)) != 0) {
      SDL_Event event{};
      event.type = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
      event.common.timestamp = timestamp;
      event.gbutton.which = kGamepadId;
      event.gbutton.button = static_cast<SDL_GamepadButton>(i);
      event.gbutton.down = true;
      push_event(event);
    }
    if ((released & (1u << i)) != 0) {
      SDL_Event event{};
      event.type = SDL_EVENT_GAMEPAD_BUTTON_UP;
      event.common.timestamp = timestamp;
      event.gbutton.which = kGamepadId;
      event.gbutton.button = static_cast<SDL_GamepadButton>(i);
      event.gbutton.down = false;
      push_event(event);
    }
  }

  // SDL's vertical axes increase down, while libnx's increase up.
  std::array<int16_t, SDL_GAMEPAD_AXIS_COUNT> axes{};
  constexpr int32_t deadzone = 2000;
  const auto left = padGetStickPos(&g_pad, 0);
  const auto right = padGetStickPos(&g_pad, 1);
  axes[SDL_GAMEPAD_AXIS_LEFTX] = axis_from_stick(left.x, deadzone);
  axes[SDL_GAMEPAD_AXIS_LEFTY] = axis_from_stick(-left.y, deadzone);
  axes[SDL_GAMEPAD_AXIS_RIGHTX] = axis_from_stick(right.x, deadzone);
  axes[SDL_GAMEPAD_AXIS_RIGHTY] = axis_from_stick(-right.y, deadzone);
  axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = (held & HidNpadButton_ZL) ? 32767 : 0;
  axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = (held & HidNpadButton_ZR) ? 32767 : 0;
  for (int i = 0; i < SDL_GAMEPAD_AXIS_COUNT; ++i) {
    if (axes[i] != g_axes[i]) {
      SDL_Event event{};
      event.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
      event.common.timestamp = timestamp;
      event.gaxis.which = kGamepadId;
      event.gaxis.axis = static_cast<uint8_t>(i);
      event.gaxis.value = axes[i];
      g_axes[i] = axes[i];
      push_event(event);
    }
  }

  // The console's HOME/Plus combinations still belong to the system; nothing else to forward.
}

bool SDL_PollEvent(SDL_Event* event) {
  if (event == nullptr) {
    std::lock_guard lock(g_eventMutex);
    g_events.clear();
    return false;
  }
  SDL_PumpEvents();
  std::lock_guard lock(g_eventMutex);
  if (g_events.empty()) {
    return false;
  }
  *event = g_events.front();
  g_events.pop_front();
  return true;
}

bool SDL_WaitEvent(SDL_Event* event) {
  for (;;) {
    if (SDL_PollEvent(event)) {
      return true;
    }
    svcSleepThread(1000000ll);
  }
}

bool SDL_WaitEventTimeout(SDL_Event* event, int32_t timeoutMs) {
  const uint64_t deadline = SDL_GetTicks() + static_cast<uint64_t>(timeoutMs < 0 ? 0 : timeoutMs);
  for (;;) {
    if (SDL_PollEvent(event)) {
      return true;
    }
    if (timeoutMs >= 0 && SDL_GetTicks() >= deadline) {
      return false;
    }
    svcSleepThread(1000000ll);
  }
}

bool SDL_PushEvent(SDL_Event* event) {
  if (event == nullptr) {
    return false;
  }
  for (auto* filter : g_eventFilters) {
    if (!filter(nullptr, event)) {
      return false;
    }
  }
  push_event(*event);
  return true;
}

bool SDL_AddEventWatch(SDL_EventFilter filter, void* userdata) {
  if (filter == nullptr) {
    return false;
  }
  g_eventFilters.push_back(filter);
  return true;
}

void SDL_RemoveEventWatch(SDL_EventFilter filter, void* userdata) {
  (void)userdata;
  for (auto it = g_eventFilters.begin(); it != g_eventFilters.end(); ++it) {
    if (*it == filter) {
      g_eventFilters.erase(it);
      return;
    }
  }
}

void SDL_FlushEvents(uint32_t minType, uint32_t maxType) {
  std::lock_guard lock(g_eventMutex);
  for (auto it = g_events.begin(); it != g_events.end();) {
    if (it->type >= minType && it->type <= maxType) {
      it = g_events.erase(it);
    } else {
      ++it;
    }
  }
}

bool SDL_ConvertEventToRenderCoordinates(SDL_Renderer* renderer, SDL_Event* event) {
  (void)renderer;
  return event != nullptr;
}

bool SDL_GetWindowKeyboardGrab(SDL_Window* window) {
  (void)window;
  return false;
}

bool SDL_GetWindowMouseGrab(SDL_Window* window) {
  (void)window;
  return false;
}

SDL_Window* SDL_GetKeyboardFocus(void) { return nullptr; }
SDL_Window* SDL_GetMouseFocus(void) { return nullptr; }

SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y) {
  if (x) {
    *x = 0.f;
  }
  if (y) {
    *y = 0.f;
  }
  return true;
}

SDL_MouseButtonFlags SDL_GetGlobalMouseState(float* x, float* y) { return SDL_GetMouseState(x, y); }

SDL_MouseButtonFlags SDL_GetRelativeMouseState(float* x, float* y) {
  if (x) {
    *x = 0.f;
  }
  if (y) {
    *y = 0.f;
  }
  return 0;
}

const bool* SDL_GetKeyboardState(int* numkeys) {
  static bool state[SDL_SCANCODE_COUNT] = {};
  if (numkeys) {
    *numkeys = SDL_SCANCODE_COUNT;
  }
  return state;
}

const char* SDL_GetScancodeName(SDL_Scancode scancode) {
  // The console has no keyboard; the runtime only asks for names when building its keybind UI.
  (void)scancode;
  return "";
}

SDL_Scancode SDL_GetScancodeFromName(const char* name) {
  (void)name;
  return SDL_SCANCODE_UNKNOWN;
}

bool SDL_StartTextInput(SDL_Window* window) { (void)window; return true; }
bool SDL_StopTextInput(SDL_Window* window) { (void)window; return true; }
bool SDL_TextInputActive(SDL_Window* window) { (void)window; return false; }

bool SDL_ShowCursor(void) { return true; }
bool SDL_HideCursor(void) { return true; }

} // extern "C"

// ---------------------------------------------------------------------------------------------
// Gamepad / joystick
// ---------------------------------------------------------------------------------------------

struct SDL_Gamepad {
  SDL_JoystickID id = kGamepadId;
  std::atomic<int> playerIndex{0};
  const char* name = "Switch Controller";
};

namespace {
// One logical PAD port lives for the process. SDL's borrowed lookups must return
// the same handle as OpenGamepad, including across a controller reconnect.
SDL_Gamepad g_gamepad;
}

extern "C" {

SDL_JoystickID* SDL_GetGamepads(int* count) {
  std::lock_guard lock(g_padMutex);
  const int connectedCount = g_padConnected ? 1 : 0;
  auto* ids = static_cast<SDL_JoystickID*>(SDL_calloc(connectedCount + 1, sizeof(SDL_JoystickID)));
  if (ids && g_padConnected) {
    ids[0] = kGamepadId;
  }
  if (count) {
    *count = ids ? connectedCount : 0;
  }
  return ids;
}

bool SDL_IsGamepad(SDL_JoystickID instanceId) { return SDL_GetGamepadFromID(instanceId) != nullptr; }

SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID instanceId) { return SDL_GetGamepadFromID(instanceId); }

void SDL_CloseGamepad(SDL_Gamepad* gamepad) { (void)gamepad; }

SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID instanceId) {
  std::lock_guard lock(g_padMutex);
  return instanceId == kGamepadId && g_padConnected ? &g_gamepad : nullptr;
}

SDL_Gamepad* SDL_GetGamepadFromPlayerIndex(int playerIndex) {
  return playerIndex >= 0 && g_gamepad.playerIndex.load() == playerIndex
             ? SDL_GetGamepadFromID(kGamepadId)
             : nullptr;
}

SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad* gamepad) { return gamepad ? gamepad->id : 0; }

bool SDL_GamepadConnected(SDL_Gamepad* gamepad) {
  return gamepad && SDL_GetGamepadFromID(gamepad->id) != nullptr;
}

SDL_Joystick* SDL_GetGamepadJoystick(SDL_Gamepad* gamepad) {
  return reinterpret_cast<SDL_Joystick*>(gamepad);
}

SDL_JoystickID SDL_GetJoystickID(SDL_Joystick* joystick) {
  return reinterpret_cast<SDL_Gamepad*>(joystick) ? kGamepadId : 0;
}

const char* SDL_GetGamepadName(SDL_Gamepad* gamepad) { return gamepad ? gamepad->name : ""; }

SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad* gamepad) {
  return gamepad ? SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO : SDL_GAMEPAD_TYPE_UNKNOWN;
}

SDL_GamepadType SDL_GetRealGamepadType(SDL_Gamepad* gamepad) { return SDL_GetGamepadType(gamepad); }

int SDL_GetGamepadPlayerIndex(SDL_Gamepad* gamepad) { return gamepad ? gamepad->playerIndex.load() : -1; }

bool SDL_SetGamepadPlayerIndex(SDL_Gamepad* gamepad, int playerIndex) {
  if (gamepad) {
    gamepad->playerIndex = playerIndex;
  }
  return gamepad != nullptr;
}

int SDL_GetGamepadPlayerIndexForID(SDL_JoystickID instanceId) {
  return SDL_GetGamepadPlayerIndex(SDL_GetGamepadFromID(instanceId));
}

SDL_JoystickID SDL_GetGamepadIDForPlayerIndex(int playerIndex) {
  return SDL_GetGamepadID(SDL_GetGamepadFromPlayerIndex(playerIndex));
}

uint16_t SDL_GetGamepadVendor(SDL_Gamepad* gamepad) { return gamepad ? 0x057E : 0; }

uint16_t SDL_GetGamepadProduct(SDL_Gamepad* gamepad) { return gamepad ? 0x2009 : 0; }

uint16_t SDL_GetGamepadProductVersion(SDL_Gamepad* gamepad) { return gamepad ? 0x0001 : 0; }

uint16_t SDL_GetGamepadFirmwareVersion(SDL_Gamepad* gamepad) { return gamepad ? 0x0001 : 0; }

const char* SDL_GetGamepadSerial(SDL_Gamepad* gamepad) { return gamepad ? "" : nullptr; }

SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad* gamepad) { return gamepad ? 1 : 0; }

char* SDL_GetGamepadMapping(SDL_Gamepad* gamepad) { (void)gamepad; return nullptr; }

char* SDL_GetGamepadMappingForID(SDL_JoystickID instanceId) { (void)instanceId; return nullptr; }

char* SDL_GetGamepadMappingForGUID(SDL_GUID guid) { (void)guid; return nullptr; }

int SDL_AddGamepadMapping(const char* mapping) { (void)mapping; return -1; }

int SDL_AddGamepadMappingsFromFile(const char* file) { (void)file; return 0; }

SDL_JoystickID SDL_AttachVirtualJoystick(const SDL_VirtualJoystickDesc* desc) {
  (void)desc;
  return 0;
}

bool SDL_DetachVirtualJoystick(SDL_JoystickID instanceId) { (void)instanceId; return false; }

SDL_GUID SDL_GetGamepadGUIDForID(SDL_JoystickID instanceId) {
  SDL_GUID guid{};
  (void)instanceId;
  guid.data[0] = 0x7E;
  guid.data[1] = 0x05;
  return guid;
}

SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID instanceId) { return SDL_GetGamepadGUIDForID(instanceId); }

void SDL_GUIDToString(SDL_GUID guid, char* pszGUID, int cbGUID) {
  if (pszGUID == nullptr || cbGUID <= 0) {
    return;
  }
  std::snprintf(pszGUID, static_cast<size_t>(cbGUID), "%02x%02x%02x%02x", guid.data[0], guid.data[1], guid.data[2],
                guid.data[3]);
}

SDL_GUID SDL_StringToGUID(const char* pchGUID) {
  SDL_GUID guid{};
  (void)pchGUID;
  guid.data[0] = 0x7E;
  guid.data[1] = 0x05;
  return guid;
}

void SDL_GetJoystickGUIDInfo(SDL_GUID guid, uint16_t* vendor, uint16_t* product, uint16_t* version,
                             uint16_t* crc16) {
  if (vendor) {
    *vendor = 0x057E;
  }
  if (product) {
    *product = 0x2009;
  }
  if (version) {
    *version = 1;
  }
  if (crc16) {
    *crc16 = 0;
  }
  (void)guid;
}

SDL_JoystickID* SDL_GetJoysticks(int* count) { return SDL_GetGamepads(count); }

SDL_Joystick* SDL_OpenJoystick(SDL_JoystickID instanceId) {
  return instanceId == kGamepadId ? reinterpret_cast<SDL_Joystick*>(SDL_OpenGamepad(instanceId)) : nullptr;
}

void SDL_CloseJoystick(SDL_Joystick* joystick) { SDL_CloseGamepad(reinterpret_cast<SDL_Gamepad*>(joystick)); }

const char* SDL_GetJoystickNameForID(SDL_JoystickID instanceId) {
  return instanceId == kGamepadId ? "Switch Controller" : nullptr;
}

SDL_JoystickType SDL_GetJoystickTypeForID(SDL_JoystickID instanceId) {
  return instanceId == kGamepadId ? SDL_JOYSTICK_TYPE_GAMEPAD : SDL_JOYSTICK_TYPE_UNKNOWN;
}

SDL_PowerState SDL_GetJoystickPowerInfo(SDL_Joystick* joystick, int* percent) {
  (void)joystick;
  if (percent) {
    *percent = -1;
  }
  return SDL_POWERSTATE_UNKNOWN;
}

SDL_PowerState SDL_GetGamepadPowerInfo(SDL_Gamepad* gamepad, int* percent) {
  (void)gamepad;
  if (percent) {
    *percent = -1;
  }
  return SDL_POWERSTATE_UNKNOWN;
}

int SDL_GetNumJoystickAxes(SDL_Joystick* joystick) {
  return joystick ? SDL_GAMEPAD_AXIS_COUNT : 0;
}

int SDL_GetNumJoystickButtons(SDL_Joystick* joystick) {
  return joystick ? SDL_GAMEPAD_BUTTON_COUNT : 0;
}

int SDL_GetNumJoystickHats(SDL_Joystick* joystick) { (void)joystick; return 0; }

int16_t SDL_GetJoystickAxis(SDL_Joystick* joystick, int axis) {
  return SDL_GetGamepadAxis(reinterpret_cast<SDL_Gamepad*>(joystick), static_cast<SDL_GamepadAxis>(axis));
}

bool SDL_GetJoystickAxisInitialState(SDL_Joystick* joystick, int axis, int16_t* state) {
  (void)joystick;
  (void)axis;
  if (state) {
    *state = 0;
  }
  return false;
}

bool SDL_GetJoystickButton(SDL_Joystick* joystick, int button) {
  return SDL_GetGamepadButton(reinterpret_cast<SDL_Gamepad*>(joystick), static_cast<SDL_GamepadButton>(button));
}

uint8_t SDL_GetJoystickHat(SDL_Joystick* joystick, int hat) {
  (void)joystick;
  (void)hat;
  return SDL_HAT_CENTERED;
}

SDL_JoystickID SDL_GetJoystickInstanceID(SDL_Joystick* joystick) {
  return joystick ? kGamepadId : 0;
}

bool SDL_RumbleJoystick(SDL_Joystick* joystick, uint16_t low, uint16_t high, uint32_t ms) {
  (void)joystick;
  (void)low;
  (void)high;
  (void)ms;
  return false;
}

bool SDL_RumbleGamepad(SDL_Gamepad* gamepad, uint16_t low, uint16_t high, uint32_t ms) {
  return SDL_RumbleJoystick(SDL_GetGamepadJoystick(gamepad), low, high, ms);
}

bool SDL_RumbleGamepadTriggers(SDL_Gamepad* gamepad, uint16_t left, uint16_t right, uint32_t ms) {
  (void)gamepad;
  (void)left;
  (void)right;
  (void)ms;
  return false;
}

bool SDL_SetGamepadLED(SDL_Gamepad* gamepad, uint8_t red, uint8_t green, uint8_t blue) {
  (void)gamepad;
  (void)red;
  (void)green;
  (void)blue;
  return false;
}

bool SDL_SendGamepadEffect(SDL_Gamepad* gamepad, const void* data, int size) {
  (void)gamepad;
  (void)data;
  (void)size;
  return false;
}

bool SDL_GamepadHasAxis(SDL_Gamepad* gamepad, SDL_GamepadAxis axis) {
  return gamepad != nullptr && axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT;
}

bool SDL_GamepadHasButton(SDL_Gamepad* gamepad, SDL_GamepadButton button) {
  return gamepad != nullptr && button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT;
}

int16_t SDL_GetGamepadAxis(SDL_Gamepad* gamepad, SDL_GamepadAxis axis) {
  if (!SDL_GamepadHasAxis(gamepad, axis)) {
    return 0;
  }
  std::lock_guard lock(g_padMutex);
  return g_axes[axis];
}

bool SDL_GetGamepadButton(SDL_Gamepad* gamepad, SDL_GamepadButton button) {
  if (!SDL_GamepadHasButton(gamepad, button)) {
    return false;
  }
  std::lock_guard lock(g_padMutex);
  return (g_buttons & (1u << button)) != 0;
}

bool SDL_GamepadHasSensor(SDL_Gamepad* gamepad, SDL_SensorType type) {
  (void)gamepad;
  (void)type;
  return false;
}

bool SDL_SetGamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type, bool enabled) {
  (void)gamepad;
  (void)type;
  (void)enabled;
  return false;
}

bool SDL_GamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type) {
  (void)gamepad;
  (void)type;
  return false;
}

float SDL_GetGamepadSensorDataRate(SDL_Gamepad* gamepad, SDL_SensorType type) { (void)gamepad; (void)type; return 0.f; }

bool SDL_GetGamepadSensorData(SDL_Gamepad* gamepad, SDL_SensorType type, float* data, int num_values) {
  (void)gamepad;
  (void)type;
  (void)data;
  (void)num_values;
  return false;
}

const char* SDL_GetGamepadStringForButton(SDL_GamepadButton button) {
  static const char* const names[SDL_GAMEPAD_BUTTON_COUNT] = {
      "South", "East", "West", "North", "Back", "Guide", "Start", "Left Stick", "Right Stick",
      "Left Shoulder", "Right Shoulder", "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right",
      "Misc 1", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad", "Misc 2", "Misc 3",
      "Misc 4", "Misc 5", "Misc 6"};
  if (button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) {
    return "";
  }
  return names[button];
}

const char* SDL_GetGamepadStringForAxis(SDL_GamepadAxis axis) {
  static const char* const names[SDL_GAMEPAD_AXIS_COUNT] = {
      "Left X", "Left Y", "Right X", "Right Y", "Left Trigger", "Right Trigger"};
  if (axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) {
    return "";
  }
  return names[axis];
}

bool SDL_SetJoystickVirtualAxis(SDL_Joystick* joystick, int axis, int16_t value) {
  (void)joystick;
  (void)axis;
  (void)value;
  return false;
}

bool SDL_SetJoystickVirtualButton(SDL_Joystick* joystick, int button, bool down) {
  (void)joystick;
  (void)button;
  (void)down;
  return false;
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// Surface (only used for the window icon)
// ---------------------------------------------------------------------------------------------

extern "C" {

SDL_Surface* SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void* pixels, int pitch) {
  auto* surface = static_cast<SDL_Surface*>(std::calloc(1, sizeof(SDL_Surface)));
  if (surface == nullptr) {
    return nullptr;
  }
  surface->w = width;
  surface->h = height;
  surface->format = format;
  surface->pitch = pitch;
  surface->pixels = pixels;
  surface->refcount = 1;
  return surface;
}

void SDL_DestroySurface(SDL_Surface* surface) { std::free(surface); }

SDL_PixelFormat SDL_GetPixelFormatForMasks(int bpp, uint32_t rmask, uint32_t gmask, uint32_t bmask, uint32_t amask) {
  (void)bpp;
  (void)rmask;
  (void)gmask;
  (void)bmask;
  (void)amask;
  return SDL_PIXELFORMAT_RGBA8888;
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// Filesystem
// ---------------------------------------------------------------------------------------------

namespace {
constexpr const char* kBasePath = NSMBW_SWITCH_DATA_ROOT "/";
constexpr const char* kPrefPath = NSMBW_SWITCH_DATA_ROOT "/data/";
} // namespace

extern "C" {

const char* SDL_GetBasePath(void) {
  return kBasePath;
}

char* SDL_GetPrefPath(const char* org, const char* app) {
  (void)org;
  (void)app;
  char* copy = static_cast<char*>(std::malloc(std::strlen(kPrefPath) + 1));
  if (copy) {
    std::strcpy(copy, kPrefPath);
  }
  return copy;
}

bool SDL_CreateDirectory(const char* path) {
  if (path == nullptr) {
    return false;
  }
  const Result rc = fsdevCreateFile(path, 0, 0);
  return R_SUCCEEDED(rc);
}

bool SDL_RemovePath(const char* path) {
  if (path == nullptr) {
    return false;
  }
  return R_SUCCEEDED(fsdevDeleteDirectoryRecursively(path)) || ::remove(path) == 0;
}

bool SDL_GetPathInfo(const char* path, SDL_PathInfo* info) {
  if (path == nullptr || info == nullptr) {
    return false;
  }
  std::memset(info, 0, sizeof(*info));
  struct stat st{};
  if (::stat(path, &st) != 0) {
    info->type = SDL_PATHTYPE_NONE;
    return false;
  }
  info->size = st.st_size;
  info->modify_time = static_cast<SDL_Time>(st.st_mtime) * 1000000000ll;
  info->access_time = info->modify_time;
  info->create_time = info->modify_time;
  info->type = S_ISDIR(st.st_mode) ? SDL_PATHTYPE_DIRECTORY : SDL_PATHTYPE_FILE;
  return true;
}

bool SDL_EnumerateDirectory(const char* path, SDL_EnumerateDirectoryCallback callback, void* userdata) {
  (void)path;
  (void)callback;
  (void)userdata;
  return false;
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// IO streams (used for asset loading; plain stdio underneath)
// ---------------------------------------------------------------------------------------------

struct SDL_IOStream {
  FILE* file = nullptr;
  bool writable = false;
  Sint64 size = 0;
};

extern "C" {

SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode) {
  if (file == nullptr) {
    return nullptr;
  }
  FILE* handle = std::fopen(file, mode ? mode : "rb");
  if (handle == nullptr) {
    return nullptr;
  }
  auto* stream = new SDL_IOStream();
  stream->file = handle;
  stream->writable = mode != nullptr && (std::strchr(mode, 'w') != nullptr || std::strchr(mode, 'a') != nullptr ||
                                         std::strchr(mode, '+') != nullptr);
  std::fseek(handle, 0, SEEK_END);
  stream->size = std::ftell(handle);
  std::fseek(handle, 0, SEEK_SET);
  return stream;
}

size_t SDL_ReadIO(SDL_IOStream* stream, void* ptr, size_t size) {
  if (stream == nullptr || stream->file == nullptr) {
    return 0;
  }
  return std::fread(ptr, 1, size, stream->file);
}

size_t SDL_WriteIO(SDL_IOStream* stream, const void* ptr, size_t size) {
  if (stream == nullptr || stream->file == nullptr) {
    return 0;
  }
  return std::fwrite(ptr, 1, size, stream->file);
}

Sint64 SDL_SeekIO(SDL_IOStream* stream, Sint64 offset, SDL_IOWhence whence) {
  if (stream == nullptr || stream->file == nullptr) {
    return -1;
  }
  const int origin = whence == SDL_IO_SEEK_SET ? SEEK_SET : (whence == SDL_IO_SEEK_CUR ? SEEK_CUR : SEEK_END);
  if (std::fseek(stream->file, static_cast<long>(offset), origin) != 0) {
    return -1;
  }
  return static_cast<Sint64>(std::ftell(stream->file));
}

Sint64 SDL_TellIO(SDL_IOStream* stream) {
  return stream && stream->file ? static_cast<Sint64>(std::ftell(stream->file)) : -1;
}

Sint64 SDL_GetIOSize(SDL_IOStream* stream) { return stream ? stream->size : -1; }

SDL_IOStatus SDL_GetIOStatus(SDL_IOStream* stream) {
  if (stream == nullptr || stream->file == nullptr) {
    return SDL_IO_STATUS_NOT_READY;
  }
  return std::ferror(stream->file) != 0 ? SDL_IO_STATUS_ERROR : SDL_IO_STATUS_READY;
}

bool SDL_FlushIO(SDL_IOStream* stream) {
  return stream && stream->file ? std::fflush(stream->file) == 0 : false;
}

bool SDL_CloseIO(SDL_IOStream* stream) {
  if (stream == nullptr) {
    return false;
  }
  if (stream->file != nullptr) {
    std::fclose(stream->file);
  }
  delete stream;
  return true;
}

bool SDL_ReadU8(SDL_IOStream* s, uint8_t* v) { return SDL_ReadIO(s, v, sizeof(*v)) == sizeof(*v); }
bool SDL_ReadS8(SDL_IOStream* s, int8_t* v) { return SDL_ReadIO(s, v, sizeof(*v)) == sizeof(*v); }
bool SDL_ReadU16LE(SDL_IOStream* s, uint16_t* v) { return SDL_ReadIO(s, v, sizeof(*v)) == sizeof(*v); }
bool SDL_ReadS16LE(SDL_IOStream* s, int16_t* v) { return SDL_ReadIO(s, v, sizeof(*v)) == sizeof(*v); }
bool SDL_ReadU32LE(SDL_IOStream* s, uint32_t* v) { return SDL_ReadIO(s, v, sizeof(*v)) == sizeof(*v); }
bool SDL_ReadS32LE(SDL_IOStream* s, int32_t* v) { return SDL_ReadIO(s, v, sizeof(*v)) == sizeof(*v); }
bool SDL_ReadU64LE(SDL_IOStream* s, uint64_t* v) { return SDL_ReadIO(s, v, sizeof(*v)) == sizeof(*v); }
bool SDL_ReadS64LE(SDL_IOStream* s, int64_t* v) { return SDL_ReadIO(s, v, sizeof(*v)) == sizeof(*v); }
bool SDL_WriteU8(SDL_IOStream* s, uint8_t v) { return SDL_WriteIO(s, &v, sizeof(v)) == sizeof(v); }
bool SDL_WriteS8(SDL_IOStream* s, int8_t v) { return SDL_WriteIO(s, &v, sizeof(v)) == sizeof(v); }
bool SDL_WriteU16LE(SDL_IOStream* s, uint16_t v) { return SDL_WriteIO(s, &v, sizeof(v)) == sizeof(v); }
bool SDL_WriteS16LE(SDL_IOStream* s, int16_t v) { return SDL_WriteIO(s, &v, sizeof(v)) == sizeof(v); }
bool SDL_WriteU32LE(SDL_IOStream* s, uint32_t v) { return SDL_WriteIO(s, &v, sizeof(v)) == sizeof(v); }
bool SDL_WriteS32LE(SDL_IOStream* s, int32_t v) { return SDL_WriteIO(s, &v, sizeof(v)) == sizeof(v); }
bool SDL_WriteU64LE(SDL_IOStream* s, uint64_t v) { return SDL_WriteIO(s, &v, sizeof(v)) == sizeof(v); }
bool SDL_WriteS64LE(SDL_IOStream* s, int64_t v) { return SDL_WriteIO(s, &v, sizeof(v)) == sizeof(v); }

} // extern "C"

// ---------------------------------------------------------------------------------------------
// Audio: a silent sink. The audren mixer is the next step; until then the runtime's audio thread
// must not block, so queued data is consumed at the rate it is produced.
// ---------------------------------------------------------------------------------------------

struct SDL_AudioStream {
  SDL_AudioSpec spec{};
  std::atomic<uint64_t> queued{0};
  std::atomic<bool> paused{false};
  std::atomic<float> gain{1.f};
};

extern "C" {

SDL_AudioDeviceID SDL_OpenAudioDevice(SDL_AudioDeviceID device, const SDL_AudioSpec* spec) {
  (void)device;
  (void)spec;
  return 1;
}

SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec* spec,
                                           SDL_AudioStreamCallback callback, void* userdata) {
  (void)device;
  (void)callback;
  (void)userdata;
  auto* stream = new SDL_AudioStream();
  if (spec) {
    stream->spec = *spec;
  }
  return stream;
}

void SDL_DestroyAudioStream(SDL_AudioStream* stream) { delete stream; }

bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* data, int len) {
  (void)data;
  if (stream && len > 0) {
    stream->queued.fetch_add(static_cast<uint64_t>(len));
  }
  return stream != nullptr;
}

int SDL_GetAudioStreamQueued(SDL_AudioStream* stream) {
  return stream ? static_cast<int>(stream->queued.load()) : 0;
}

int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream) { return SDL_GetAudioStreamQueued(stream); }

bool SDL_FlushAudioStream(SDL_AudioStream* stream) {
  if (stream) {
    stream->queued.store(0);
  }
  return stream != nullptr;
}

bool SDL_ClearAudioStream(SDL_AudioStream* stream) { return SDL_FlushAudioStream(stream); }

bool SDL_SetAudioStreamGain(SDL_AudioStream* stream, float gain) {
  if (stream) {
    stream->gain.store(gain);
  }
  return stream != nullptr;
}

float SDL_GetAudioStreamGain(SDL_AudioStream* stream) { return stream ? stream->gain.load() : 0.f; }

bool SDL_PauseAudioStreamDevice(SDL_AudioStream* stream) {
  if (stream) {
    stream->paused.store(true);
  }
  return stream != nullptr;
}

bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream) {
  if (stream) {
    stream->paused.store(false);
  }
  return stream != nullptr;
}

bool SDL_AudioStreamDevicePaused(SDL_AudioStream* stream) { return stream ? stream->paused.load() : true; }

} // extern "C"

// ---------------------------------------------------------------------------------------------
// Renderer: Aurora uses an SDL renderer only for the ImGui overlay path; the game presents through
// WebGPU. These exist so the overlay code path links and reports itself unavailable.
// ---------------------------------------------------------------------------------------------

struct SDL_Renderer {
  SDL_Window* window = nullptr;
};

extern "C" {

SDL_Renderer* SDL_CreateRendererWithProperties(SDL_PropertiesID props) {
  auto* renderer = new SDL_Renderer();
  renderer->window = static_cast<SDL_Window*>(SDL_GetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, nullptr));
  return renderer;
}

SDL_Renderer* SDL_CreateRenderer(SDL_Window* window, const char* name) {
  (void)name;
  auto* renderer = new SDL_Renderer();
  renderer->window = window;
  return renderer;
}

void SDL_DestroyRenderer(SDL_Renderer* renderer) { delete renderer; }

bool SDL_RenderClear(SDL_Renderer* renderer) { return renderer != nullptr; }

bool SDL_RenderPresent(SDL_Renderer* renderer) { return renderer != nullptr; }

bool SDL_GetRenderOutputSize(SDL_Renderer* renderer, int* w, int* h) {
  return SDL_GetWindowSizeInPixels(renderer ? renderer->window : nullptr, w, h);
}

bool SDL_GetCurrentRenderOutputSize(SDL_Renderer* renderer, int* w, int* h) {
  return SDL_GetRenderOutputSize(renderer, w, h);
}

bool SDL_SetRenderScale(SDL_Renderer* renderer, float scaleX, float scaleY) {
  (void)renderer;
  (void)scaleX;
  (void)scaleY;
  return true;
}

bool SDL_GetRenderScale(SDL_Renderer* renderer, float* scaleX, float* scaleY) {
  (void)renderer;
  if (scaleX) {
    *scaleX = 1.f;
  }
  if (scaleY) {
    *scaleY = 1.f;
  }
  return true;
}

bool SDL_SetRenderLogicalPresentation(SDL_Renderer* renderer, int w, int h, SDL_RendererLogicalPresentation mode) {
  (void)renderer;
  (void)w;
  (void)h;
  (void)mode;
  return true;
}

bool SDL_GetRenderLogicalPresentation(SDL_Renderer* renderer, int* w, int* h, SDL_RendererLogicalPresentation* mode) {
  (void)renderer;
  if (w) {
    *w = static_cast<int>(kWindowWidth);
  }
  if (h) {
    *h = static_cast<int>(kWindowHeight);
  }
  if (mode) {
    *mode = SDL_LOGICAL_PRESENTATION_DISABLED;
  }
  return true;
}

SDL_Texture* SDL_CreateTexture(SDL_Renderer* renderer, SDL_PixelFormat format, SDL_TextureAccess access, int w, int h) {
  (void)renderer;
  (void)access;
  auto* texture = static_cast<SDL_Texture*>(std::calloc(1, sizeof(SDL_Texture)));
  if (texture == nullptr) {
    return nullptr;
  }
  texture->format = format;
  texture->w = w;
  texture->h = h;
  texture->refcount = 1;
  return texture;
}

SDL_Texture* SDL_CreateTextureFromSurface(SDL_Renderer* renderer, SDL_Surface* surface) {
  (void)surface;
  return SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STATIC, 1, 1);
}

void SDL_DestroyTexture(SDL_Texture* texture) { std::free(texture); }

bool SDL_UpdateTexture(SDL_Texture* texture, const SDL_Rect* rect, const void* pixels, int pitch) {
  (void)texture;
  (void)rect;
  (void)pixels;
  (void)pitch;
  return true;
}

bool SDL_SetTextureScaleMode(SDL_Texture* texture, SDL_ScaleMode scaleMode) {
  (void)texture;
  (void)scaleMode;
  return true;
}

bool SDL_RenderTexture(SDL_Renderer* renderer, SDL_Texture* texture, const SDL_FRect* src, const SDL_FRect* dst) {
  (void)renderer;
  (void)texture;
  (void)src;
  (void)dst;
  return true;
}

} // extern "C"

// ---------------------------------------------------------------------------------------------
// The remaining SDL surface used by ImGui's backends. None of it is meaningful on a console: there
// is no system cursor, clipboard, or window manager, and Dawn owns the GL context.
// ---------------------------------------------------------------------------------------------

extern "C" {

bool SDL_GL_SetAttribute(SDL_GLAttr attr, int value) {
    (void)attr;
    (void)value;
    return true;
}

bool SDL_GL_GetAttribute(SDL_GLAttr attr, int* value) {
    (void)attr;
    if (value) {
        *value = 0;
    }
    return true;
}

SDL_GLContext SDL_GL_CreateContext(SDL_Window* window) {
    (void)window;
    return reinterpret_cast<SDL_GLContext>(1);
}

bool SDL_GL_MakeCurrent(SDL_Window* window, SDL_GLContext context) {
    (void)window;
    (void)context;
    return true;
}

SDL_GLContext SDL_GL_GetCurrentContext(void) { return reinterpret_cast<SDL_GLContext>(1); }

SDL_Window* SDL_GL_GetCurrentWindow(void) { return nullptr; }

bool SDL_GL_SetSwapInterval(int interval) {
    (void)interval;
    return true;
}

bool SDL_GL_GetSwapInterval(int* interval) {
    if (interval) {
        *interval = 0;
    }
    return true;
}

bool SDL_GL_SwapWindow(SDL_Window* window) { return window != nullptr; }

bool SDL_GL_DestroyContext(SDL_GLContext context) {
    (void)context;
    return true;
}

bool SDL_Vulkan_CreateSurface(SDL_Window* window, void* instance, void* allocator, void** surface) {
    (void)window;
    (void)instance;
    (void)allocator;
    if (surface) {
        *surface = nullptr;
    }
    return false;
}

SDL_Cursor* SDL_CreateSystemCursor(SDL_SystemCursor id) {
    (void)id;
    return reinterpret_cast<SDL_Cursor*>(1);
}

bool SDL_SetCursor(SDL_Cursor* cursor) {
    (void)cursor;
    return true;
}

void SDL_DestroyCursor(SDL_Cursor* cursor) { (void)cursor; }

bool SDL_CaptureMouse(bool enabled) {
    (void)enabled;
    return true;
}

bool SDL_SetClipboardText(const char* text) {
    (void)text;
    return true;
}

char* SDL_GetClipboardText(void) {
    char* copy = static_cast<char*>(std::malloc(1));
    if (copy) {
        copy[0] = '\0';
    }
    return copy;
}

SDL_DisplayID* SDL_GetDisplays(int* count) {
    auto* displays = static_cast<SDL_DisplayID*>(SDL_malloc(2 * sizeof(SDL_DisplayID)));
    if (count) {
        *count = displays ? 1 : 0;
    }
    if (displays) {
        displays[0] = kDisplayId;
        displays[1] = 0;
    }
    return displays;
}

bool SDL_GetDisplayBounds(SDL_DisplayID display, SDL_Rect* rect) {
    (void)display;
    if (rect) {
        rect->x = 0;
        rect->y = 0;
        rect->w = static_cast<int>(kWindowWidth);
        rect->h = static_cast<int>(kWindowHeight);
    }
    return true;
}

bool SDL_GetDisplayUsableBounds(SDL_DisplayID display, SDL_Rect* rect) {
    return SDL_GetDisplayBounds(display, rect);
}

float SDL_GetDisplayContentScale(SDL_DisplayID display) {
    (void)display;
    return 1.f;
}

SDL_WindowID SDL_GetWindowID(SDL_Window* window) { return window ? 1 : 0; }

SDL_Window* SDL_GetWindowFromID(SDL_WindowID id) { (void)id; return nullptr; }

bool SDL_GetWindowPosition(SDL_Window* window, int* x, int* y) {
    (void)window;
    if (x) {
        *x = 0;
    }
    if (y) {
        *y = 0;
    }
    return true;
}

bool SDL_GetWindowRelativeMouseMode(SDL_Window* window) {
    (void)window;
    return false;
}

bool SDL_SetWindowOpacity(SDL_Window* window, float opacity) {
    (void)window;
    (void)opacity;
    return true;
}

bool SDL_SetWindowParent(SDL_Window* window, SDL_Window* parent) {
    (void)window;
    (void)parent;
    return true;
}

bool SDL_RaiseWindow(SDL_Window* window) { return window != nullptr; }

bool SDL_SetTextInputArea(SDL_Window* window, const SDL_Rect* rect, int cursor) {
    (void)window;
    (void)rect;
    (void)cursor;
    return true;
}

bool SDL_OpenURL(const char* url) {
    (void)url;
    return false;
}

void SDL_WarpMouseInWindow(SDL_Window* window, float x, float y) {
    (void)window;
    (void)x;
    (void)y;
}

bool SDL_WarpMouseGlobal(float x, float y) {
    (void)x;
    (void)y;
    return true;
}

bool SDL_SetRenderClipRect(SDL_Renderer* renderer, const SDL_Rect* rect) {
    (void)renderer;
    (void)rect;
    return true;
}

bool SDL_RenderClipEnabled(SDL_Renderer* renderer) {
    (void)renderer;
    return false;
}

bool SDL_GetRenderClipRect(SDL_Renderer* renderer, SDL_Rect* rect) {
    (void)renderer;
    if (rect) {
        std::memset(rect, 0, sizeof(*rect));
    }
    return true;
}

bool SDL_SetRenderViewport(SDL_Renderer* renderer, const SDL_Rect* rect) {
    (void)renderer;
    (void)rect;
    return true;
}

bool SDL_RenderViewportSet(SDL_Renderer* renderer) {
    (void)renderer;
    return false;
}

bool SDL_GetRenderViewport(SDL_Renderer* renderer, SDL_Rect* rect) {
    (void)renderer;
    if (rect) {
        std::memset(rect, 0, sizeof(*rect));
    }
    return true;
}

bool SDL_RenderGeometryRaw(SDL_Renderer* renderer, SDL_Texture* texture, const float* xy, int xy_stride,
                           const SDL_FColor* color, int color_stride, const float* uv, int uv_stride,
                           int num_vertices, const void* indices, int num_indices, int size_indices) {
    (void)renderer;
    (void)texture;
    (void)xy;
    (void)xy_stride;
    (void)color;
    (void)color_stride;
    (void)uv;
    (void)uv_stride;
    (void)num_vertices;
    (void)indices;
    (void)num_indices;
    (void)size_indices;
    return true;
}

bool SDL_SetTextureBlendMode(SDL_Texture* texture, SDL_BlendMode blendMode) {
    (void)texture;
    (void)blendMode;
    return true;
}

} // extern "C"
