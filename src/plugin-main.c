#ifdef MPT_WINDOWS_FALLBACK_OBS
#include "mpt-obs-module.h"
#else
#include <obs-module.h>
#endif

#include "motionpngtuber-source.h"
#include "plugin-support.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("MotionPngTuberPlayer", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "MotionPngTuberPlayer native OBS source";
}

bool obs_module_load(void)
{
	obs_register_source(&motionpngtuber_source_info);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
