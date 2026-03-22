#pragma once

#ifdef MPT_WINDOWS_FALLBACK_OBS
#include "mpt-obs-module.h"
#else
#include <obs-module.h>
#endif

extern struct obs_source_info motionpngtuber_source_info;
