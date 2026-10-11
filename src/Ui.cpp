#include "pch.h"
#include "Ui.h"

#include "Log.h"

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

constexpr float kFontSize = 15.0f; // pixels at 96 dpi

// Unscaled style: dark, translucent like the D2D HUD on the left.
ImGuiStyle baseStyle() {
  ImGuiStyle s;
  ImGui::StyleColorsDark(&s);
  s.WindowPadding = ImVec2(10.0f, 8.0f);
  s.FramePadding = ImVec2(6.0f, 3.0f);
  s.ItemSpacing = ImVec2(8.0f, 5.0f);
  s.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
  s.WindowRounding = 0.0f;
  s.FrameRounding = 3.0f;
  s.GrabRounding = 3.0f;
  s.WindowBorderSize = 0.0f;
  s.ScrollbarSize = 12.0f;
  ImVec4* c = s.Colors;
  c[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.62f);
  c[ImGuiCol_PopupBg] = ImVec4(0.06f, 0.07f, 0.08f, 0.96f);
  c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.12f, 0.15f, 0.85f);
  c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.16f, 0.22f, 0.90f);
  c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.0f, 0.0f, 0.0f, 0.62f);
  c[ImGuiCol_Header] = ImVec4(0.26f, 0.42f, 0.62f, 0.45f);
  c[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.23f, 0.28f, 0.70f);
  return s;
}

} // namespace

bool Ui::init(HWND hwnd, ID3D11Device* device, ID3D11DeviceContext* context) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr; // fixed layout; nothing is written next to the exe
  io.LogFilename = nullptr;

  wchar_t windowsDir[MAX_PATH] = {};
  const UINT len = GetWindowsDirectoryW(windowsDir, MAX_PATH);
  const std::filesystem::path font = std::filesystem::path(std::wstring(windowsDir, len)) / L"Fonts" / L"segoeui.ttf";
  std::error_code ec;
  if (len == 0 || !std::filesystem::exists(font, ec) ||
      !io.Fonts->AddFontFromFileTTF(Log::toUtf8(font.wstring()).c_str(), kFontSize)) {
    LOG_W("UI font {} not available, using the built-in Dear ImGui font", Log::toUtf8(font.wstring()));
    io.Fonts->AddFontDefault();
  }

  if (!ImGui_ImplWin32_Init(hwnd)) {
    LOG_E("Dear ImGui Win32 backend initialization failed");
    ImGui::DestroyContext();
    return false;
  }
  if (!ImGui_ImplDX11_Init(device, context)) {
    LOG_E("Dear ImGui D3D11 backend initialization failed");
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return false;
  }
  _initialized = true;
  setScale(_scale);
  LOG_I("Dear ImGui {} initialized", ImGui::GetVersion());
  return true;
}

void Ui::shutdown() {
  if (!_initialized) {
    return;
  }
  _initialized = false;
  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
}

LRESULT Ui::processMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  return _initialized ? ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp) : 0;
}

bool Ui::wantsMouse() const { return _initialized && ImGui::GetIO().WantCaptureMouse; }

bool Ui::wantsKeyboard() const { return _initialized && ImGui::GetIO().WantCaptureKeyboard; }

void Ui::setScale(float scale) {
  _scale = std::clamp(scale, 0.5f, 4.0f);
  if (!_initialized) {
    return;
  }
  ImGuiStyle s = baseStyle();
  s.ScaleAllSizes(_scale);
  s.FontScaleDpi = _scale;
  ImGui::GetStyle() = s;
}

void Ui::newFrame(bool inputEnabled) {
  if (!_initialized) {
    return;
  }
  ImGuiIO& io = ImGui::GetIO();
  const ImGuiConfigFlags noInput = ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoKeyboard;
  io.ConfigFlags = inputEnabled ? (io.ConfigFlags & ~noInput) : (io.ConfigFlags | noInput);
  ImGui_ImplDX11_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();
}

void Ui::endFrame() {
  if (_initialized) {
    ImGui::Render();
  }
}

void Ui::render() {
  if (!_initialized) {
    return;
  }
  ImDrawData* drawData = ImGui::GetDrawData();
  if (drawData && drawData->Valid) {
    ImGui_ImplDX11_RenderDrawData(drawData);
  }
}
