#ifdef MPT_FALLBACK_OBS
#include "mpt-obs-module.h"
#else
#include <obs-module.h>
#endif

#include "mpt-text.h"

#include <cstring>

namespace {

struct TextEntry {
	const char *key;
	const char *en;
	const char *ja;
};

constexpr TextEntry kTextEntries[] = {
	{"MotionPngTuberPlayer.SourceName", "MotionPngTuberPlayer",
	 u8"MotionPNG\u30C1\u30E5\u30FC\u30D0\u30FC\u30D7\u30EC\u30A4\u30E4\u30FC"},
	{"MotionPngTuberPlayer.LoopVideo", "Mouthless Video", u8"\u53E3\u306A\u3057\u52D5\u753B"},
	{"MotionPngTuberPlayer.MouthDir", "Mouth Directory", u8"\u53E3\u753B\u50CF\u30D5\u30A9\u30EB\u30C0\u30FC"},
	{"MotionPngTuberPlayer.TrackFile", "Track File (.json or .npz)",
	 u8"\u53E3\u30D1\u30AF\u30C8\u30E9\u30C3\u30AF (.json / .npz)"},
	{"MotionPngTuberPlayer.TrackCalibratedFile", "Track Calibrated File (.json or .npz)",
	 u8"\u88DC\u6B63\u6E08\u307F\u53E3\u30D1\u30AF\u30C8\u30E9\u30C3\u30AF (.json / .npz)"},
	{"MotionPngTuberPlayer.RenderFps", "Render FPS", u8"\u63CF\u753B\u30D5\u30EC\u30FC\u30E0\u30EC\u30FC\u30C8"},
	{"MotionPngTuberPlayer.StatusConfigureSource",
	 "Configure the video, mouth directory, and track file to start MotionPngTuberPlayer.",
	 u8"\u53E3\u306A\u3057\u52D5\u753B\u30FB\u53E3\u753B\u50CF\u30D5\u30A9\u30EB\u30C0\u30FC\u30FB\u53E3\u30D1\u30AF\u30C8\u30E9\u30C3\u30AF\u3092\u8A2D\u5B9A\u3057\u3066\u304F\u3060\u3055\u3044\u3002"},
	{"MotionPngTuberPlayer.StatusWorkerStarting", "Native runtime is starting.",
	 u8"\u30CD\u30A4\u30C6\u30A3\u30D6\u518D\u751F\u30A8\u30F3\u30B8\u30F3\u3092\u8D77\u52D5\u3057\u3066\u3044\u307E\u3059\u3002"},
	{"MotionPngTuberPlayer.StatusRunning", "Running", u8"\u518D\u751F\u4E2D"},
	{"MotionPngTuberPlayer.StatusWaitingFrame", "Native runtime is running and waiting for the first frame.",
	 u8"\u30CD\u30A4\u30C6\u30A3\u30D6\u518D\u751F\u30A8\u30F3\u30B8\u30F3\u306F\u8D77\u52D5\u3057\u307E\u3057\u305F\u3002\u6700\u521D\u306E\u30D5\u30EC\u30FC\u30E0\u3092\u5F85\u3063\u3066\u3044\u307E\u3059\u3002"},
	{"MotionPngTuberPlayer.AudioDevice", "Audio Device", u8"\u53E3\u30D1\u30AF\u7528\u30DE\u30A4\u30AF"},
	{"MotionPngTuberPlayer.AudioDeviceAuto", "Auto (first available input)",
	 u8"\u81EA\u52D5 (\u6700\u521D\u306B\u898B\u3064\u304B\u3063\u305F\u5165\u529B\u30C7\u30D0\u30A4\u30B9)"},
	{"MotionPngTuberPlayer.AudioDeviceIndex", "Audio Device Fallback Index (-1 = Auto)",
	 u8"\u53E3\u30D1\u30AF\u7528\u30DE\u30A4\u30AF\u756A\u53F7\uFF08\u4E88\u5099\u3001-1 \u3067\u81EA\u52D5\uFF09"},
	{"MotionPngTuberPlayer.AudioDeviceNone", "No input devices were detected.",
	 u8"\u4F7F\u3048\u308B\u30DE\u30A4\u30AF\u304C\u898B\u3064\u304B\u308A\u307E\u305B\u3093\u3067\u3057\u305F\u3002"},
	{"MotionPngTuberPlayer.AudioDeviceUnnamed", "Unnamed input device",
	 u8"\u540D\u524D\u306E\u306A\u3044\u5165\u529B\u30C7\u30D0\u30A4\u30B9"},
	{"MotionPngTuberPlayer.ValidPolicy", "Valid Policy", u8"\u30C8\u30E9\u30C3\u30AD\u30F3\u30B0\u6B20\u640D\u6642\u306E\u51E6\u7406"},
	{"MotionPngTuberPlayer.Hold", "Hold", u8"\u524D\u306E\u53E3\u4F4D\u7F6E\u3092\u4F7F\u3046"},
	{"MotionPngTuberPlayer.Strict", "Strict", u8"\u305D\u306E\u30D5\u30EC\u30FC\u30E0\u306F\u63CF\u753B\u3057\u306A\u3044"},
};

bool is_japanese_locale(const char *locale)
{
	return locale && (locale[0] == 'j' || locale[0] == 'J') && (locale[1] == 'a' || locale[1] == 'A') &&
	       (locale[2] == '\0' || locale[2] == '-' || locale[2] == '_');
}

const TextEntry *find_text_entry(const char *key)
{
	for (const auto &entry : kTextEntries) {
		if (std::strcmp(entry.key, key) == 0)
			return &entry;
	}

	return nullptr;
}

} // namespace

extern "C" const char *mpt_text(const char *key)
{
	if (!key)
		return "";

	const char *translated = obs_module_text(key);
	if (translated && std::strcmp(translated, key) != 0)
		return translated;

	const TextEntry *fallback = find_text_entry(key);
	if (!fallback)
		return translated ? translated : key;

	return is_japanese_locale(obs_get_locale()) ? fallback->ja : fallback->en;
}
