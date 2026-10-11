#pragma once
#include <d3d11.h>

// Dear ImGui plumbing: context, Win32 + D3D11 backends, style, fonts and DPI
// scale. The widgets themselves are built by App::drawPanel (AppPanel.cpp).
//
// Per frame: newFrame() -> widgets -> endFrame() (ImGui::Render); render()
// draws the last finished frame and may be called more than once (screenshots
// re-render the scene with the same UI). Draw it on a non-sRGB view of the back
// buffer (Renderer::bindUiTarget): ImGui colours are gamma-space values.
class Ui {
public:
  bool init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context);
  void shutdown();

  // Forwards a window message to ImGui (no-op before init / after shutdown).
  // Non-zero: ImGui handled it (WM_SETCURSOR over a widget, IME characters) -
  // return that from the window procedure without DefWindowProc.
  LRESULT processMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
  // True while the mouse is over / dragging an ImGui window, or a text field
  // or slider is being edited: the app must not act on that input.
  bool wantsMouse() const;
  bool wantsKeyboard() const;

  // DPI scale (1 = 96 dpi): font size, padding, spacing.
  void setScale(float scale);
  float scale() const { return _scale; }

  // inputEnabled = false ignores mouse and keyboard (deterministic harness runs).
  void newFrame(bool inputEnabled);
  void endFrame();
  void render();

private:
  bool _initialized = false;
  float _scale = 1.0f;
};
