// Render page (docs/GUIDE.md "UI"; docs/ui-mockups/04-render): pick a scene, set it up, start.

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <cstdlib>
#include <fstream>
#include <spdlog/spdlog.h>

#include "apps/penumbra/ui/file_dialog.h"
#include "apps/penumbra/ui/pages.h"
#include "apps/penumbra/ui/theme.h"
#include "apps/penumbra/ui/widgets.h"
#include "core/film/film.h"
#include "core/output/writers.h"
#include "core/settings/presets.h"
#include "core/settings/schema.h"
#include "core/system/file.h"
#include "core/system/paths.h"
#include "core/system/sysinfo.h"
#include "worker/device_manager.h"

namespace pen::ui {

namespace {

constexpr size_t kMaxRecent = 8;

std::filesystem::path recentFile() { return paths::userDataDir() / "recent_scenes.txt"; }

void saveRecent(const std::vector<std::string>& recent) {
  std::error_code ec;
  std::filesystem::create_directories(recentFile().parent_path(), ec);
  std::ofstream out(recentFile(), std::ios::binary);
  for (const auto& r : recent) out << r << '\n';
}

void copyTo(char* buf, size_t size, const std::string& s) { std::snprintf(buf, size, "%s", s.c_str()); }

void openScene(UiState& ui, const std::filesystem::path& path) {
  RenderPage& rp = ui.render;
  copyTo(rp.scene_path, sizeof(rp.scene_path), pathToUtf8(path));
  rp.loading = std::async(std::launch::async, inspectScene, path);
  rp.start_error.clear();
  rp.from_command_line = false;
  const std::string p = pathToUtf8(std::filesystem::absolute(path));
  std::erase(rp.recent, p);
  rp.recent.insert(rp.recent.begin(), p);
  if (rp.recent.size() > kMaxRecent) rp.recent.resize(kMaxRecent);
  saveRecent(rp.recent);
}

int presetIndex(const RenderSettings& s) {
  const auto presets = resolutionPresets();
  for (size_t i = 0; i < presets.size(); ++i)
    if (presets[i].name == s.res.preset) return int(i);
  return -1;
}

// The scene finished loading: the form takes its resolved settings (config file, then .pen.json).
void sceneLoaded(UiState& ui) {
  RenderPage& rp = ui.render;
  rp.info = std::make_shared<SceneInfo>(rp.loading.get());
  if (!rp.info->error.empty()) return;
  // A scene from the command line keeps the command line's settings on top of its .pen.json (already resolved).
  RenderSettings form = rp.from_command_line ? ui.settings : rp.info->settings;
  rp.from_command_line = false;
  rp.form = form;
  copyTo(rp.output, sizeof(rp.output), pathToUtf8(form.output));
  rp.preset = presetIndex(form);
  rp.custom_w = int(form.res.width);
  rp.custom_h = int(form.res.height);
  rp.preview_wanted = true;
  rp.changed = std::chrono::steady_clock::now();
  ui.settings = form;  // the Dashboard's disk card follows the output folder
}

// The framing preview: a small render on this machine's CPU, a moment after the framing last changed.
void updatePreview(RenderPage& rp) {
  if (rp.preview.valid() && rp.preview.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
    const std::vector<uint8_t> px = rp.preview.get();
    if (!px.empty()) rp.preview_tex.set(px.data(), int(rp.preview_w), int(rp.preview_h));
  }
  if (!rp.preview_wanted || rp.preview.valid() || !rp.info || !rp.info->scene) return;
  if (std::chrono::steady_clock::now() - rp.changed < std::chrono::milliseconds(300)) return;
  rp.preview_wanted = false;
  const double aspect = double(rp.form.outputWidth()) / double(std::max(1u, rp.form.outputHeight()));
  rp.preview_w = aspect >= 16.0 / 9.0 ? 480u : std::max(16u, uint32_t(270 * aspect));
  rp.preview_h = aspect >= 16.0 / 9.0 ? std::max(16u, uint32_t(480 / aspect)) : 270u;
  rp.preview = std::async(std::launch::async, [scene = rp.info->scene, s = rp.form, w = rp.preview_w,
                                               h = rp.preview_h] { return renderPreview(*scene, s, w, h, 8); });
}

struct FormatChoice {
  ImageFormat format;
  uint32_t bits;
  const char* label;
  const char* ext;
};
constexpr FormatChoice kFormats[] = {
    {ImageFormat::Png, 8, "PNG \xC2\xB7 8-bit", ".png"},      {ImageFormat::Png, 16, "PNG \xC2\xB7 16-bit", ".png"},
    {ImageFormat::Exr, 16, "EXR \xC2\xB7 16-bit half", ".exr"}, {ImageFormat::Exr, 32, "EXR \xC2\xB7 32-bit float", ".exr"},
    {ImageFormat::Tiff, 16, "TIFF \xC2\xB7 16-bit", ".tiff"},  {ImageFormat::Jpeg, 8, "JPEG \xC2\xB7 8-bit", ".jpg"},
    {ImageFormat::Webp, 8, "WebP \xC2\xB7 8-bit", ".webp"},
};

int formatIndex(const RenderSettings& s) {
  for (int i = 0; i < int(std::size(kFormats)); ++i)
    if (s.format && *s.format == kFormats[i].format && (s.bit_depth == kFormats[i].bits || s.bit_depth == 0))
      return i;
  return 0;
}

template <class E>
bool enumCombo(const char* label, E& value, std::initializer_list<std::pair<E, const char*>> items) {
  const char* current = "?";
  for (const auto& [v, name] : items)
    if (v == value) current = name;
  bool changed = false;
  if (ImGui::BeginCombo(label, current)) {
    for (const auto& [v, name] : items)
      if (ImGui::Selectable(name, v == value)) {
        value = v;
        changed = true;
      }
    ImGui::EndCombo();
  }
  return changed;
}

void fieldLabel(const char* text) { small("%s", text); }

void sceneCard(UiState& ui, float w, float h) {
  RenderPage& rp = ui.render;
  beginCard("scene", w, h);
  cardLabel("Scene");
  const std::vector<FileFilter> filters = {{"Scenes", {"glb", "gltf", "obj", "json"}}};
  if (primaryButton("Open\xE2\x80\xA6"))
    if (auto p = openFileDialog("Open a scene", filters)) openScene(ui, *p);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - px(90));
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(6));
  const bool enter =
      ImGui::InputTextWithHint("##scene", ".glb, .gltf, .obj or .pen.json", rp.scene_path, sizeof(rp.scene_path),
                               ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  if ((secondaryButton("Load") || enter) && rp.scene_path[0]) openScene(ui, pathFromUtf8(rp.scene_path));
  if (!rp.recent.empty()) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::BeginCombo("##recent", "Recent scenes")) {
      for (const std::string& r : rp.recent)
        if (ImGui::Selectable(r.c_str())) openScene(ui, pathFromUtf8(r));
      ImGui::EndCombo();
    }
  }
  ImGui::Dummy(ImVec2(0, px(4)));
  if (rp.loading.valid()) {
    secondary("Loading and checking the scene\xE2\x80\xA6");
  } else if (rp.info && !rp.info->error.empty()) {
    ImGui::PushTextWrapPos(0);
    text(fonts().sans, size::kBody, color::kDanger, "%s", rp.info->error.c_str());
    ImGui::PopTextWrapPos();
  } else if (rp.info) {
    const SceneInfo& in = *rp.info;
    const auto stat = [](const char* value, const char* label) {
      ImGui::BeginGroup();
      statNumber("%s", value);
      small("%s", label);
      ImGui::EndGroup();
      ImGui::SameLine(0, px(28));
    };
    const std::string tris = in.stats.triangles >= 1000000 ? fmt::format("{:.1f} M", in.stats.triangles / 1e6)
                                                           : fmt::format("{}", in.stats.triangles);
    stat(tris.c_str(), "Triangles");
    stat(std::to_string(in.stats.instances).c_str(), "Instances");
    stat(std::to_string(in.lights).c_str(), "Lights");
    stat(formatBytesShort(in.stats.texture_bytes).c_str(), "Textures");
    stat(formatBytesShort(in.package_bytes).c_str(), "Package");
    ImGui::NewLine();
    text(fonts().sans_medium, size::kBody, color::kOk, "OK \xC2\xB7 all %zu files found", in.package_files);
    for (size_t i = 0; i < std::min<size_t>(in.warnings.size(), 3); ++i) small("%s", in.warnings[i].c_str());
  } else {
    secondary("No scene yet. Open a .glb from Blender, or a .pen.json.");
  }
  endCard();
}

void cameraCard(UiState& ui, float w, float h) {
  RenderPage& rp = ui.render;
  beginCard("camera", w, h);
  cardLabel("Camera");
  if (rp.info && rp.info->scene && !rp.info->cameras.empty()) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    const std::string current = rp.form.camera.empty() ? rp.info->cameras.front() : rp.form.camera;
    if (ImGui::BeginCombo("##camera", current.c_str())) {
      for (size_t i = 0; i < rp.info->cameras.size(); ++i)
        if (ImGui::Selectable(rp.info->cameras[i].c_str(), rp.info->cameras[i] == current)) {
          rp.form.camera = i == 0 ? "" : rp.info->cameras[i];
          rp.preview_wanted = true;
          rp.changed = std::chrono::steady_clock::now();
        }
      ImGui::EndCombo();
    }
  }
  updatePreview(rp);
  const ImVec2 avail = ImGui::GetContentRegionAvail();
  if (rp.preview_tex.valid()) {
    const float aspect = float(rp.preview_tex.width()) / float(rp.preview_tex.height());
    float iw = avail.x, ih = iw / aspect;
    if (ih > avail.y - px(24)) {
      ih = avail.y - px(24);
      iw = ih * aspect;
    }
    ImGui::Image(rp.preview_tex.ref(), ImVec2(iw, ih));
    small(rp.preview.valid() ? "Refreshing the preview\xE2\x80\xA6" : "Framing preview \xC2\xB7 low resolution, rendered on this machine");
  } else {
    secondary(rp.preview.valid() ? "Rendering the framing preview\xE2\x80\xA6"
                                 : "A low-resolution framing preview appears here once a scene is loaded.");
  }
  endCard();
}

void imageCard(UiState& ui, float w, float h) {
  RenderPage& rp = ui.render;
  RenderSettings& f = rp.form;
  beginCard("image", w, h);
  cardLabel("Image");
  const float fieldW = ImGui::GetContentRegionAvail().x;
  const auto presets = resolutionPresets();
  fieldLabel("Resolution");
  ImGui::SetNextItemWidth(fieldW);
  const std::string current =
      rp.preset >= 0 ? fmt::format("{} \xC2\xB7 {} \xC3\x97 {}", presets[rp.preset].name, presets[rp.preset].width,
                                   presets[rp.preset].height)
                     : std::string("Custom\xE2\x80\xA6");
  if (ImGui::BeginCombo("##res", current.c_str())) {
    for (int i = int(presets.size()) - 1; i >= 0; --i) {
      const std::string label = fmt::format("{} \xC2\xB7 {} \xC3\x97 {}", presets[i].name, presets[i].width, presets[i].height);
      if (ImGui::Selectable(label.c_str(), rp.preset == i)) {
        rp.preset = i;
        f.res = {presets[i].width, presets[i].height, std::string(presets[i].name)};
        rp.preview_wanted = true;
        rp.changed = std::chrono::steady_clock::now();
      }
    }
    if (ImGui::Selectable("Custom\xE2\x80\xA6", rp.preset < 0)) rp.preset = -1;
    ImGui::EndCombo();
  }
  if (rp.preset < 0) {
    ImGui::SetNextItemWidth(fieldW / 2 - px(6));
    bool changed = ImGui::InputInt("##w", &rp.custom_w, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(fieldW / 2 - px(6));
    changed |= ImGui::InputInt("##h", &rp.custom_h, 0);
    rp.custom_w = std::clamp(rp.custom_w, int(kMinImageSide), int(kMaxImageSide));
    rp.custom_h = std::clamp(rp.custom_h, int(kMinImageSide), int(kMaxImageSide));
    if (changed) {
      f.res = {uint32_t(rp.custom_w), uint32_t(rp.custom_h), ""};
      rp.preview_wanted = true;
      rp.changed = std::chrono::steady_clock::now();
    }
  }
  const float half = fieldW / 2 - px(6);
  ImGui::BeginGroup();
  fieldLabel("Samples per pixel");
  ImGui::SetNextItemWidth(half);
  ImGui::InputScalar("##spp", ImGuiDataType_U32, &f.spp);
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  fieldLabel("Max bounces");
  ImGui::SetNextItemWidth(half);
  ImGui::InputScalar("##bounces", ImGuiDataType_U32, &f.max_bounces);
  f.max_bounces = std::min(f.max_bounces, 1024u);
  ImGui::EndGroup();

  ImGui::BeginGroup();
  fieldLabel("Integrator");
  ImGui::SetNextItemWidth(half);
  enumCombo("##integrator", f.integrator,
            {{Integrator::Path, "Path tracing"}, {Integrator::Ao, "Ambient occlusion"},
             {Integrator::DebugNormal, "Debug: normals"}, {Integrator::DebugAlbedo, "Debug: albedo"},
             {Integrator::DebugDepth, "Debug: depth"}});
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  fieldLabel("Backend");
  ImGui::SetNextItemWidth(half);
  enumCombo("##backend", f.backend,
            {{BackendKind::Auto, "Auto (per GPU)"}, {BackendKind::Vulkan, "Vulkan RT"}, {BackendKind::Cuda, "CUDA"},
             {BackendKind::Cpu, "CPU"}});
  ImGui::EndGroup();

  fieldLabel("Seed");
  ImGui::SetNextItemWidth(half);
  ImGui::InputScalar("##seed", ImGuiDataType_U32, &f.seed);
  if (ImGui::CollapsingHeader("Advanced: per-type bounce caps, tile size, time limit")) {
    const auto cap = [&](const char* label, std::optional<uint32_t>& v) {
      uint32_t x = v.value_or(f.max_bounces);
      ImGui::SetNextItemWidth(half);
      if (ImGui::InputScalar(label, ImGuiDataType_U32, &x)) v = std::min(x, 1024u);
    };
    cap("Diffuse bounces", f.max_diffuse);
    cap("Glossy bounces", f.max_glossy);
    cap("Transmission bounces", f.max_transmission);
    ImGui::SetNextItemWidth(half);
    ImGui::InputScalar("Tile size (px)", ImGuiDataType_U32, &f.tile_size);
    f.tile_size = std::clamp(f.tile_size, 16u, 4096u);
    uint32_t minutes = uint32_t(f.time_limit_s / 60);
    ImGui::SetNextItemWidth(half);
    if (ImGui::InputScalar("Time limit (minutes, 0 = none)", ImGuiDataType_U32, &minutes)) f.time_limit_s = uint64_t(minutes) * 60;
  }
  endCard();
}

void outputCard(UiState& ui, float w, float h) {
  RenderPage& rp = ui.render;
  RenderSettings& f = rp.form;
  beginCard("output", w, h);
  cardLabel("Output");
  const float fieldW = ImGui::GetContentRegionAvail().x;
  fieldLabel("File");
  ImGui::SetNextItemWidth(fieldW - px(110));
  if (ImGui::InputText("##out", rp.output, sizeof(rp.output))) f.output = pathFromUtf8(rp.output);
  ImGui::SameLine();
  if (secondaryButton("Browse\xE2\x80\xA6")) {
    const FormatChoice& fc = kFormats[formatIndex(f)];
    if (auto p = saveFileDialog("Save the image as", f.output, {{fc.label, {std::string(fc.ext + 1)}}})) {
      f.output = *p;
      copyTo(rp.output, sizeof(rp.output), pathToUtf8(f.output));
    }
  }
  fieldLabel("Format");
  ImGui::SetNextItemWidth(fieldW);
  int fi = formatIndex(f);
  if (ImGui::BeginCombo("##format", kFormats[fi].label)) {
    for (int i = 0; i < int(std::size(kFormats)); ++i)
      if (ImGui::Selectable(kFormats[i].label, i == fi)) {
        f.output.replace_extension(kFormats[i].ext);
        f.format = kFormats[i].format;
        f.bit_depth = kFormats[i].bits;
        copyTo(rp.output, sizeof(rp.output), pathToUtf8(f.output));
      }
    ImGui::EndCombo();
  }
  const float half = fieldW / 2 - px(6);
  ImGui::BeginGroup();
  fieldLabel("Denoise");
  ImGui::SetNextItemWidth(half);
  ImGui::BeginDisabled();
  int off = 0;
  ImGui::Combo("##denoise", &off, "Off\0");
  ImGui::EndDisabled();
  ImGui::EndGroup();
  ImGui::SameLine();
  ImGui::BeginGroup();
  fieldLabel("Tonemap");
  ImGui::SetNextItemWidth(half);
  ImGui::BeginDisabled();
  int srgb = 0;
  ImGui::Combo("##tonemap", &srgb, "sRGB curve\0");
  ImGui::EndDisabled();
  ImGui::EndGroup();
  small("Denoising (OIDN), AgX/Filmic/ACES and extra passes arrive in milestone M11.");
  fieldLabel("Exposure (EV)");
  ImGui::SetNextItemWidth(fieldW);
  float ev = float(f.exposure);
  if (ImGui::SliderFloat("##ev", &ev, -5.0f, 5.0f, "%+.1f")) {
    f.exposure = ev;
    rp.preview_wanted = true;
    rp.changed = std::chrono::steady_clock::now();
  }
  endCard();
}

}  // namespace

void initRenderPage(UiState& ui) {
  RenderPage& rp = ui.render;
  std::ifstream in(recentFile(), std::ios::binary);
  for (std::string line; std::getline(in, line) && rp.recent.size() < kMaxRecent;)
    if (!line.empty()) rp.recent.push_back(line);
  rp.form = ui.settings;
  copyTo(rp.output, sizeof(rp.output), pathToUtf8(rp.form.output));
  rp.preset = presetIndex(rp.form);
  const std::filesystem::path given =
      !ui.settings.scene_description.empty() ? ui.settings.scene_description : ui.settings.scene;
  if (!given.empty()) {
    openScene(ui, given);
    rp.from_command_line = true;
  }
}

void drawRender(Model& model, UiState& ui) {
  RenderPage& rp = ui.render;
  if (rp.loading.valid() && rp.loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) sceneLoaded(ui);
  pageHeader("Render", "Pick a scene, set it up, start. Workers download the scene, then join in.");

  const float gap = px(16);
  const float full = ImGui::GetContentRegionAvail().x;
  const float left = std::floor((full - gap) * 0.55f), right = full - gap - left;
  sceneCard(ui, left, px(300));
  ImGui::SameLine(0, gap);
  cameraCard(ui, right, px(300));
  ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));
  const float half = (full - gap) / 2;
  imageCard(ui, half, px(430));
  ImGui::SameLine(0, gap);
  outputCard(ui, half, px(430));
  ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y));

  // Estimates and Start (docs/GUIDE.md "Live estimates"; "Start render: disabled with a visible reason").
  RenderSettings s = rp.form;
  s.output = pathFromUtf8(rp.output);
  std::string why;
  try {
    // The format follows the file's extension (as on the command line); the bit depth chosen in the Format list
    // stays when it belongs to that format.
    const FormatChoice& chosen = kFormats[formatIndex(rp.form)];
    std::string ext = pathToUtf8(s.output.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const bool same = ext == chosen.ext || (ext == ".jpeg" && chosen.format == ImageFormat::Jpeg) ||
                      (ext == ".tif" && chosen.format == ImageFormat::Tiff);
    s.format.reset();
    s.bit_depth = same ? chosen.bits : 0;
    finalizeSettings(s);
  } catch (const std::exception& e) {
    why = e.what();
  }
  const uint32_t w = s.outputWidth(), h = s.outputHeight();
  const FilmLayout layout = FilmLayout::make(w, h, filmTileSize(w, h, s.tile_size));
  const uint64_t filmBytes = uint64_t(w) * h * 16;
  const uint64_t ramBudget = s.ram_limit ? s.ram_limit : querySystem().ram_total / 4 * 3;
  const bool inRam = uint64_t(w) * h <= uint64_t(40960) * 23040 && filmBytes <= ramBudget / 2;
  const uint64_t need = Film::fileBytes(layout) * (inRam ? 2 : 1) +
                        (s.format ? estimateOutputBytes(*s.format, s.bit_depth, w, h) : 0);
  const std::filesystem::path outDir = std::filesystem::absolute(s.output).parent_path();
  const uint64_t freeBytes = freeDiskBytes(outDir);
  double speed = 0;
  for (float v : model.throughput().values()) speed = std::max(speed, double(v));
  const double samples = double(w) * h * s.spp;

  beginCard("estimates", full, 0, 24);
  const auto est = [&](const std::string& value, const std::string& label) {
    ImGui::BeginGroup();
    statNumber("%s", value.c_str());
    small("%s", label.c_str());
    ImGui::EndGroup();
    ImGui::SameLine(0, px(48));
  };
  est(formatBytesShort(filmBytes), inRam ? "Film (in RAM)" : "Film (on disk)");
  est(fmt::format("{} / {}", formatBytesShort(need), formatBytesShort(freeBytes)), "Disk needed / free");
  est(speed > 0 && s.spp ? "\xE2\x89\x88 " + formatDuration(samples / speed) : std::string("\xE2\x80\x94"),
      speed > 0 ? "At " + formatRate(speed) + " (best measured in the last 10 minutes)"
                : std::string("No speed measured yet in this session"));

  if (why.empty()) {
    if (!rp.info) why = "Open a scene first";
    else if (rp.loading.valid()) why = "The scene is still loading";
    else if (!rp.info->error.empty()) why = "The scene has a problem (see above)";
    else if (ui.runner && ui.runner->busy()) why = "A render is already running";
    else if (need > freeBytes) why = fmt::format("Not enough disk in {}", pathToUtf8(outDir));
    else if (localDevices(ui).empty() && model.hostsOnline() <= 1) why = "This machine's devices are off and no worker is connected";
  }
  ImGui::NewLine();
  // Test only (D-047 style): PENUMBRA_TEST_UI_START presses Start once the scene is ready, for viewport checks.
  static const bool autoStart = [] {
    const char* v = std::getenv("PENUMBRA_TEST_UI_START");
    if (v && *v) spdlog::warn("TEST HOOK ACTIVE (PENUMBRA_TEST_UI_START): starting the render when the scene is ready");
    return v && *v;
  }();
  static bool autoStarted = false;
  const bool autoGo = autoStart && !autoStarted && why.empty();
  if (autoGo) autoStarted = true;
  if (primaryButton("Start render", why.empty()) || autoGo) {
    s.scene = rp.info->settings.scene;
    s.scene_description = rp.info->settings.scene_description;
    // A rendering CPU leaves the Settings page's threads free for the UI (unless --threads chose).
    if (s.threads == 0)
      s.threads = uint32_t(std::max(1, int(usableCpuThreads()) - ui.cpu_reserve));
    if (ui.runner->start(s, localDevices(ui), rp.start_error)) ui.page = Page::Viewport;
  }
  ImGui::SameLine(0, px(16));
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + px(12));
  if (!why.empty()) text(fonts().sans, size::kBody, color::kAccent, "%s", why.c_str());
  else secondary("Local devices start at once. Each worker downloads the %s scene, builds its BVH, then joins.",
                 rp.info ? formatBytesShort(rp.info->package_bytes).c_str() : "");
  if (!rp.start_error.empty()) text(fonts().sans, size::kBody, color::kDanger, "%s", rp.start_error.c_str());
  endCard();
}

}  // namespace pen::ui
