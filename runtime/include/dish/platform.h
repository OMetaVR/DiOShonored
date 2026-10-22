// Platform layer shared declarations (user32/gdi32/xinput/dinput8/dsound/d3d9 over SDL3).
// Everything that touches SDL windows or the event queue runs on the process main thread via mainthread_call;
// guest-visible state (focus, key state, cursor position, window rects) is mirrored in atomics so the hot Win32 queries
// (GetKeyState 4.8 M calls, GetFocus 182 k, GetCursorPos 45 k ...) never take a lock or call into SDL.
#pragma once
#include <atomic>
#include <cstdint>
#include <vector>

struct SDL_Window;
union SDL_Event;

namespace dish {

// ---- video / event pump
bool platform_video_init();                    // SDL_InitSubSystem(VIDEO|EVENTS|GAMEPAD) once; false when no display is available
void platform_pump();                          // poll and route SDL events; main thread (or the only thread when no main loop is running)
void platform_dev_input();                     // DISH_DEV_KEYS scripted key presses (test runs); main thread, before polling SDL
void platform_shutdown();
bool platform_route_event(const SDL_Event &e); // called by the host loop for events it polled itself
SDL_Window *platform_sdl_window(uint32_t hwnd);   // nullptr when the window has no SDL backing (child/message-only/unknown)
uint32_t platform_main_hwnd();                 // first top-level window, 0 if none
void platform_drawable_size(uint32_t hwnd, int *w, int *h);   // pixel size of the client area

// ---- input state shared by user32, dinput8 and xinput
extern std::atomic<uint8_t> g_vk_down[256];    // bit0 = down; bit1 = toggled (caps lock style)
struct MouseAccum { std::atomic<int32_t> dx{0}, dy{0}, wheel{0}; std::atomic<uint32_t> buttons{0}; };
extern MouseAccum g_mouse;                     // relative motion since the last dinput read; buttons bit0..4 (L,R,M,X1,X2)
extern std::atomic<int32_t> g_cursor_x, g_cursor_y;   // screen coordinates

// buffered mouse events for DirectInput (DIMOFS_X=0, Y=4, Z=8, BUTTON0..=12..): pushed by the SDL event translation, drained by dinput8
struct MouseEvent { uint32_t ofs, data, time, seq; };
void input_mouse_push(uint32_t ofs, uint32_t data);
size_t input_mouse_pop(MouseEvent *out, size_t max, bool peek);
void input_mouse_clear();

// ---- gamepads (xinput.cpp)
void xinput_on_event(const SDL_Event &e);
void xinput_shutdown();

// ---- audio (dsound.cpp)
void dsound_shutdown();
// ole32 CoCreateInstance hook: returns true when `clsid` is one of ours (then *hr / *out_obj are the result).
bool dsound_co_create(uint32_t clsid, uint32_t iid, uint32_t *out_obj, uint32_t *hr);
uint64_t dsound_frames_mixed();
void dsound_set_capture(std::vector<float> *sink);   // tests: record the mixer output (interleaved float stereo, 48 kHz)

}  // namespace dish
