// ge - ReXGlue Recompiled Project
//
// In-game pause menu implementation. See ge_menu.h.
//
// Drawn procedurally for now (flat fills + outlines that approximate the
// manila-folder briefing screen). Every visual block is marked `// TEXTURE:`
// so it can be swapped for an ImGui::Image() once the Photoshop art exists --
// the layout geometry stays the same.

#include "ge_menu.h"
#include "ge_postfx.h"

#include <rex/cvar.h>
#include <rex/ui/keybinds.h>     // VirtualKeyToString (key rebinding)
#include <rex/ui/virtual_key.h>

#include <imgui.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// --- Small cvar accessors (the menu reads/writes settings by name) ---
float GetCvarF(const char* name) {
  return static_cast<float>(std::atof(rex::cvar::GetFlagByName(name).c_str()));
}
void SetCvarF(const char* name, float v) { rex::cvar::SetFlagByName(name, std::to_string(v)); }
bool GetCvarB(const char* name) { return rex::cvar::GetFlagByName(name) == "true"; }
void SetCvarB(const char* name, bool v) { rex::cvar::SetFlagByName(name, v ? "true" : "false"); }

// --- Briefing palette (sampled from the reference screenshot) ---
constexpr ImU32 kFolder = IM_COL32(214, 201, 162, 255);     // manila paper
constexpr ImU32 kFolderEdge = IM_COL32(120, 108, 78, 255);  // darker rim
constexpr ImU32 kTab = IM_COL32(196, 184, 146, 255);        // unselected tab
constexpr ImU32 kTabSel = IM_COL32(224, 213, 176, 255);     // selected tab
constexpr ImU32 kInk = IM_COL32(52, 44, 32, 255);           // body text
constexpr ImU32 kInkDim = IM_COL32(92, 82, 60, 255);        // secondary text
constexpr ImU32 kTitle = IM_COL32(40, 33, 24, 255);         // big serif title
constexpr ImU32 kReticle = IM_COL32(196, 36, 28, 255);      // red selection crosshair
constexpr ImU32 kStamp = IM_COL32(176, 42, 34, 70);         // faded CLASSIFIED stamp

struct Tab {
  const char* label;  // short, drawn vertically on the tab
  const char* title;  // big serif heading shown in the body
};

constexpr Tab kTabs[] = {
    {"AUDIO", "AUDIO"},
    {"VIDEO", "VIDEO"},
    {"INPUT", "CONTROLS"},
    {"ONLINE", "ONLINE"},
    {"SYSTEM", "SYSTEM"},
};
constexpr int kTabCount = static_cast<int>(sizeof(kTabs) / sizeof(kTabs[0]));

// Rotate the glyphs AddText() just appended about `pivot` by `angle` radians.
void AddTextRotated(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col,
                    const char* text, float angle, ImVec2 pivot) {
  int v0 = dl->VtxBuffer.Size;
  dl->AddText(font, size, pos, col, text);
  int v1 = dl->VtxBuffer.Size;
  const float s = std::sin(angle), c = std::cos(angle);
  for (int i = v0; i < v1; ++i) {
    ImDrawVert& v = dl->VtxBuffer[i];
    const float dx = v.pos.x - pivot.x, dy = v.pos.y - pivot.y;
    v.pos.x = pivot.x + dx * c - dy * s;
    v.pos.y = pivot.y + dx * s + dy * c;
  }
}

// Draw a short label as a centered vertical stack of characters.
void AddVerticalLabel(ImDrawList* dl, ImFont* font, float size, ImVec2 center, ImU32 col,
                      const char* text) {
  const float line_h = size + 1.0f;
  int n = 0;
  for (const char* p = text; *p; ++p) ++n;
  float y = center.y - (n * line_h) * 0.5f;
  for (const char* p = text; *p; ++p, y += line_h) {
    char ch[2] = {*p, 0};
    ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, ch);
    dl->AddText(font, size, ImVec2(center.x - sz.x * 0.5f, y), col, ch);
  }
}

// Keyboard rebinding ---------------------------------------------------------
struct KeyBind {
  const char* label;
  const char* cvar;
};
// Order = display order. Right-stick (look) is the mouse, so it is not here.
constexpr KeyBind kBinds[] = {
    {"Move Forward", "ge_key_mv_up"},   {"Move Back", "ge_key_mv_down"},
    {"Move Left", "ge_key_mv_left"},    {"Move Right", "ge_key_mv_right"},
    {"A", "ge_key_a"},                  {"B", "ge_key_b"},
    {"X", "ge_key_x"},                  {"Y", "ge_key_y"},
    {"Left Trigger", "ge_key_lt"},      {"Right Trigger", "ge_key_rt"},
    {"Left Bumper", "ge_key_lb"},       {"Right Bumper", "ge_key_rb"},
    {"Left Stick (L3)", "ge_key_l3"},   {"Right Stick (R3)", "ge_key_r3"},
    {"D-Pad Up", "ge_key_dup"},         {"D-Pad Down", "ge_key_ddown"},
    {"D-Pad Left", "ge_key_dleft"},     {"D-Pad Right", "ge_key_dright"},
    {"Start", "ge_key_start"},          {"Back", "ge_key_back"},
};

// Map an ImGui key to a Windows virtual-key code (== rex VirtualKey value).
int ImGuiKeyToVk(ImGuiKey k) {
  if (k >= ImGuiKey_A && k <= ImGuiKey_Z) return 'A' + (k - ImGuiKey_A);
  if (k >= ImGuiKey_0 && k <= ImGuiKey_9) return '0' + (k - ImGuiKey_0);
  if (k >= ImGuiKey_Keypad0 && k <= ImGuiKey_Keypad9) return 0x60 + (k - ImGuiKey_Keypad0);
  if (k >= ImGuiKey_F1 && k <= ImGuiKey_F12) return 0x70 + (k - ImGuiKey_F1);
  switch (k) {
    case ImGuiKey_Space: return 0x20;
    case ImGuiKey_Enter: return 0x0D;
    case ImGuiKey_Tab: return 0x09;
    case ImGuiKey_Backspace: return 0x08;
    case ImGuiKey_Delete: return 0x2E;
    case ImGuiKey_Insert: return 0x2D;
    case ImGuiKey_Home: return 0x24;
    case ImGuiKey_End: return 0x23;
    case ImGuiKey_PageUp: return 0x21;
    case ImGuiKey_PageDown: return 0x22;
    case ImGuiKey_LeftArrow: return 0x25;
    case ImGuiKey_RightArrow: return 0x27;
    case ImGuiKey_UpArrow: return 0x26;
    case ImGuiKey_DownArrow: return 0x28;
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift: return 0x10;
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl: return 0x11;
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt: return 0x12;
    case ImGuiKey_GraveAccent: return 0xC0;
    case ImGuiKey_Minus: return 0xBD;
    case ImGuiKey_Equal: return 0xBB;
    case ImGuiKey_Comma: return 0xBC;
    case ImGuiKey_Period: return 0xBE;
    case ImGuiKey_Semicolon: return 0xBA;
    case ImGuiKey_Slash: return 0xBF;
    case ImGuiKey_Backslash: return 0xDC;
    case ImGuiKey_LeftBracket: return 0xDB;
    case ImGuiKey_RightBracket: return 0xDD;
    case ImGuiKey_Apostrophe: return 0xDE;
    case ImGuiKey_CapsLock: return 0x14;
    case ImGuiKey_KeypadEnter: return 0x0D;
    case ImGuiKey_KeypadAdd: return 0x6B;
    case ImGuiKey_KeypadSubtract: return 0x6D;
    case ImGuiKey_KeypadMultiply: return 0x6A;
    case ImGuiKey_KeypadDivide: return 0x6F;
    default: return 0;
  }
}

}  // namespace

GeMenuDialog::GeMenuDialog(rex::ui::ImGuiDrawer* drawer, Callbacks callbacks)
    : rex::ui::ImGuiDialog(drawer), callbacks_(std::move(callbacks)) {}

GeMenuDialog::~GeMenuDialog() = default;

void GeMenuDialog::RequestClose() { Close(); }

void GeMenuDialog::OnClose() {
  if (callbacks_.on_closed) callbacks_.on_closed();
  if (quit_requested_ && callbacks_.on_quit) callbacks_.on_quit();
}

void GeMenuDialog::OnDraw(ImGuiIO& io) {
  // Keyboard tab navigation.
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
    selected_tab_ = (selected_tab_ - 1 + kTabCount) % kTabCount;
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
    selected_tab_ = (selected_tab_ + 1) % kTabCount;

  const ImVec2 disp = io.DisplaySize;

  // --- Centered portrait panel (folder body + right-edge tab strip) ---
  const float folder_h = std::floor(disp.y * 0.82f);
  const float folder_w = std::floor(folder_h * 0.72f);  // portrait: taller than wide
  tab_w_ = std::floor(folder_w * 0.13f);
  const float total_w = folder_w + tab_w_;
  const ImVec2 origin(std::floor((disp.x - total_w) * 0.5f),
                      std::floor((disp.y - folder_h) * 0.5f));
  f0_ = origin;
  f1_ = ImVec2(origin.x + folder_w, origin.y + folder_h);

  ImGui::SetNextWindowPos(origin);
  ImGui::SetNextWindowSize(ImVec2(total_w, folder_h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                           ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground |
                           ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav;
  if (!ImGui::Begin("##ge_pause_menu", nullptr, flags)) {
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }

  DrawFolder(io);
  DrawTabs(io);
  DrawContent(io);

  ImGui::End();
  ImGui::PopStyleVar();
}

void GeMenuDialog::DrawFolder(ImGuiIO& /*io*/) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 f0 = f0_, f1 = f1_;
  const float fw = f1.x - f0.x, fh = f1.y - f0.y;

  // Drop shadow.
  dl->AddRectFilled(ImVec2(f0.x + 10, f0.y + 12), ImVec2(f1.x + 10, f1.y + 12),
                    IM_COL32(0, 0, 0, 90), 6.0f);
  // TEXTURE: manila folder body.
  dl->AddRectFilled(f0, f1, kFolder, 6.0f);
  dl->AddRect(f0, f1, kFolderEdge, 6.0f, 0, 2.0f);

  // TEXTURE: diagonal CLASSIFIED stamp (behind body content).
  {
    const char* stamp = "CLASSIFIED";
    const float ssize = std::floor(fw * 0.16f);
    ImVec2 ssz = ImGui::GetFont()->CalcTextSizeA(ssize, FLT_MAX, 0.0f, stamp);
    ImVec2 spos((f0.x + f1.x) * 0.5f - ssz.x * 0.5f, f0.y + fh * 0.62f - ssz.y * 0.5f);
    AddTextRotated(dl, ImGui::GetFont(), ssize, spos, kStamp, stamp, -0.5f,
                   ImVec2((f0.x + f1.x) * 0.5f, f0.y + fh * 0.62f));
  }
}

void GeMenuDialog::DrawTabs(ImGuiIO& /*io*/) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 f0 = f0_, f1 = f1_;
  const float fh = f1.y - f0.y;
  const float folder_right = f1.x;

  const float strip_top = f0.y + std::floor(fh * 0.10f);
  const float gap = std::floor(fh * 0.025f);
  const float tab_h = std::floor((f1.y - strip_top - gap * (kTabCount - 1)) / kTabCount);
  const float label_size = std::floor(tab_w_ * 0.32f);

  for (int i = 0; i < kTabCount; ++i) {
    const float ty0 = strip_top + i * (tab_h + gap);
    const float ty1 = ty0 + tab_h;
    const bool sel = (i == selected_tab_);
    // Selected tab overlaps the folder edge so it reads as the open page.
    const float left = sel ? folder_right - 4.0f : folder_right + 3.0f;
    const float right = folder_right + tab_w_;
    const ImVec2 t0(left, ty0), t1(right, ty1);

    if (ImGui::IsMouseHoveringRect(t0, t1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      selected_tab_ = i;

    // TEXTURE: tab (selected vs unselected).
    dl->AddRectFilled(t0, t1, sel ? kTabSel : kTab, 5.0f, ImDrawFlags_RoundCornersRight);
    dl->AddRect(t0, t1, kFolderEdge, 5.0f, ImDrawFlags_RoundCornersRight, 1.5f);

    AddVerticalLabel(dl, ImGui::GetFont(), label_size,
                     ImVec2((left + right) * 0.5f + (sel ? 2.0f : 0.0f), (ty0 + ty1) * 0.5f), kInk,
                     kTabs[i].label);

    // TEXTURE: red reticle next to the selected tab.
    if (sel) {
      const ImVec2 c(folder_right - 16.0f, (ty0 + ty1) * 0.5f);
      const float r = 8.0f;
      dl->AddCircle(c, r, kReticle, 16, 2.0f);
      dl->AddLine(ImVec2(c.x - r - 4, c.y), ImVec2(c.x + r + 4, c.y), kReticle, 2.0f);
      dl->AddLine(ImVec2(c.x, c.y - r - 4), ImVec2(c.x, c.y + r + 4), kReticle, 2.0f);
    }
  }
}

void GeMenuDialog::DrawContent(ImGuiIO& /*io*/) {
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 f0 = f0_, f1 = f1_;
  const float fw = f1.x - f0.x, fh = f1.y - f0.y;

  const float pad = std::floor(fw * 0.08f);
  const float title_size = std::floor(fh * 0.065f);

  // TEXTURE: big serif section title (top-left).
  dl->AddText(ImGui::GetFont(), title_size, ImVec2(f0.x + pad, f0.y + pad * 0.7f), kTitle,
              kTabs[selected_tab_].title);
  const float rule_y = f0.y + pad * 0.7f + title_size + 6.0f;
  dl->AddLine(ImVec2(f0.x + pad, rule_y), ImVec2(f1.x - pad, rule_y), kInkDim, 2.0f);

  // --- Interactive content per tab ---
  ImGui::PushStyleColor(ImGuiCol_Text, kInk);
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(180, 168, 132, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(190, 178, 142, 255));
  ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(170, 158, 122, 255));
  ImGui::PushStyleColor(ImGuiCol_SliderGrab, kReticle);
  ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(220, 60, 50, 255));
  ImGui::PushStyleColor(ImGuiCol_CheckMark, kReticle);
  ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(180, 168, 132, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(198, 186, 150, 255));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(166, 154, 118, 255));
  // Dropdown popups: a panel slightly darker than the folder, so the dark ink
  // text stays readable (ImGui's default popup background is near-black).
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(190, 178, 140, 255));
  ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(168, 150, 104, 255));
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(184, 162, 112, 255));
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(160, 138, 96, 255));
  // Scrollbars (in the dropdown popups and the content panel) -- dark on tan so
  // they're actually visible and grabbable.
  ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(172, 160, 124, 200));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(118, 102, 70, 255));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, IM_COL32(140, 122, 84, 255));
  ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, IM_COL32(96, 82, 56, 255));

  // Enlarge the interactive content (text + widgets). Tabs/title are drawn via
  // the draw list with explicit sizes, so they are unaffected by this scale.
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(14.0f, 14.0f));

  const ImVec2 content_pos(f0.x + pad, rule_y + std::floor(fh * 0.05f));
  const float content_w = (f1.x - pad) - content_pos.x;
  const float content_h = (f1.y - std::floor(fh * 0.11f)) - content_pos.y;
  ImGui::SetCursorScreenPos(content_pos);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
  // Scrollable so longer tabs (e.g. Post-FX) fit the portrait folder.
  ImGui::BeginChild("##gecontent", ImVec2(content_w, content_h), false,
                    ImGuiWindowFlags_NoBackground);
  ImGui::SetWindowFontScale(1.5f);
  ImGui::PushItemWidth(content_w * 0.62f);

  switch (selected_tab_) {
    case 0: {  // AUDIO
      // Master volume -- live: the SDL mixer reads master_volume each callback,
      // so the slider is audible immediately. Persisted to ge.toml on release
      // (mouse-up) so it survives a restart.
      float vol = GetCvarF("master_volume");
      if (ImGui::SliderFloat("Master Volume", &vol, 0.0f, 1.0f, "%.2f")) {
        if (vol < 0.0f) vol = 0.0f;
        if (vol > 1.0f) vol = 1.0f;
        SetCvarF("master_volume", vol);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();
      ImGui::Spacing();
      ImGui::TextColored(ImColor(kInkDim), "(applies instantly)");
      break;
    }
    case 1: {  // VIDEO
      // --- Fullscreen (live; the request is applied deferred, off the paint
      //     thread, so the surface is never torn down mid-frame) ---
      bool fs = callbacks_.get_fullscreen
                    ? callbacks_.get_fullscreen()
                    : (rex::cvar::GetFlagByName("fullscreen") == "true");
      if (ImGui::Checkbox("Fullscreen", &fs)) {
        if (callbacks_.request_fullscreen) callbacks_.request_fullscreen(fs);
        rex::cvar::SetFlagByName("fullscreen", fs ? "true" : "false");
        if (callbacks_.persist_config) callbacks_.persist_config();
      }

      // --- V-Sync (live; the GPU vsync worker reads this each frame) ---
      bool vsync = rex::cvar::GetFlagByName("vsync") == "true";
      if (ImGui::Checkbox("V-Sync", &vsync)) {
        rex::cvar::SetFlagByName("vsync", vsync ? "true" : "false");
        if (callbacks_.persist_config) callbacks_.persist_config();
      }

      // --- Frame limit. NOTE: GoldenEye renders internally at 60Hz, so values
      //     above 60 are clamped to 60 by the vblank worker (driving it faster
      //     floods the guest interrupt and freezes the picture). Listed anyway
      //     per request; >60 simply stays 60. ---
      static const struct { const char* label; const char* value; } kFps[] = {
          {"30 FPS", "30"},   {"60 FPS", "60"},   {"90 FPS", "90"},
          {"120 FPS", "120"}, {"144 FPS", "144"}, {"165 FPS", "165"},
          {"180 FPS", "180"}, {"240 FPS", "240"}, {"300 FPS", "300"},
          {"Uncapped", "0"}};
      const std::string cur_fps = rex::cvar::GetFlagByName("max_fps");
      int fps_idx = 0;
      for (int i = 0; i < IM_ARRAYSIZE(kFps); ++i)
        if (cur_fps == kFps[i].value) fps_idx = i;
      ImGui::BeginDisabled(vsync);
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 6.0f));
      if (ImGui::BeginCombo("Frame Limit", vsync ? "V-Synced" : kFps[fps_idx].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kFps); ++i) {
          bool sel = (i == fps_idx);
          if (ImGui::Selectable(kFps[i].label, sel)) {
            rex::cvar::SetFlagByName("max_fps", kFps[i].value);
            if (callbacks_.persist_config) callbacks_.persist_config();
          }
          if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::EndDisabled();

      // --- GPU throttle. Pauses the emulated GPU command worker after each ring
      //     drain so it can't outrun the render thread -- the cause of the
      //     intermittent picture freeze. Higher = fewer freezes but more input
      //     latency; lower = snappier but more freeze risk; 0 = off. Applied
      //     live (the CP worker reads it each drain); saved to ge.toml on
      //     release. This is the supported way to tune it -- editing ge.toml by
      //     hand is fragile because the game rewrites the file on every save. ---
      int throttle_us = std::atoi(rex::cvar::GetFlagByName("ge_gpu_throttle_us").c_str());
      if (ImGui::SliderInt("GPU Throttle (us)", &throttle_us, 0, 500)) {
        if (throttle_us < 0) throttle_us = 0;
        rex::cvar::SetFlagByName("ge_gpu_throttle_us", std::to_string(throttle_us));
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();
      ImGui::TextColored(ImColor(kInkDim),
                         "(fixes picture freeze; higher = fewer freezes, more lag)");

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      // --- Internal resolution (supersampling). This is the resolution the 3D
      //     scene is actually rendered at, independent of the window size: the
      //     game's native 1280x720 surfaces are scaled by an integer factor and
      //     downsampled to the window. Driven by the engine's resolution_scale
      //     cvar (1x/2x/3x/4x). It is kRequiresRestart, so the choice is written
      //     now and the render targets are rebuilt at this size on next launch.
      //     1080p is intentionally absent: 720p x 1.5 is not an integer scale,
      //     and only integer supersampling is guaranteed to change render res. ---
      static const struct { const char* label; const char* scale; } kIRes[] = {
          {"720p (native, 1x)", "1"},
          {"1440p / 2K (2x)", "2"},
          {"2160p / 4K (3x)", "3"},
          {"2880p (4x)", "4"}};
      const std::string cur_scale = rex::cvar::GetFlagByName("resolution_scale");
      int ires_idx = 0;
      for (int i = 0; i < IM_ARRAYSIZE(kIRes); ++i)
        if (cur_scale == kIRes[i].scale) ires_idx = i;
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 6.0f));
      if (ImGui::BeginCombo("Internal Resolution", kIRes[ires_idx].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kIRes); ++i) {
          bool sel = (i == ires_idx);
          if (ImGui::Selectable(kIRes[i].label, sel)) {
            rex::cvar::SetFlagByName("resolution_scale", kIRes[i].scale);
            if (callbacks_.persist_config) callbacks_.persist_config();
          }
          if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::TextColored(ImColor(kInkDim),
                         "(renders the 3D scene at this resolution; applies after restart)");

      ImGui::Spacing();

      // --- Graphics API. "Auto" detects the GPU at boot: AMD -> Vulkan (the
      //     D3D12 path times out / black-screens on AMD for this title), all
      //     others -> Direct3D 12. Override here if needed; applied on restart. ---
      static const struct { const char* label; const char* value; } kApi[] = {
          {"Auto (recommended)", "auto"}, {"Direct3D 12", "d3d12"}, {"Vulkan", "vulkan"}};
      const std::string cur_api = rex::cvar::GetFlagByName("gpu");
      int api_idx = 0;
      for (int i = 0; i < IM_ARRAYSIZE(kApi); ++i)
        if (cur_api == kApi[i].value) api_idx = i;
      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 6.0f));
      if (ImGui::BeginCombo("Graphics API", kApi[api_idx].label)) {
        for (int i = 0; i < IM_ARRAYSIZE(kApi); ++i) {
          bool sel = (i == api_idx);
          if (ImGui::Selectable(kApi[i].label, sel)) {
            rex::cvar::SetFlagByName("gpu", kApi[i].value);
            if (callbacks_.persist_config) callbacks_.persist_config();
          }
          if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
      }
      ImGui::TextColored(ImColor(kInkDim), "(AMD uses Vulkan automatically; applies after restart)");

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      // --- Post-FX (live full-screen filter; see ge_postfx) ---
      ImGui::TextColored(ImColor(kTitle), "POST-FX");
      bool pfx_on = GetCvarB("postfx_enabled");
      if (ImGui::Checkbox("Enable Post-FX", &pfx_on)) {
        SetCvarB("postfx_enabled", pfx_on);
        if (callbacks_.persist_config) callbacks_.persist_config();
      }

      ImGui::SetNextWindowSizeConstraints(
          ImVec2(0, 0), ImVec2(FLT_MAX, ImGui::GetFrameHeightWithSpacing() * 6.0f));
      if (ImGui::BeginCombo("Preset", "Apply preset...")) {
        for (int i = 0; i < ge::PostFxPresetCount(); ++i) {
          if (ImGui::Selectable(ge::PostFxPresetName(i))) ge::ApplyPostFxPreset(i);
        }
        ImGui::EndCombo();
      }

      ImGui::BeginDisabled(!pfx_on);
      {
        float b = GetCvarF("postfx_brightness");
        if (ImGui::SliderFloat("Brightness", &b, -1.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_brightness", b);

        float con = GetCvarF("postfx_contrast");
        if (ImGui::SliderFloat("Contrast", &con, 0.0f, 2.0f, "%.2f"))
          SetCvarF("postfx_contrast", con);

        float sat = GetCvarF("postfx_saturation");
        if (ImGui::SliderFloat("Saturation", &sat, 0.0f, 2.0f, "%.2f"))
          SetCvarF("postfx_saturation", sat);

        float vib = GetCvarF("postfx_vibrance");
        if (ImGui::SliderFloat("Vibrance", &vib, -1.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_vibrance", vib);

        float temp = GetCvarF("postfx_temperature");
        if (ImGui::SliderFloat("Temperature", &temp, -1.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_temperature", temp);

        float gam = GetCvarF("postfx_gamma");
        if (ImGui::SliderFloat("Gamma", &gam, 0.3f, 3.0f, "%.2f")) SetCvarF("postfx_gamma", gam);

        float tint[3] = {GetCvarF("postfx_tint_r"), GetCvarF("postfx_tint_g"),
                         GetCvarF("postfx_tint_b")};
        if (ImGui::ColorEdit3("Tint Colour", tint, ImGuiColorEditFlags_NoInputs)) {
          SetCvarF("postfx_tint_r", tint[0]);
          SetCvarF("postfx_tint_g", tint[1]);
          SetCvarF("postfx_tint_b", tint[2]);
        }
        float ts = GetCvarF("postfx_tint");
        if (ImGui::SliderFloat("Tint Strength", &ts, 0.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_tint", ts);

        float vig = GetCvarF("postfx_vignette");
        if (ImGui::SliderFloat("Vignette", &vig, 0.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_vignette", vig);

        float scan = GetCvarF("postfx_scanlines");
        if (ImGui::SliderFloat("Scanlines", &scan, 0.0f, 1.0f, "%.2f"))
          SetCvarF("postfx_scanlines", scan);
      }
      ImGui::EndDisabled();

      ImGui::Spacing();
      if (ImGui::Button("Save Look")) {
        if (callbacks_.persist_config) callbacks_.persist_config();
      }
      ImGui::SameLine();
      if (ImGui::Button("Reset to Default")) {
        ge::ResetPostFx();
        if (callbacks_.persist_config) callbacks_.persist_config();
      }
      break;
    }
    case 2: {  // CONTROLS
      ImGui::TextColored(ImColor(kTitle), "MOUSE LOOK");
      ImGui::Spacing();

      // Mouse sensitivity -- live (the look hooks read ge_mouse_sens each frame);
      // persisted to ge.toml on release so it survives a restart.
      float sens = GetCvarF("ge_mouse_sens");
      if (ImGui::SliderFloat("Mouse Sensitivity", &sens, 0.05f, 20.0f, "%.2f")) {
        if (sens < 0.05f) sens = 0.05f;
        if (sens > 20.0f) sens = 20.0f;
        SetCvarF("ge_mouse_sens", sens);
      }
      if (ImGui::IsItemDeactivatedAfterEdit() && callbacks_.persist_config)
        callbacks_.persist_config();

      ImGui::Spacing();

      // Mouse-look toggle. ON: the mouse looks (on top of the controller -- both
      // work at once) and the cursor is captured in-game. OFF: controller only,
      // cursor free.
      bool ml = GetCvarB("ge_mouselook_enable");
      if (ImGui::Checkbox("Mouse look", &ml)) {
        SetCvarB("ge_mouselook_enable", ml);
        if (callbacks_.persist_config) callbacks_.persist_config();
      }
      ImGui::TextColored(ImColor(kInkDim),
                         "(controller still works; cursor is captured in-game, freed in this menu)");

      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      ImGui::TextColored(ImColor(kTitle), "REBIND KEYS");
      ImGui::TextColored(ImColor(kInkDim), "(click a binding, then press the new key; Esc cancels)");
      ImGui::Spacing();

      const float btn_w = content_w * 0.34f;
      const float label_x = content_w * 0.44f;
      for (const auto& b : kBinds) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(b.label);
        ImGui::SameLine(label_x);
        const bool capturing = (rebinding_cvar_ == b.cvar);
        std::string cur = capturing ? "press a key..." : rex::cvar::GetFlagByName(b.cvar);
        if (cur.empty()) cur = "(unbound)";
        ImGui::PushID(b.cvar);
        if (ImGui::Button(cur.c_str(), ImVec2(btn_w, 0))) {
          rebinding_cvar_ = b.cvar;
          rebind_skip_ = 1;  // ignore the click that started this rebind
        }
        ImGui::PopID();
      }

      // Capture the next key / mouse button for the pending rebind.
      if (rebinding_cvar_) {
        if (rebind_skip_ > 0) {
          --rebind_skip_;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
          rebinding_cvar_ = nullptr;  // cancel
        } else {
          int vk = 0;
          if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) vk = 0x01;          // LMB
          else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) vk = 0x02;     // RMB
          else if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) vk = 0x04;    // MMB
          else {
            for (ImGuiKey k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END;
                 k = static_cast<ImGuiKey>(k + 1)) {
              if (ImGui::IsKeyPressed(k, false)) {
                vk = ImGuiKeyToVk(k);
                if (vk) break;
              }
            }
          }
          if (vk) {
            std::string name = rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(vk));
            if (!name.empty()) {
              rex::cvar::SetFlagByName(rebinding_cvar_, name);
              if (callbacks_.persist_config) callbacks_.persist_config();
            }
            rebinding_cvar_ = nullptr;
          }
        }
      }
      break;
    }
    case 3: {  // ONLINE
      // Load the cvars into the edit buffers once, the first time the tab opens
      // (so typing doesn't fight a per-frame reload).
      if (!online_loaded_) {
        online_loaded_ = true;
        std::snprintf(username_buf_, sizeof(username_buf_), "%s",
                      rex::cvar::GetFlagByName("ge_username").c_str());
        std::snprintf(server_buf_, sizeof(server_buf_), "%s",
                      rex::cvar::GetFlagByName("ge_online_server").c_str());
        online_enable_ = GetCvarB("ge_online_enable");
        online_port_ = std::atoi(rex::cvar::GetFlagByName("ge_online_port").c_str());
        if (online_port_ <= 0 || online_port_ > 65535) online_port_ = 31000;
      }

      ImGui::TextWrapped(
          "Play with friends over a shared server. One person runs the server "
          "(GoldeneyeServer -- see DEPLOY.md) and shares its address; everyone "
          "enters that same address here.");
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      ImGui::TextColored(ImColor(kInk), "Username");
      ImGui::SetNextItemWidth(content_w * 0.85f);
      ImGui::InputText("##ge_user", username_buf_, sizeof(username_buf_));
      ImGui::TextColored(ImColor(kInkDim), "(shown to other players, max 15 characters)");
      ImGui::Spacing();

      ImGui::TextColored(ImColor(kInk), "Server address");
      ImGui::SetNextItemWidth(content_w * 0.85f);
      ImGui::InputText("##ge_server", server_buf_, sizeof(server_buf_));
      ImGui::TextColored(ImColor(kInkDim),
                         "(IP or hostname -- your own server, or a playit.gg / Hamachi address)");
      ImGui::Spacing();

      ImGui::TextColored(ImColor(kInk), "Server port");
      ImGui::SetNextItemWidth(content_w * 0.85f);
      ImGui::InputInt("##ge_port", &online_port_, 0, 0);  // 0,0 = no +/- step buttons
      ImGui::TextColored(ImColor(kInkDim), "(must match the port the host started the server with)");
      ImGui::Spacing();

      ImGui::Checkbox("Enable online play", &online_enable_);
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      const ImVec2 obsize(content_w * 0.85f, std::floor(fh * 0.07f));
      const bool empty_name = username_buf_[0] == '\0';
      if (empty_name) ImGui::BeginDisabled();
      if (ImGui::Button("SAVE & RESTART", obsize)) {
        const int sp = (online_port_ > 0 && online_port_ <= 65535) ? online_port_ : 31000;
        rex::cvar::SetFlagByName("ge_username", username_buf_);
        rex::cvar::SetFlagByName("ge_online_server",
                                 server_buf_[0] ? server_buf_ : "127.0.0.1");
        rex::cvar::SetFlagByName("ge_online_port", std::to_string(sp));
        SetCvarB("ge_online_enable", online_enable_);
        if (callbacks_.persist_config) callbacks_.persist_config();
        if (callbacks_.request_restart) callbacks_.request_restart();
      }
      if (empty_name) ImGui::EndDisabled();
      ImGui::TextColored(ImColor(kInkDim),
                         "(the game restarts to apply name, server & port changes)");
      break;
    }
    case 4:  // SYSTEM
    default: {
      const ImVec2 bsize(content_w * 0.85f, std::floor(fh * 0.07f));
      if (ImGui::Button("RESUME GAME", bsize)) {
        Close();
      }
      ImGui::Spacing();
      if (ImGui::Button("QUIT TO DESKTOP", bsize)) {
        quit_requested_ = true;
        Close();
      }
      break;
    }
  }

  ImGui::PopItemWidth();
  ImGui::SetWindowFontScale(1.0f);
  ImGui::EndChild();
  ImGui::PopStyleColor();  // ChildBg
  ImGui::PopStyleVar(2);

  // Footer hint.
  ImGui::SetWindowFontScale(1.25f);
  ImGui::SetCursorScreenPos(ImVec2(f0.x + pad, f1.y - std::floor(fh * 0.06f)));
  ImGui::TextColored(ImColor(kInkDim), "Up/Down or click a tab  -  ESC to close");
  ImGui::SetWindowFontScale(1.0f);

  ImGui::PopStyleColor(18);
}
