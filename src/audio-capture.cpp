#include <cstdint>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <optional>
#include <string>
#include <format>
#include <set>
#include <unordered_map>
#include <vector>

#include <windows.h>
#include <stringapiset.h>
#include <processthreadsapi.h>
#include <mmreg.h>
#include <audiopolicy.h>
#include <audioclientactivationparams.h>
#include <tlhelp32.h>

#include <obs.h>
#include <obs-module.h>
#include <obs-data.h>
#include <obs-properties.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <winuser.h>

#include "wil/result.h"
#include "wil/result_macros.h"

#include "audio-capture.hpp"
#include "audio-capture-helper-manager.hpp"

AudioCaptureHelperManager helper_manager;

static std::unordered_map<DWORD, DWORD> GetProcessParents(const std::set<DWORD> &pids)
{
	wil::unique_handle handle;
	*handle.put() = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	PROCESSENTRY32W info;
	info.dwSize = sizeof(PROCESSENTRY32W);

	std::unordered_map<DWORD, DWORD> parent_map;

	if (handle.get() == INVALID_HANDLE_VALUE)
		return parent_map;

	bool ret = Process32FirstW(handle.get(), &info);

	while (ret) {
		if (pids.contains(info.th32ProcessID))
			parent_map[info.th32ProcessID] = info.th32ParentProcessID;

		ret = Process32NextW(handle.get(), &info);
	}

	for (auto pid : pids) {
		if (parent_map.contains(pid))
			continue;

		parent_map[pid] = 0;
	}

	return parent_map;
}

std::set<DWORD>
AudioCapture::DeDuplicateCaptureList(const std::set<DWORD> &pids,
				     const std::set<DWORD> &exclude_pids = std::set<DWORD>())
{
	std::set<DWORD> all_pids = pids;
	all_pids.insert(exclude_pids.begin(), exclude_pids.end());

	auto parents = GetProcessParents(all_pids);

	std::set<DWORD> uncaptured_pids = pids;
	for (auto pid : exclude_pids) {
		auto it = parents.find(pid);
		if (it != parents.end())
			uncaptured_pids.erase(it->second);
	}

	std::set<DWORD> explicitly_captured_pids;
	std::set<DWORD> implicitly_captured_pids;

	while (uncaptured_pids.size() > 0) {
		for (auto pid : uncaptured_pids) {
			auto parent = parents.find(pid);
			if (parent != parents.end() && uncaptured_pids.contains(parent->second))
				continue;

			explicitly_captured_pids.insert(pid);
		}

		for (auto pid : explicitly_captured_pids)
			uncaptured_pids.erase(pid);

		for (auto pid : uncaptured_pids) {
			auto parent = parents.find(pid);
			if (parent == parents.end() ||
			    !explicitly_captured_pids.contains(parent->second))
				continue;

			implicitly_captured_pids.insert(pid);
			uncaptured_pids.erase(pid);
		}
	}

	return explicitly_captured_pids;
}

void AudioCapture::StartCapture(const std::set<DWORD> &new_pids)
{
	if (!mixer.has_value())
		return;

	for (auto pid : pids) {
		if (new_pids.contains(pid))
			continue;

		helper_manager.UnRegisterMixer(pid, &mixer.value());
	}

	for (auto new_pid : new_pids) {
		if (pids.contains(new_pid))
			continue;

		helper_manager.RegisterMixer(new_pid, &mixer.value());
	}

	pids = new_pids;
}

void AudioCapture::StopCapture()
{
	if (mixer.has_value()) {
		for (auto pid : pids)
			helper_manager.UnRegisterMixer(pid, &mixer.value());
	}

	pids.clear();
}

void AudioCapture::WorkerUpdate()
{
	auto config_lock = config_section.lock();
	auto cfg = this->config;
	config_lock.reset();

	if (cfg.mode == MODE_HOTKEY) {
		if (cfg.hotkey_window == NULL) {
			StopCapture();
			return;
		}

		DWORD pid = 0;
		GetWindowThreadProcessId(cfg.hotkey_window, &pid);

		if (pid == 0) {
			StopCapture();
			return;
		}

		StartCapture({pid});
		return;
	}

	auto *monitor = SessionMonitor::Instance();
	if (monitor == nullptr) {
		StopCapture();
		return;
	}

	auto sessions = monitor->GetSessions();

	std::set<DWORD> capture_pids;
	std::set<DWORD> exclude_pids;

	for (auto &[key, executable] : sessions) {
		if ((!cfg.executables.contains(executable)) ^ cfg.exclude) {
			exclude_pids.insert(key.pid);
			continue;
		}

		capture_pids.insert(key.pid);
	}

	if (capture_pids.size() == 0) {
		StopCapture();
		return;
	}

	StartCapture(AudioCapture::DeDuplicateCaptureList(
		capture_pids, cfg.exclude ? exclude_pids : std::set<DWORD>()));
}

bool AudioCapture::Tick(const MSG &msg)
{
	bool shutdown = false;

	switch (msg.message) {
	case CaptureEvents::Shutdown:
		debug("shutting down");
		shutdown = true;

		break;

	case CaptureEvents::Update:
	case CaptureEvents::SessionAdded:
	case CaptureEvents::SessionExpired:
		WorkerUpdate();
		break;

	default:
		warn("unexpected event id, ignoring");
		break;
	}

	return shutdown;
}

void AudioCapture::Run()
{
	// Force message queue creation
	MSG msg;
	PeekMessageA(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

	worker_ready.SetEvent();

	// The initial configuration was stored by the constructor before this
	// thread existed, so apply it directly instead of posting to ourselves.
	WorkerUpdate();

	bool shutdown = false;
	while (!shutdown) {
		auto result = GetMessageW(&msg, NULL, 0, 0);
		if (result <= 0) {
			debug("message loop exited with %lld", static_cast<long long>(result));
			shutdown = true;
			continue;
		}

		if (msg.message >= CaptureEvents::Shutdown &&
		    msg.message <= CaptureEvents::SessionExpired) {
			shutdown = Tick(msg);
		}
	}

	StopCapture();
}

void AudioCapture::Update(obs_data_t *settings)
{
	AudioCaptureConfig new_config = {
		.mode = (mode)obs_data_get_int(settings, SETTING_MODE),
		.exclude = obs_data_get_bool(settings, SETTING_EXCLUDE),
	};

	if (new_config.mode == MODE_SESSION)
		new_config.executables = GetExecutables(settings);

	auto lock = config_section.lock();
	config = std::move(new_config);
	lock.reset();

	auto tid = worker_tid.load();
	if (tid != 0)
		PostThreadMessageA(tid, CaptureEvents::Update, 0, 0);
}

static void audio_capture_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	ctx->Update(settings);
}

bool AudioCapture::IsUwpWindow(HWND window)
{
	wchar_t name[256] = {L'\0'};

	if (!GetClassNameW(window, name, sizeof(name) / sizeof(wchar_t)))
		return false;

	return wcscmp(name, L"ApplicationFrameWindow") == 0;
}

HWND AudioCapture::GetUwpActualWindow(HWND parent_window)
{
	DWORD parent_pid;
	HWND child_window;

	GetWindowThreadProcessId(parent_window, &parent_pid);
	child_window = FindWindowEx(parent_window, NULL, NULL, NULL);

	while (child_window != NULL) {
		DWORD child_pid;
		GetWindowThreadProcessId(child_window, &child_pid);

		if (child_pid != parent_pid)
			return child_window;

		child_window = FindWindowEx(parent_window, child_window, NULL, NULL);
	}

	return NULL;
}

void AudioCapture::HotkeyStart()
{
	auto window = GetForegroundWindow();
	if (window == NULL)
		return;

	if (AudioCapture::IsUwpWindow(window))
		window = AudioCapture::GetUwpActualWindow(window);

	{
		auto lock = config_section.lock();
		if (config.mode != MODE_HOTKEY)
			return;

		config.hotkey_window = window;
	}

	auto tid = worker_tid.load();
	if (tid != 0)
		PostThreadMessageA(tid, CaptureEvents::Update, 0, 0);
}

void AudioCapture::HotkeyStop()
{
	{
		auto lock = config_section.lock();
		if (config.mode != MODE_HOTKEY)
			return;

		config.hotkey_window = NULL;
	}

	auto tid = worker_tid.load();
	if (tid != 0)
		PostThreadMessageA(tid, CaptureEvents::Update, 0, 0);
}

static bool hotkey_start(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	if (!pressed)
		return false;

	auto *ctx = static_cast<AudioCapture *>(data);
	ctx->HotkeyStart();

	return true;
}

static bool hotkey_stop(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	if (!pressed)
		return false;

	auto *ctx = static_cast<AudioCapture *>(data);
	ctx->HotkeyStop();

	return true;
}

AudioCapture::AudioCapture(obs_data_t *settings, obs_source_t *source) : source{source}
{
	// Store the configuration before the worker exists; Update() only posts
	// when the worker thread id is known.
	Update(settings);

	mixer.emplace(source, helper_manager.GetFormat());

	worker_thread = std::thread(&AudioCapture::Run, this);
	worker_tid.store(GetThreadId(worker_thread.native_handle()));

	if (auto *monitor = SessionMonitor::Instance()) {
		monitor->RegisterEvent(worker_tid.load(), CaptureEvents::SessionAdded,
				       CaptureEvents::SessionExpired);
	}

	hotkey_pair = obs_hotkey_pair_register_source(source, HOTKEY_START, TEXT_HOTKEY_START,
						      HOTKEY_STOP, TEXT_HOTKEY_STOP, hotkey_start,
						      hotkey_stop, this, this);
}

static void *audio_capture_create(obs_data_t *settings, obs_source_t *source)
{
	try {
		return new AudioCapture(settings, source);
	} catch (const wil::ResultException &e) {
		error("failed to create context: %s", e.what());
		return nullptr;
	} catch (const std::exception &e) {
		error("failed to create context: %s", e.what());
		return nullptr;
	}
}

AudioCapture::~AudioCapture()
{
	if (auto *monitor = SessionMonitor::Instance())
		monitor->UnRegisterEvent(worker_tid.load());

	if (worker_thread.joinable()) {
		worker_ready.wait(5000);

		auto tid = worker_tid.load();
		if (tid != 0)
			PostThreadMessageA(tid, CaptureEvents::Shutdown, 0, 0);

		worker_thread.join();
	}

	// The worker stops its capture when it exits. If it never started (or died
	// before the loop) there may still be registered mixers pointing at this
	// object, which would be a use-after-free once we are gone.
	StopCapture();
}

static void audio_capture_destroy(void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	delete ctx;
}

static bool mode_callback(obs_properties_t *ps, obs_property_t *p, obs_data_t *settings)
{
	int64_t mode = obs_data_get_int(settings, SETTING_MODE);

	p = obs_properties_get(ps, SETTING_EXECUTABLE_LIST);
	obs_property_set_visible(p, mode == MODE_SESSION);

	p = obs_properties_get(ps, SETTING_ACTIVE_SESSION_GROUP);
	obs_property_set_visible(p, mode == MODE_SESSION);

	p = obs_properties_get(ps, SETTING_EXCLUDE);
	obs_property_set_visible(p, mode == MODE_SESSION);

	return true;
}

std::tuple<std::string, std::string>
AudioCapture::MakeSessionOptionStrings(std::set<DWORD> session_pids,
				       const std::string &executable, bool added)
{
	auto pids_string = std::string();

	if (session_pids.size() > 0) {
		auto it = std::begin(session_pids);
		pids_string.append(std::format("{}", *it));

		++it;
		for (auto end = std::end(session_pids); it != end; ++it)
			pids_string.append(std::format(", {}", *it));
	} else {
		pids_string.append("*");
	}

	if (!added)
		return {std::format("[{}] {}", pids_string, executable), executable};

	return {std::format("[{}] {} (added)", pids_string, executable),
		std::format("{}", next_session_option_id++)};
}

static bool executable_list_callback(void *data, obs_properties_t *ps, obs_property_t *p,
				     obs_data_t *settings)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	if (ctx == nullptr)
		return false;

	auto *active_session_list = obs_properties_get(ps, SETTING_ACTIVE_SESSION_LIST);
	auto *active_session_add = obs_properties_get(ps, SETTING_ACTIVE_SESSION_ADD);

	if (active_session_list == nullptr)
		return false;

	obs_property_list_clear(active_session_list);
	ctx->FillActiveSessionList(active_session_list, active_session_add);

	return true;
}

static bool session_add_callback(obs_properties_t *ps, obs_property_t *p, void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);
	if (ctx == nullptr)
		return false;

	auto *source = ctx->GetSource();
	if (source == nullptr)
		return false;

	auto *settings = obs_source_get_settings(source);
	if (settings == nullptr)
		return false;

	auto *executable_list_array = obs_data_get_array(settings, SETTING_EXECUTABLE_LIST);

	if (executable_list_array == nullptr) {
		executable_list_array = obs_data_array_create();
		obs_data_set_array(settings, SETTING_EXECUTABLE_LIST, executable_list_array);
	}

	const char *executable = obs_data_get_string(settings, SETTING_ACTIVE_SESSION_LIST);
	if (executable == nullptr || executable[0] == '\0') {
		obs_data_array_release(executable_list_array);
		obs_data_release(settings);
		return false;
	}

	auto *executable_obj = obs_data_create();
	obs_data_set_bool(executable_obj, "hidden", false);
	obs_data_set_bool(executable_obj, "selected", false);
	obs_data_set_string(executable_obj, "value", executable);

	obs_data_array_push_back(executable_list_array, executable_obj);

	auto *active_session_list = obs_properties_get(ps, SETTING_ACTIVE_SESSION_LIST);
	auto *active_session_add = obs_properties_get(ps, SETTING_ACTIVE_SESSION_ADD);

	if (active_session_list != nullptr) {
		obs_property_list_clear(active_session_list);
		ctx->FillActiveSessionList(active_session_list, active_session_add);
	}

	obs_data_release(executable_obj);
	obs_data_array_release(executable_list_array);
	obs_data_release(settings);

	return true;
}

std::set<std::string> AudioCapture::GetExecutables(obs_data_t *settings)
{
	std::set<std::string> executables;

	auto *executable_list_array = obs_data_get_array(settings, SETTING_EXECUTABLE_LIST);
	if (executable_list_array == nullptr)
		return executables;

	auto count = obs_data_array_count(executable_list_array);

	for (std::size_t i = 0; i < count; ++i) {
		auto *item = obs_data_array_item(executable_list_array, i);
		if (item == nullptr)
			continue;

		auto *executable = obs_data_get_string(item, "value");
		if (executable != nullptr && executable[0] != '\0')
			executables.insert(std::string(executable));

		obs_data_release(item);
	}

	obs_data_array_release(executable_list_array);
	return executables;
}

void AudioCapture::FillActiveSessionList(obs_property_t *session_list, obs_property_t *session_add)
{
	if (session_list == nullptr)
		return;

	auto *settings = obs_source_get_settings(GetSource());
	if (settings == nullptr)
		return;

	auto executables = GetExecutables(settings);

	std::unordered_map<std::string, std::set<DWORD>> session_options;

	if (auto *monitor = SessionMonitor::Instance()) {
		auto sessions = monitor->GetSessions();
		for (auto &[key, executable] : sessions)
			session_options[executable].insert(key.pid);
	}

	std::vector<std::tuple<std::string, std::set<DWORD>>> enabled_session_options;
	std::vector<std::tuple<std::string, std::set<DWORD>>> disabled_session_options;

	for (auto &[executable, session_pids] : session_options) {
		if (executables.contains(executable)) {
			disabled_session_options.push_back({executable, session_pids});
			continue;
		}

		enabled_session_options.push_back({executable, session_pids});
	}

	auto cmp = [](auto a, auto b) {
		return astrcmpi(std::get<0>(a).c_str(), std::get<0>(b).c_str()) < 0;
	};

	std::sort(enabled_session_options.begin(), enabled_session_options.end(), cmp);
	std::sort(disabled_session_options.begin(), disabled_session_options.end(), cmp);

	for (auto &[executable, session_pids] : enabled_session_options) {
		auto [name, val] = AudioCapture::MakeSessionOptionStrings(session_pids, executable);
		obs_property_list_add_string(session_list, name.c_str(), val.c_str());
	}

	for (auto &[executable, session_pids] : disabled_session_options) {
		auto [name, val] =
			AudioCapture::MakeSessionOptionStrings(session_pids, executable, true);
		auto idx = obs_property_list_add_string(session_list, name.c_str(), val.c_str());
		obs_property_list_item_disable(session_list, idx, true);
	}

	if (session_add != nullptr) {
		obs_property_set_enabled(session_add, enabled_session_options.size() != 0);
		obs_property_set_enabled(session_list, enabled_session_options.size() != 0);
	}

	obs_data_release(settings);
}

static obs_properties_t *audio_capture_properties(void *data)
{
	auto *ctx = static_cast<AudioCapture *>(data);

	obs_properties_t *ps = obs_properties_create();

	// Mode setting (specific session or hotkey)
	auto *mode = obs_properties_add_list(ps, SETTING_MODE, TEXT_MODE, OBS_COMBO_TYPE_LIST,
					     OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(mode, TEXT_MODE_SESSION, MODE_SESSION);
	obs_property_list_add_int(mode, TEXT_MODE_HOTKEY, MODE_HOTKEY);

	obs_property_set_modified_callback(mode, mode_callback);

	// Executable list setting
	auto *executable_list =
		obs_properties_add_editable_list(ps, SETTING_EXECUTABLE_LIST, TEXT_EXECUTABLE_LIST,
						 OBS_EDITABLE_LIST_TYPE_STRINGS, NULL, NULL);

	obs_property_set_modified_callback2(executable_list, executable_list_callback, ctx);

	// Exclude setting
	obs_properties_add_bool(ps, SETTING_EXCLUDE, TEXT_EXCLUDE);

	// Active session group
	obs_properties_t *active_session_group = obs_properties_create();

	// Active session list
	auto *active_session_list = obs_properties_add_list(
		active_session_group, SETTING_ACTIVE_SESSION_LIST, TEXT_ACTIVE_SESSION_LIST,
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	// Add session button
	auto *active_session_add =
		obs_properties_add_button2(active_session_group, SETTING_ACTIVE_SESSION_ADD,
					   TEXT_ACTIVE_SESSION_ADD, session_add_callback, ctx);

	if (ctx != nullptr)
		ctx->FillActiveSessionList(active_session_list, active_session_add);

	// Active session group
	obs_properties_add_group(ps, SETTING_ACTIVE_SESSION_GROUP, TEXT_ACTIVE_SESSION_GROUP,
				 OBS_GROUP_NORMAL, active_session_group);

	return ps;
}

static void audio_capture_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, SETTING_MODE, MODE_SESSION);

	auto *executable_list = obs_data_array_create();
	obs_data_set_default_array(settings, SETTING_EXECUTABLE_LIST, executable_list);
	obs_data_array_release(executable_list);

	obs_data_set_default_bool(settings, SETTING_EXCLUDE, false);
}

static const char *audio_capture_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return TEXT_NAME;
}

struct obs_source_info audio_capture_info = {
	.id = "audio_capture",

	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO,

	.get_name = audio_capture_get_name,

	.create = audio_capture_create,
	.destroy = audio_capture_destroy,

	.get_defaults = audio_capture_defaults,
	.get_properties = audio_capture_properties,

	.update = audio_capture_update,

	.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT,

	// Mantém o ID versionado "audio_capture_v1" gravado nas cenas existentes.
	// O OBS monta o id como "<id>_v<version>" (obs-module.c), então sem isto o
	// plugin registra "audio_capture" puro e as fontes antigas viram
	// "Source ID 'audio_capture_v1' not found". É o mesmo valor usado pelos
	// builds anteriores deste plugin (o upstream nunca definiu .version).
	.version = 1,
};
