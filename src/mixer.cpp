#include "mixer.hpp"
#include "format-conversion.hpp"
#include "wil/result_macros.h"
#include <algorithm>
#include <basetsd.h>
#include <memory>
#include <mutex>
#include <processthreadsapi.h>
#include <threadpoollegacyapiset.h>
#include <winbase.h>
#include <winuser.h>

template<typename T> T Mixer::RoundToNearest(T x, T m)
{
	return ((x + m / 2) / m) * m;
}

UINT64 Mixer::GetCurrentTimestamp()
{
	static std::once_flag frequency_once;
	static LARGE_INTEGER frequency = {0};

	std::call_once(frequency_once, [] { QueryPerformanceFrequency(&frequency); });

	LARGE_INTEGER timestamp_ticks;
	QueryPerformanceCounter(&timestamp_ticks);

	if (frequency.QuadPart <= 0)
		return 0;

	// Convert to 100 ns units without overflowing: (ticks / f) * 1e7 + rest.
	UINT64 ticks = static_cast<UINT64>(timestamp_ticks.QuadPart);
	UINT64 freq = static_cast<UINT64>(frequency.QuadPart);

	return (ticks / freq) * 10000000ULL + ((ticks % freq) * 10000000ULL) / freq;
}

std::size_t Mixer::DurationToFrames(UINT64 duration)
{
	UINT64 duration_ns = duration * 100;
	return (duration_ns * format.nSamplesPerSec) / 1000000000;
}

UINT64 Mixer::FramesToDuration(std::size_t frames)
{
	UINT64 duration_ns = (frames * 1000000000) / format.nSamplesPerSec;
	return duration_ns / 100;
}

void Mixer::ProcessInput(UINT64 input_timestamp, std::vector<float> &input_buffer)
{
	if (input_buffer.empty())
		return;

	if (mix.size() == 0) {
		mix_timestamp = input_timestamp;
		mix = std::move(input_buffer);

		return;
	}

	if (input_timestamp < mix_timestamp) {
		debug("late mix input packet, dropping");
		return;
	}

	auto offset = format.nChannels * DurationToFrames(input_timestamp - mix_timestamp);

	// A large gap would otherwise allocate an unbounded amount of memory.
	const std::size_t max_offset_frames =
		static_cast<std::size_t>(format.nSamplesPerSec) * 10; // 10 seconds
	if (offset > max_offset_frames * format.nChannels) {
		debug("input timestamp gap too large, resetting mix buffer");
		mix.clear();
		mix_timestamp = input_timestamp;
		mix = std::move(input_buffer);
		return;
	}

	if (offset + input_buffer.size() > mix.size())
		mix.resize(offset + input_buffer.size());

	for (std::size_t i = 0; i < input_buffer.size(); ++i)
		mix[offset + i] += input_buffer[i];
}

void Mixer::ProcessInput()
{
	auto lock = input_section.lock();

	while (input_queue.size() > 0) {
		auto &[input_timestamp, input_buffer] = input_queue.front();
		ProcessInput(input_timestamp, input_buffer);
		input_queue.pop();
	}
}

std::size_t Mixer::TimestampToMixOffset(UINT64 timestamp)
{
	if (timestamp < mix_timestamp)
		return 0;

	return (std::min)(mix.size() / format.nChannels, DurationToFrames(timestamp - mix_timestamp));
}

std::tuple<std::size_t, std::size_t> Mixer::CalculateCutoff(UINT64 timestamp)
{
	if (timestamp < cutoff_end)
		return {0, 0};

	if (timestamp < cutoff_start)
		return {0, TimestampToMixOffset(timestamp - cutoff_end)};

	auto start = TimestampToMixOffset(timestamp - cutoff_start);
	auto end = TimestampToMixOffset(timestamp - cutoff_end);

	return {start, end};
}

void Mixer::Tick()
{
	ProcessInput();

	if (mix.size() == 0)
		return;

	UINT64 current_timestamp = GetCurrentTimestamp();
	auto [start, end] = CalculateCutoff(current_timestamp);

	if (start * format.nChannels >= mix.size()) {
		mix.clear();
		return;
	} else if (end - start == 0)
		return;

	obs_source_audio obs_packet = {
		.frames = static_cast<UINT32>(end - start),
		.speakers = get_obs_speaker_layout(&format),
		.format = get_obs_format(&format),
		.samples_per_sec = format.nSamplesPerSec,
		.timestamp = (mix_timestamp + FramesToDuration(start)) * 100,
	};

	obs_packet.data[0] = reinterpret_cast<BYTE *>(mix.data() + start * format.nChannels);
	obs_source_output_audio(source, &obs_packet);

	std::vector<float> new_mix(mix.begin() + end * format.nChannels, mix.end());
	mix = std::move(new_mix);

	mix_timestamp += FramesToDuration(end);
}

void Mixer::Run()
{
	// Force message queue creation
	MSG msg;
	PeekMessageA(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

	worker_ready.SetEvent();

	bool shutdown = false;
	while (!shutdown) {
		auto result = GetMessageW(&msg, NULL, 0, 0);
		if (result <= 0) {
			debug("message loop exited with %lld", static_cast<long long>(result));
			shutdown = true;
			continue;
		}

		switch (msg.message) {
		case MixerEvents::Shutdown:
			debug("shutting down");
			shutdown = true;
			break;

		case MixerEvents::Tick:
			Tick();
			break;

		default:
			break;
		}
	}
}

void Mixer::SubmitPacket(UINT64 timestamp, float *data, UINT32 num_frames)
{
	if (data == nullptr || num_frames == 0)
		return;

	auto lock = input_section.lock();

	while (input_queue.size() >= max_queued_packets) {
		debug("input queue overflow, dropping oldest packet");
		input_queue.pop();
	}

	auto &[_, buffer] = input_queue.emplace(timestamp, 0);
	buffer.assign(data, data + num_frames * format.nChannels);
}

static void CALLBACK post_tick(PVOID param, BOOLEAN)
{
	auto worker_tid = reinterpret_cast<intptr_t>(param);
	PostThreadMessageW(static_cast<DWORD>(worker_tid), MixerEvents::Tick, 0, 0);
}

Mixer::Mixer(obs_source_t *source, WAVEFORMATEX format) : source{source}, format{format}
{
	worker_thread = std::thread(&Mixer::Run, this);
	worker_tid = GetThreadId(worker_thread.native_handle());

	intptr_t tid_64 = worker_tid;

	SetThreadPriority(worker_thread.native_handle(), THREAD_PRIORITY_HIGHEST);
	CreateTimerQueueTimer(&timer, NULL, post_tick, reinterpret_cast<void *>(tid_64),
			      tick_interval, tick_interval, WT_EXECUTEINTIMERTHREAD);
}

Mixer::~Mixer()
{
	if (timer != nullptr) {
		DeleteTimerQueueTimer(NULL, timer, INVALID_HANDLE_VALUE);
		timer = nullptr;
	}

	if (!worker_thread.joinable())
		return;

	worker_ready.wait(5000);

	if (worker_tid != 0)
		PostThreadMessageW(worker_tid, MixerEvents::Shutdown, 0, 0);

	worker_thread.join();
}
