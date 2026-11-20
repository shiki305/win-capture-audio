#include <functional>
#include <windows.h>

#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include <vector>
#include <stdexcept>

#include <wil/result.h>
#include <wil/result_macros.h>
#include <wil/resource.h>

#include "audio-capture-helper.hpp"
#include "format-conversion.hpp"

AUDIOCLIENT_ACTIVATION_PARAMS AudioCaptureHelper::GetParams()
{
	auto mode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

	return {
		.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
		.ProcessLoopbackParams =
			{
				.TargetProcessId = pid,
				.ProcessLoopbackMode = mode,
			},
	};
}

PROPVARIANT
AudioCaptureHelper::GetPropvariant(AUDIOCLIENT_ACTIVATION_PARAMS *params)
{
	return {
		.vt = VT_BLOB,
		.blob =
			{
				.cbSize = sizeof(*params),
				.pBlobData = (BYTE *)params,
			},
	};
}

void AudioCaptureHelper::InitClient()
{
	auto params = GetParams();
	auto propvariant = GetPropvariant(&params);

	wil::com_ptr<IActivateAudioInterfaceAsyncOperation> async_op;
	CompletionHandler completion_handler;

	THROW_IF_FAILED(ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
						    __uuidof(IAudioClient), &propvariant,
						    &completion_handler, &async_op));

	completion_handler.event_finished.wait();
	THROW_IF_FAILED(completion_handler.activate_hr);

	client = completion_handler.client;

	THROW_IF_FAILED(
		client->Initialize(AUDCLNT_SHAREMODE_SHARED,
				   AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
				   5 * 10000000, 0, &format, NULL));

	THROW_IF_FAILED(client->SetEventHandle(events[HelperEvents::PacketReady].get()));
}

void AudioCaptureHelper::InitCapture()
{
	InitClient();
	THROW_IF_FAILED(
		client->GetService(__uuidof(IAudioCaptureClient), capture_client.put_void()));
}

void AudioCaptureHelper::RegisterMixer(Mixer *mixer)
{
	auto lock = mixers_section.lock();
	mixers.insert(mixer);
}

bool AudioCaptureHelper::UnRegisterMixer(Mixer *mixer)
{
	auto lock = mixers_section.lock();
	mixers.erase(mixer);

	return mixers.size() == 0;
}

void AudioCaptureHelper::ForwardToMixers(UINT64 qpc_position, BYTE *data, UINT32 num_frames)
{
	std::vector<Mixer *> targets;

	{
		auto lock = mixers_section.lock();
		targets.assign(mixers.begin(), mixers.end());
	}

	// Submitting outside the lock keeps a slow mixer from blocking capture
	// and avoids holding this lock while callbacks run.
	for (auto *mixer : targets)
		mixer->SubmitPacket(qpc_position, reinterpret_cast<float *>(data), num_frames);
}

void AudioCaptureHelper::ForwardPacket()
{
	size_t frame_size = format.nBlockAlign;

	UINT32 num_frames = 0;
	THROW_IF_FAILED(capture_client->GetNextPacketSize(&num_frames));

	while (num_frames > 0) {
		BYTE *new_data;
		DWORD flags;
		UINT64 qpc_position;

		THROW_IF_FAILED(capture_client->GetBuffer(&new_data, &num_frames, &flags, NULL,
							  &qpc_position));

		if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
			ForwardToMixers(qpc_position, new_data, num_frames);

		if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
			warn("data discontinuity flag set");

		if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
			warn("timestamp error flag set");

		THROW_IF_FAILED(capture_client->ReleaseBuffer(num_frames));
		THROW_IF_FAILED(capture_client->GetNextPacketSize(&num_frames));
	}
}

void AudioCaptureHelper::Capture()
{
	// NOTE: the audio client does not exist yet at this point; it is created
	// by InitCapture() -> InitClient() below, inside this thread. Do not guard
	// on `client` here.

	// ActivateAudioInterfaceAsync and the audio client require COM on the
	// calling thread. This has to happen *here*, on the capture thread: the
	// upstream code initialized COM in the constructor instead, which is the
	// thread that creates the helper (the source worker), not the thread that
	// actually calls the audio API. Worse, the paired CoUninitialize then ran
	// on whatever thread destroyed the helper (possibly OBS's main thread),
	// tearing down COM that OBS itself depends on.
	auto couninit = wil::CoInitializeEx(COINIT_MULTITHREADED);

	InitCapture();
	THROW_IF_FAILED(client->Start());

	// Visible at the default log level, so a normal OBS log shows whether the
	// process loopback capture actually started (the rest of this file logs at
	// debug level).
	info("capture started for pid %lu", pid);

	bool shutdown = false;
	while (!shutdown) {
		auto event_id = WaitForMultipleObjects(static_cast<DWORD>(events.size()),
						       events[0].addressof(), FALSE, INFINITE);

		if (event_id == WAIT_FAILED || event_id == WAIT_TIMEOUT) {
			error("wait failed with result: %lu", GetLastError());
			shutdown = true;
			break;
		}

		switch (event_id - WAIT_OBJECT_0) {
		case HelperEvents::PacketReady:
			ForwardPacket();
			break;

		case HelperEvents::Shutdown:
			shutdown = true;
			break;

		default:
			error("unexpected wait result: %lu", event_id);
			shutdown = true;
			break;
		}
	}

	THROW_IF_FAILED(client->Stop());
}

void AudioCaptureHelper::CaptureSafe()
{
	try {
		Capture();
	} catch (const wil::ResultException &e) {
		error("%s", e.what());
	} catch (const std::exception &e) {
		error("%s", e.what());
	} catch (...) {
		error("unknown exception in capture thread");
	}
}

AudioCaptureHelper::AudioCaptureHelper(Mixer *mixer, WAVEFORMATEX format, DWORD pid)
	: mixers{mixer}, format{format}, pid{pid}
{
	for (auto &event : events)
		event.create();

	capture_thread = std::thread(&AudioCaptureHelper::CaptureSafe, this);
}

AudioCaptureHelper::~AudioCaptureHelper()
{
	{
		auto lock = mixers_section.lock();
		mixers.clear();
	}

	if (events[HelperEvents::Shutdown])
		events[HelperEvents::Shutdown].SetEvent();

	if (capture_thread.joinable())
		capture_thread.join();
}