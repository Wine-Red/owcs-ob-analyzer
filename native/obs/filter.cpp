#include "filter.hpp"
#include "../runtime/engine.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <graphics/vec4.h>
#include <memory>
#include <obs-module.h>
#include <stdexcept>

namespace {
struct Filter {
  obs_source_t *source = nullptr;
  std::unique_ptr<owcs::Engine> engine;
  gs_texrender_t *texture = nullptr;
  gs_stagesurf_t *surface = nullptr;
  uint32_t width = 0, height = 0;
  std::atomic_int fps{5};
  std::atomic_bool enabled{true};
  qint64 next = 0;
};
const char *name(void *) { return obs_module_text("Filter.Name"); }
void update(void *data, obs_data_t *settings) {
  auto f = static_cast<Filter *>(data);
  owcs::Settings s;
  s.enabled = obs_data_get_bool(settings, "enabled");
  s.autoTab = obs_data_get_bool(settings, "auto_tab");
  s.names = obs_data_get_bool(settings, "recognize_names");
  s.fps = std::clamp(int(obs_data_get_int(settings, "fps")), 1, 10);
  f->fps = s.fps;
  f->enabled = s.enabled;
  f->engine->configure(s);
  if (!s.enabled)
    f->engine->pause();
}
void *create(obs_data_t *settings, obs_source_t *source) {
  try {
    auto f = std::make_unique<Filter>();
    f->source = source;
    char *path = obs_module_file("models");
    if (!path)
      throw std::runtime_error("Plugin models directory not found");
    QString assets = QString::fromUtf8(path);
    bfree(path);
    f->engine =
        std::make_unique<owcs::Engine>(QString::fromUtf8(obs_source_get_uuid(source)), assets);
    update(f.get(), settings);
    return f.release();
  } catch (const std::exception &e) {
    blog(LOG_ERROR, "[owcs-ob] create: %s", e.what());
    return nullptr;
  }
}
void destroy(void *data) {
  auto f = static_cast<Filter *>(data);
  f->engine.reset();
  obs_enter_graphics();
  if (f->surface)
    gs_stagesurface_destroy(f->surface);
  if (f->texture)
    gs_texrender_destroy(f->texture);
  obs_leave_graphics();
  delete f;
}
void defaults(obs_data_t *s) {
  obs_data_set_default_bool(s, "enabled", true);
  obs_data_set_default_bool(s, "auto_tab", false);
  obs_data_set_default_bool(s, "recognize_names", true);
  obs_data_set_default_int(s, "fps", 5);
}
obs_properties_t *properties(void *) {
  auto p = obs_properties_create();
  obs_properties_add_bool(p, "enabled", obs_module_text("Filter.Enabled"));
  obs_properties_add_int_slider(p, "fps", obs_module_text("Filter.Fps"), 1, 10, 1);
  obs_properties_add_bool(p, "recognize_names", obs_module_text("Filter.Names"));
  obs_properties_add_bool(p, "auto_tab", obs_module_text("Filter.AutoTab"));
  obs_properties_add_text(p, "help", obs_module_text("Filter.Help"), OBS_TEXT_INFO);
  return p;
}
void hide(void *data) {
  auto f = static_cast<Filter *>(data);
  f->engine->pause();
}
void render(void *data, gs_effect_t *) {
  auto f = static_cast<Filter *>(data);
  auto target = obs_filter_get_target(f->source);
  if (!target)
    return;
  qint64 now = owcs::monotonicMs();
  uint32_t sw = obs_source_get_base_width(target), sh = obs_source_get_base_height(target);
  if (f->enabled && now >= f->next && sw >= 320 && sh >= 200 && sw <= 16384 && sh <= 16384) {
    f->next = now + 1000 / f->fps.load();
    // Keep small text/icon detail until the worker crops it. Localization still
    // runs at 1600 wide; cap readback so 4K/8K sources cannot grow it unboundedly.
    uint32_t w = std::min(sw, 2560u), h = uint32_t(std::nearbyint(double(sh) * w / sw));
    if (h > 0 && h <= 4096) {
      if (!f->texture)
        f->texture = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
      if (f->width != w || f->height != h) {
        if (f->surface)
          gs_stagesurface_destroy(f->surface);
        f->surface = gs_stagesurface_create(w, h, GS_BGRA);
        f->width = w;
        f->height = h;
      }
      gs_texrender_reset(f->texture);
      if (f->surface && gs_texrender_begin(f->texture, w, h)) {
        gs_blend_state_push();
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
        vec4 clear{};
        gs_clear(GS_CLEAR_COLOR, &clear, 0, 0);
        gs_ortho(0, float(sw), 0, float(sh), -100, 100);
        obs_source_skip_video_filter(f->source);
        gs_blend_state_pop();
        gs_texrender_end(f->texture);
        gs_stage_texture(f->surface, gs_texrender_get_texture(f->texture));
        uint8_t *bytes = nullptr;
        uint32_t stride = 0;
        if (gs_stagesurface_map(f->surface, &bytes, &stride)) {
          cv::Mat copy;
          try {
            copy = cv::Mat(int(h), int(w), CV_8UC4, bytes, stride).clone();
          } catch (const std::exception &e) {
            blog(LOG_WARNING, "[owcs-ob] frame copy: %s", e.what());
          }
          gs_stagesurface_unmap(f->surface);
          if (!copy.empty()) {
            auto parent = obs_filter_get_parent(f->source);
            f->engine->submit(std::move(copy),
                              QString::fromUtf8(obs_source_get_name(parent ? parent : target)),
                              now);
          }
        }
      }
    }
  }
  obs_source_skip_video_filter(f->source);
}
} // namespace
void registerAnalyzerFilter() {
  obs_source_info info{};
  info.id = "owcs_ob_analyzer_filter";
  info.type = OBS_SOURCE_TYPE_FILTER;
  info.output_flags = OBS_SOURCE_VIDEO;
  info.get_name = name;
  info.create = create;
  info.destroy = destroy;
  info.update = update;
  info.get_defaults = defaults;
  info.get_properties = properties;
  info.video_render = render;
  info.hide = hide;
  info.deactivate = hide;
  obs_register_source(&info);
}
