#pragma once

#include <unordered_map>
#include <tuple>
#include <set>

#include <windows.h>

#include <wil/resource.h>

#include "common.hpp"
#include "audio-capture-helper.hpp"
#include "format-conversion.hpp"

class AudioCaptureHelperManager {
private:
	wil::critical_section helpers_section;
	std::unordered_map<DWORD, AudioCaptureHelper> helpers;

	WAVEFORMATEX format = {};
	bool format_ready = false;

	WAVEFORMATEX MakeFormat()
	{
		obs_audio_info info = {};
		obs_get_audio_info(&info);

		WAVEFORMATEX fmt = {};

		fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
		fmt.nChannels = static_cast<WORD>(get_audio_channels(info.speakers));
		fmt.nSamplesPerSec = info.samples_per_sec;

		fmt.nBlockAlign = fmt.nChannels * sizeof(float);
		fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
		fmt.wBitsPerSample = CHAR_BIT * sizeof(float);
		fmt.cbSize = 0;

		return fmt;
	}

public:
	AudioCaptureHelperManager() = default;

	~AudioCaptureHelperManager() { Shutdown(); }

	WAVEFORMATEX GetFormat()
	{
		if (!format_ready) {
			format = MakeFormat();
			format_ready = true;
		}

		return format;
	}

	void RegisterMixer(DWORD pid, Mixer *mixer)
	{
		auto lock = helpers_section.lock();

		try {
			auto [it, inserted] = helpers.try_emplace(pid, mixer, GetFormat(), pid);
			if (!inserted)
				it->second.RegisterMixer(mixer);
		} catch (const wil::ResultException &e) {
			error("failed to create helper... update Windows?");
			error("%s", e.what());
		} catch (const std::exception &e) {
			error("failed to create helper... update Windows?");
			error("%s", e.what());
		}
	};

	void UnRegisterMixer(DWORD pid, Mixer *mixer)
	{
		auto lock = helpers_section.lock();

		auto it = helpers.find(pid);
		if (it == helpers.end())
			return;

		auto remove_helper = it->second.UnRegisterMixer(mixer);
		if (remove_helper)
			helpers.erase(it);
	};

	// Stops every capture thread. Called on module unload so no helper outlives
	// the mixers it points at.
	void Shutdown()
	{
		std::unordered_map<DWORD, AudioCaptureHelper> local;

		{
			auto lock = helpers_section.lock();
			local.swap(helpers);
		}

		// Destroyed here, outside the lock, which joins each capture thread.
	}
};
