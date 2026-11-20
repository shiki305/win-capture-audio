#include <obs-module.h>
#include "session-monitor.hpp"
#include "audio-capture-helper-manager.hpp"
#include "common.hpp"
#include "plugin-macros.generated.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("win-capture-audio", "en-GB")

extern struct obs_source_info audio_capture_info;
extern AudioCaptureHelperManager helper_manager;

bool obs_module_load(void)
{
	blog(LOG_INFO, "[win-capture-audio] Version %s (%s)", PLUGIN_VERSION, GIT_HASH);

	SessionMonitor::Create();

	obs_register_source(&audio_capture_info);
	return true;
}

void obs_module_unload()
{
	// OBS destroys sources before unloading modules, but be defensive: stop the
	// monitor first and then any capture thread that is still running, so no
	// helper keeps a pointer to a mixer that no longer exists.
	SessionMonitor::Destroy();
	helper_manager.Shutdown();
}
