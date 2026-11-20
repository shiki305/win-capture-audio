#include <windows.h>
#include <processthreadsapi.h>
#include <psapi.h>

#include <memory>
#include <utility>

#include <wil/result_macros.h>
#include <wil/resource.h>
#include <winuser.h>

#include "session-monitor.hpp"

static SessionMonitor *instance = nullptr;

void SessionMonitor::Create()
{
	if (instance != nullptr)
		return;

	instance = new SessionMonitor();
}

void SessionMonitor::Destroy()
{
	delete instance;
	instance = nullptr;
}

SessionMonitor *SessionMonitor::Instance()
{
	return instance;
}

DeviceWatcher::DeviceWatcher(std::wstring device_id, wil::com_ptr<IMMDevice> device,
			     DWORD worker_tid)
	: device_id{device_id},
	  device{device},
	  worker_tid{worker_tid},
	  session_notification_client{worker_tid}
{
	THROW_IF_FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, NULL,
					 manager2.put_void()));

	THROW_IF_FAILED(manager2->RegisterSessionNotification(&session_notification_client));

	// From here on the device forwards session notifications to us, so a failure
	// has to unregister before propagating or the callback would point at a
	// destroyed object.
	try {
		THROW_IF_FAILED(manager2->GetSessionEnumerator(enumerator.put()));

		int num_sessions = 0;
		THROW_IF_FAILED(enumerator->GetCount(&num_sessions));

		for (int i = 0; i < num_sessions; ++i) {
			wil::com_ptr<IAudioSessionControl> session;
			THROW_IF_FAILED(enumerator->GetSession(i, session.put()));

			AudioSessionState state;
			THROW_IF_FAILED(session->GetState(&state));

			if (state == AudioSessionStateExpired)
				continue;

			// The message queue takes over this reference; if posting fails
			// the reference is released here instead of leaking.
			session->AddRef();
			if (!PostThreadMessageA(worker_tid, SessionEvents::SessionAdded,
						reinterpret_cast<WPARAM>(session.get()), 0)) {
				session->Release();
			}
		}
	} catch (...) {
		manager2->UnregisterSessionNotification(&session_notification_client);
		throw;
	}
}

DeviceWatcher::~DeviceWatcher()
{
	if (manager2)
		manager2->UnregisterSessionNotification(&session_notification_client);
}

SessionWatcher::SessionWatcher(DWORD worker_tid,
			       const wil::com_ptr<IAudioSessionControl> &session_control)
	: session_control{session_control}
{
	wil::unique_cotaskmem_string session_id_raw;

	auto session_control2 = GetSessionControl2();
	if (session_control2 == nullptr)
		THROW_HR(E_NOINTERFACE);

	THROW_IF_FAILED(session_control2->GetSessionIdentifier(session_id_raw.put()));

	session_id = session_id_raw.get();

	THROW_IF_FAILED(session_control2->GetProcessId(&pid));

	notification_client.emplace(worker_tid, SessionKey(pid, session_id));

	THROW_IF_FAILED(
		session_control->RegisterAudioSessionNotification(&notification_client.value()));

	wil::unique_process_handle session_process{
		OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, pid)};

	if (session_process.get() == NULL) {
		executable = std::string("unknown");
		return;
	}

	wchar_t name_buf[MAX_PATH] = {'\0'};
	DWORD length = GetProcessImageFileNameW(session_process.get(), name_buf, MAX_PATH - 1);
	if (length == 0) {
		executable = std::string("unknown");
		return;
	}

	auto num_chars = WideCharToMultiByte(CP_UTF8, 0, name_buf, -1, NULL, 0, NULL, NULL);
	if (num_chars <= 1) {
		executable = std::string("unknown");
		return;
	}

	std::string executable_path(num_chars - 1, '\0');
	WideCharToMultiByte(CP_UTF8, 0, name_buf, -1, executable_path.data(), num_chars, NULL, NULL);

	executable = executable_path.substr(executable_path.find_last_of("\\") + 1);
	debug("registered new session: [%lu] %s", pid, executable.c_str());
}

SessionWatcher::~SessionWatcher()
{
	if (session_control && notification_client.has_value())
		session_control->UnregisterAudioSessionNotification(&notification_client.value());
	debug("session expired: [%lu] %s", pid, executable.c_str());
}

void SessionMonitor::Init()
{
	enumerator = wil::CoCreateInstance<MMDeviceEnumerator, IMMDeviceEnumerator>();
	THROW_IF_FAILED(
		enumerator->RegisterEndpointNotificationCallback(&device_notification_client));

	wil::com_ptr<IMMDeviceCollection> collection;
	THROW_IF_FAILED(
		enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put()));

	UINT num_devices = 0;
	THROW_IF_FAILED(collection->GetCount(&num_devices));

	for (UINT i = 0; i < num_devices; ++i) {
		wil::com_ptr<IMMDevice> device;
		THROW_IF_FAILED(collection->Item(i, device.put()));

		wil::unique_cotaskmem_string device_id;
		THROW_IF_FAILED(device->GetId(device_id.put()));

		AddDevice(std::wstring(device_id.get()), device);
	}
}

void SessionMonitor::UnInit()
{
	if (enumerator)
		enumerator->UnregisterEndpointNotificationCallback(&device_notification_client);
}

void SessionMonitor::AddDevice(MSG msg)
{
	std::unique_ptr<std::wstring> id{reinterpret_cast<std::wstring *>(msg.wParam)};
	if (!id)
		return;

	wil::com_ptr<IMMDevice> device;
	THROW_IF_FAILED(enumerator->GetDevice(id->c_str(), device.put()));

	wil::com_ptr<IMMEndpoint> endpoint = device.query<IMMEndpoint>();
	EDataFlow data_flow;
	THROW_IF_FAILED(endpoint->GetDataFlow(&data_flow));

	if (data_flow == eRender)
		AddDevice(*id, device);
}

void SessionMonitor::AddDevice(std::wstring id, wil::com_ptr<IMMDevice> device)
{
	device_watchers.try_emplace(id, id, device, worker_tid);
	debug("registered new device: %ls", id.c_str());
}

void SessionMonitor::RemoveDevice(MSG msg)
{
	std::unique_ptr<std::wstring> id{reinterpret_cast<std::wstring *>(msg.wParam)};
	if (!id)
		return;

	RemoveDevice(*id);
}

void SessionMonitor::RemoveDevice(std::wstring id)
{
	if (!device_watchers.contains(id))
		return;

	device_watchers.erase(id);
	debug("removed device: %ls", id.c_str());
}

void SessionMonitor::AddSession(MSG msg)
{
	auto session_control_ptr = reinterpret_cast<IAudioSessionControl *>(msg.wParam);
	if (session_control_ptr == nullptr)
		return;

	wil::com_ptr<IAudioSessionControl> session_control;
	*session_control.put() = session_control_ptr;

	auto session_control2 = session_control.query<IAudioSessionControl2>();
	if (session_control2 == nullptr)
		return;

	if (session_control2->IsSystemSoundsSession() == S_OK)
		return;

	std::wstring session_id;
	DWORD pid = 0;

	std::string executable;

	try {
		wil::unique_cotaskmem_string session_id_raw;
		THROW_IF_FAILED(session_control2->GetSessionIdentifier(session_id_raw.put()));

		session_id = std::wstring(session_id_raw.get());
		THROW_IF_FAILED(session_control2->GetProcessId(&pid));

		// System sounds and the audio service itself have no usable process.
		if (pid == 0)
			return;

		if (session_watchers.contains({pid, session_id}))
			return;

		auto [it, inserted] = session_watchers.try_emplace({pid, session_id}, worker_tid,
								   session_control);

		if (!inserted)
			return;

		executable = it->second.GetExecutable();
	} catch (const wil::ResultException &e) {
		error("unable to add session: %s", e.what());
		return;
	}

	{
		auto lock = sessions_lock.lock();
		sessions_list.emplace(SessionKey(pid, session_id), executable);
	}

	{
		auto lock = callbacks_lock.lock();
		for (auto [client_tid, msgs] : callbacks)
			PostThreadMessageA(client_tid, std::get<0>(msgs), 0, 0);
	}
}

void SessionMonitor::RemoveSession(MSG msg)
{
	std::unique_ptr<SessionKey> session_key{reinterpret_cast<SessionKey *>(msg.wParam)};
	if (!session_key)
		return;

	auto it = session_watchers.find(*session_key);
	if (it == session_watchers.end())
		return;

	auto executable = it->second.GetExecutable();
	session_watchers.erase(it);

	{
		auto lock = sessions_lock.lock();
		auto itr = sessions_list.find(*session_key);
		if (itr != sessions_list.end())
			sessions_list.erase(itr);
	}

	{
		auto lock = callbacks_lock.lock();
		for (auto [client_tid, msgs] : callbacks)
			PostThreadMessageA(client_tid, std::get<1>(msgs), 0, 0);
	}

	debug("session expired: [%lu] %s", session_key->pid, executable.c_str());
}

void SessionMonitor::Run()
{
	// This thread talks to the Windows audio session APIs, which need COM to be
	// initialized on the calling thread (CoCreateInstance fails with
	// CO_E_NOTINITIALIZED otherwise, and the session list comes back empty).
	auto couninit = wil::CoInitializeEx(COINIT_MULTITHREADED);

	// Force message queue creation
	MSG msg;
	PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);

	worker_ready.SetEvent();
	Init();

	bool shutdown = false;
	while (!shutdown) {
		auto result = GetMessageW(&msg, NULL, 0, 0);
		if (result <= 0) {
			debug("shutting down (message loop exited with %lld)",
			      static_cast<long long>(result));
			shutdown = true;
			continue;
		}

		switch (msg.message) {
		case SessionEvents::Shutdown:
			debug("shutting down");
			shutdown = true;
			break;

		case SessionEvents::DeviceAdded:
			AddDevice(msg);
			break;

		case SessionEvents::DeviceRemoved:
			RemoveDevice(msg);
			break;

		case SessionEvents::SessionAdded:
			AddSession(msg);
			break;

		case SessionEvents::SessionExpired:
			RemoveSession(msg);
			break;

		default:
			// Thread messages (WM_QUIT, WM_TIMER, ...) we do not care about.
			break;
		}
	}

	UnInit();
}

void SessionMonitor::SafeRun()
{
	try {
		Run();
	} catch (const wil::ResultException &e) {
		error("%s", e.what());
	} catch (const std::exception &e) {
		error("%s", e.what());
	} catch (...) {
		error("unknown exception in session monitor thread");
	}
}

SessionMonitor::SessionMonitor()
{
	worker_thread = std::thread(&SessionMonitor::SafeRun, this);
	worker_tid = GetThreadId(worker_thread.native_handle());

	device_notification_client.SetWorkerThreadId(worker_tid);
}

SessionMonitor::~SessionMonitor()
{
	if (!worker_thread.joinable())
		return;

	// Make sure the worker has created its message queue before posting to it.
	worker_ready.wait(5000);

	if (worker_tid != 0)
		PostThreadMessageW(worker_tid, SessionEvents::Shutdown, 0, 0);

	worker_thread.join();
}

void SessionMonitor::RegisterEvent(DWORD client_tid, UINT session_added, UINT session_expired)
{
	auto lock = callbacks_lock.lock();
	callbacks[client_tid] = {session_added, session_expired};
}

void SessionMonitor::UnRegisterEvent(DWORD client_tid)
{
	auto lock = callbacks_lock.lock();
	auto itr = callbacks.find(client_tid);
	if (itr != callbacks.end())
		callbacks.erase(itr);
}

std::unordered_map<SessionKey, std::string> SessionMonitor::GetSessions()
{
	auto lock = sessions_lock.lock();
	return sessions_list;
}
