#include "dock.hpp"
#include "filter.hpp"
#include <obs-frontend-api.h>
#include <obs-module.h>
OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("owcs-ob-analyzer", "en-US")
MODULE_EXPORT const char *obs_module_description() {
  return "Native Overwatch observer scoreboard recognition for any OBS video "
         "source";
}
MODULE_EXPORT const char *obs_module_name() { return "OWCS OB Analyzer"; }
static void event(enum obs_frontend_event event, void *) {
  if (event == OBS_FRONTEND_EVENT_EXIT)
    destroyAnalyzerDock();
}
bool obs_module_load() {
  registerAnalyzerFilter();
  obs_frontend_add_event_callback(event, nullptr);
  blog(LOG_INFO, "[owcs-ob] Native plugin 0.4.0 loaded; no Python runtime");
  return true;
}
MODULE_EXPORT void obs_module_post_load() { createAnalyzerDock(); }
void obs_module_unload() {
  obs_frontend_remove_event_callback(event, nullptr);
  destroyAnalyzerDock();
}
