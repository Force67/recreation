#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "core/window.h"

namespace rec {
namespace {

Key TranslateKey(SDL_Scancode code) {
  switch (code) {
    case SDL_SCANCODE_W: return Key::kW;
    case SDL_SCANCODE_A: return Key::kA;
    case SDL_SCANCODE_S: return Key::kS;
    case SDL_SCANCODE_D: return Key::kD;
    case SDL_SCANCODE_Q: return Key::kQ;
    case SDL_SCANCODE_E: return Key::kE;
    case SDL_SCANCODE_F: return Key::kF;
    case SDL_SCANCODE_T: return Key::kT;
    case SDL_SCANCODE_C: return Key::kC;
    case SDL_SCANCODE_R: return Key::kR;
    case SDL_SCANCODE_G: return Key::kG;
    case SDL_SCANCODE_X: return Key::kX;
    case SDL_SCANCODE_Z: return Key::kZ;
    case SDL_SCANCODE_B: return Key::kB;
    case SDL_SCANCODE_V: return Key::kV;
    case SDL_SCANCODE_SPACE: return Key::kSpace;
    case SDL_SCANCODE_LSHIFT: return Key::kLeftShift;
    case SDL_SCANCODE_LCTRL: return Key::kLeftCtrl;
    case SDL_SCANCODE_ESCAPE: return Key::kEscape;
    case SDL_SCANCODE_F1: return Key::kF1;
    case SDL_SCANCODE_F2: return Key::kF2;
    case SDL_SCANCODE_F3: return Key::kF3;
    case SDL_SCANCODE_F4: return Key::kF4;
    case SDL_SCANCODE_F5: return Key::kF5;
    case SDL_SCANCODE_DELETE: return Key::kDelete;
    case SDL_SCANCODE_BACKSPACE: return Key::kBackspace;
    case SDL_SCANCODE_RETURN: return Key::kReturn;
    case SDL_SCANCODE_KP_ENTER: return Key::kReturn;
    case SDL_SCANCODE_1: return Key::k1;
    case SDL_SCANCODE_2: return Key::k2;
    case SDL_SCANCODE_3: return Key::k3;
    case SDL_SCANCODE_4: return Key::k4;
    case SDL_SCANCODE_J: return Key::kJ;
    case SDL_SCANCODE_UP: return Key::kArrowUp;
    case SDL_SCANCODE_DOWN: return Key::kArrowDown;
    case SDL_SCANCODE_LEFT: return Key::kArrowLeft;
    case SDL_SCANCODE_RIGHT: return Key::kArrowRight;
    case SDL_SCANCODE_TAB: return Key::kTab;
    default: return Key::kCount;
  }
}

MouseButton TranslateButton(u8 button) {
  switch (button) {
    case SDL_BUTTON_LEFT: return MouseButton::kLeft;
    case SDL_BUTTON_RIGHT: return MouseButton::kRight;
    case SDL_BUTTON_MIDDLE: return MouseButton::kMiddle;
    default: return MouseButton::kCount;
  }
}

GamepadButton TranslateGamepadButton(u8 button) {
  switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return GamepadButton::kSouth;
    case SDL_GAMEPAD_BUTTON_EAST: return GamepadButton::kEast;
    case SDL_GAMEPAD_BUTTON_WEST: return GamepadButton::kWest;
    case SDL_GAMEPAD_BUTTON_NORTH: return GamepadButton::kNorth;
    case SDL_GAMEPAD_BUTTON_BACK: return GamepadButton::kBack;
    case SDL_GAMEPAD_BUTTON_GUIDE: return GamepadButton::kGuide;
    case SDL_GAMEPAD_BUTTON_START: return GamepadButton::kStart;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return GamepadButton::kLeftStick;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return GamepadButton::kRightStick;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return GamepadButton::kLeftShoulder;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return GamepadButton::kRightShoulder;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return GamepadButton::kDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return GamepadButton::kDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return GamepadButton::kDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return GamepadButton::kDpadRight;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD: return GamepadButton::kTouchpad;
    default: return GamepadButton::kCount;
  }
}

GamepadAxis TranslateGamepadAxis(u8 axis) {
  switch (axis) {
    case SDL_GAMEPAD_AXIS_LEFTX: return GamepadAxis::kLeftX;
    case SDL_GAMEPAD_AXIS_LEFTY: return GamepadAxis::kLeftY;
    case SDL_GAMEPAD_AXIS_RIGHTX: return GamepadAxis::kRightX;
    case SDL_GAMEPAD_AXIS_RIGHTY: return GamepadAxis::kRightY;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return GamepadAxis::kLeftTrigger;
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return GamepadAxis::kRightTrigger;
    default: return GamepadAxis::kCount;
  }
}

GamepadState::Kind GamepadKind(SDL_Gamepad* pad) {
  switch (SDL_GetGamepadType(pad)) {
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return GamepadState::Kind::kDualSense;
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return GamepadState::Kind::kXbox;
    default: return GamepadState::Kind::kUnknown;
  }
}

class Sdl3Window final : public Window {
 public:
  explicit Sdl3Window(SDL_Window* window) : window_(window) {}

  ~Sdl3Window() override {
    CloseGamepad();
    SDL_DestroyWindow(window_);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
  }

  bool PumpEvents() override {
    input_.mouse_dx = 0;
    input_.mouse_dy = 0;
    input_.wheel = 0;
    input_.text_len = 0;
    input_.text[0] = '\0';
    std::memset(input_.pressed, 0, sizeof(input_.pressed));
    std::memset(gamepad_.pressed, 0, sizeof(gamepad_.pressed));

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event_hook_) event_hook_(&event);
      switch (event.type) {
        case SDL_EVENT_QUIT:
          return false;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
          Key key = TranslateKey(event.key.scancode);
          if (key == Key::kCount) break;
          bool down = event.type == SDL_EVENT_KEY_DOWN;
          if (down && !event.key.repeat && !input_.keys[static_cast<u8>(key)]) {
            input_.pressed[static_cast<u8>(key)] = true;
          }
          input_.keys[static_cast<u8>(key)] = down;
          break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
          input_.mouse_x = event.button.x;
          input_.mouse_y = event.button.y;
          MouseButton button = TranslateButton(event.button.button);
          if (button == MouseButton::kCount) break;
          input_.mouse[static_cast<u8>(button)] = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
          break;
        }
        case SDL_EVENT_MOUSE_MOTION:
          input_.mouse_dx += event.motion.xrel;
          input_.mouse_dy += event.motion.yrel;
          input_.mouse_x = event.motion.x;
          input_.mouse_y = event.motion.y;
          break;
        case SDL_EVENT_MOUSE_WHEEL:
          input_.wheel += event.wheel.y;
          break;
        case SDL_EVENT_TEXT_INPUT: {
          // Append the UTF-8 text to this pump's buffer (truncated to capacity).
          const char* t = event.text.text;
          while (*t && input_.text_len < sizeof(input_.text) - 1) {
            input_.text[input_.text_len++] = *t++;
          }
          input_.text[input_.text_len] = '\0';
          break;
        }
        case SDL_EVENT_GAMEPAD_ADDED:
          OpenGamepad(event.gdevice.which);
          break;
        case SDL_EVENT_GAMEPAD_REMOVED:
          if (pad_ && event.gdevice.which == pad_id_) CloseGamepad();
          break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: {
          if (event.gbutton.which != pad_id_) break;
          GamepadButton b = TranslateGamepadButton(event.gbutton.button);
          if (b == GamepadButton::kCount) break;
          bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
          if (down && !gamepad_.buttons[static_cast<u8>(b)])
            gamepad_.pressed[static_cast<u8>(b)] = true;
          gamepad_.buttons[static_cast<u8>(b)] = down;
          break;
        }
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
          if (event.gaxis.which != pad_id_) break;
          GamepadAxis a = TranslateGamepadAxis(event.gaxis.axis);
          if (a == GamepadAxis::kCount) break;
          bool trigger = a == GamepadAxis::kLeftTrigger || a == GamepadAxis::kRightTrigger;
          // Sticks span the full i16 range; triggers run 0..32767.
          f32 v = trigger ? event.gaxis.value / 32767.0f : event.gaxis.value / 32768.0f;
          gamepad_.axes[static_cast<u8>(a)] = std::clamp(v, -1.0f, 1.0f);
          break;
        }
        default:
          break;
      }
    }
    return true;
  }

  void SetRelativeMouseMode(bool enabled) override {
    SDL_SetWindowRelativeMouseMode(window_, enabled);
  }

  bool relative_mouse_mode() const override {
    return SDL_GetWindowRelativeMouseMode(window_);
  }

  void SetRumble(f32 low_freq, f32 high_freq, u32 duration_ms) override {
    if (!pad_) return;
    auto scale = [](f32 v) { return static_cast<Uint16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
    SDL_RumbleGamepad(pad_, scale(low_freq), scale(high_freq), duration_ms);
  }

  void SetLedColor(u8 r, u8 g, u8 b) override {
    if (pad_) SDL_SetGamepadLED(pad_, r, g, b);
  }

  void SetTriggerEffect(bool left, bool right, const TriggerEffect& effect) override {
    // DualSense-only. Builds a minimal DS5 output-report effect block and sends
    // it through SDL's HIDAPI driver; a no-op on other pads (kind guard) and a
    // best-effort on unsupported firmware (SDL just rejects the report).
    if (!pad_ || gamepad_.kind != GamepadState::Kind::kDualSense) return;
    SendDualSenseTriggers(left, right, effect);
  }

  NativeWindowHandles native_handles() const override {
    // The renderer goes through SDL_Vulkan_CreateSurface, which takes the
    // SDL_Window itself rather than platform handles.
    return {window_, nullptr};
  }

  u32 width() const override {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return static_cast<u32>(w);
  }

  u32 height() const override {
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window_, &w, &h);
    return static_cast<u32>(h);
  }

  std::vector<const char*> vulkan_instance_extensions() const override {
    Uint32 count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!extensions) return {};
    return {extensions, extensions + count};
  }

  bool CreateVulkanSurface(void* vk_instance, void* out_vk_surface) override {
    return SDL_Vulkan_CreateSurface(window_, static_cast<VkInstance>(vk_instance), nullptr,
                                    static_cast<VkSurfaceKHR*>(out_vk_surface));
  }

 private:
  void OpenGamepad(SDL_JoystickID id) {
    if (pad_) return;  // first pad wins; ignore additional controllers
    SDL_Gamepad* pad = SDL_OpenGamepad(id);
    if (!pad) return;
    pad_ = pad;
    pad_id_ = id;
    gamepad_ = {};
    gamepad_.connected = true;
    gamepad_.kind = GamepadKind(pad);
    REC_INFO("gamepad connected: {} ({})", SDL_GetGamepadName(pad),
             gamepad_.kind == GamepadState::Kind::kDualSense ? "DualSense"
             : gamepad_.kind == GamepadState::Kind::kXbox    ? "Xbox"
                                                             : "generic");
  }

  void CloseGamepad() {
    if (!pad_) return;
    SDL_CloseGamepad(pad_);
    pad_ = nullptr;
    pad_id_ = 0;
    gamepad_ = {};
    REC_INFO("gamepad disconnected");
  }

  // Sends a DualSense adaptive-trigger effect. The output report's effect block
  // is: [0] mode, [1] start position, [2] force; modes follow the community DS5
  // spec (0x01 resistance, 0x02 weapon/trigger, 0x26 vibration).
  void SendDualSenseTriggers(bool left, bool right, const TriggerEffect& fx) {
    Uint8 mode = 0x05;  // 0x05 = effect off / release
    switch (fx.type) {
      case TriggerEffect::Type::kResistance: mode = 0x01; break;
      case TriggerEffect::Type::kWeapon: mode = 0x02; break;
      case TriggerEffect::Type::kVibration: mode = 0x26; break;
      case TriggerEffect::Type::kOff: mode = 0x05; break;
    }
    Uint8 right_fx[11] = {mode, fx.start, fx.strength};
    Uint8 left_fx[11] = {mode, fx.start, fx.strength};

    // DS5EffectsState_t: enable bits then the per-trigger effect blocks. Bit
    // 0x04 = right trigger, 0x08 = left trigger.
    Uint8 report[48] = {};
    report[0] = static_cast<Uint8>((right ? 0x04 : 0) | (left ? 0x08 : 0));
    if (right) std::memcpy(&report[10], right_fx, sizeof(right_fx));
    if (left) std::memcpy(&report[21], left_fx, sizeof(left_fx));
    SDL_SendGamepadEffect(pad_, report, sizeof(report));
  }

  SDL_Window* window_;
  SDL_Gamepad* pad_ = nullptr;
  SDL_JoystickID pad_id_ = 0;
};

}  // namespace

std::unique_ptr<Window> CreateSdl3Window(const WindowDesc& desc) {
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
    REC_ERROR("sdl init failed: {}", SDL_GetError());
    return nullptr;
  }
  SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
  if (desc.fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
  SDL_Window* window = SDL_CreateWindow(desc.title.c_str(), static_cast<int>(desc.width),
                                        static_cast<int>(desc.height), flags);
  if (!window) {
    REC_ERROR("sdl window creation failed: {}", SDL_GetError());
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    return nullptr;
  }
  // Receive SDL_EVENT_TEXT_INPUT so editor text fields get typed characters.
  // Key events still arrive; this only adds the translated text stream.
  SDL_StartTextInput(window);

  // Enable the PS5 HIDAPI driver with enhanced reports so the DualSense exposes
  // rumble, the lightbar, and adaptive triggers (set before the subsystem inits).
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    // Non-fatal: keyboard/mouse still work, gamepads just won't be seen.
    REC_WARN("sdl gamepad subsystem init failed: {}", SDL_GetError());
  }
  return std::make_unique<Sdl3Window>(window);
}

}  // namespace rec
