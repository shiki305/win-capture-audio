// Carrega obs.dll (da instalação do OBS) e depois o win-capture-audio.dll,
// confere se os três símbolos do módulo existem e imprime a versão.
// Uso: smoke-test.exe <caminho-do-obs.dll> <caminho-do-plugin.dll>
#include <windows.h>

#include <cstdio>

typedef unsigned int (*obs_module_ver_fn)(void);

int wmain(int argc, wchar_t **argv)
{
	if (argc < 3) {
		printf("uso: smoke-test.exe <obs.dll> <win-capture-audio.dll>\n");
		return 2;
	}

	const wchar_t *dir = argv[1];
	wchar_t dir_buf[MAX_PATH] = {};
	wcsncpy_s(dir_buf, dir, wcsrchr(dir, L'\\') ? (size_t)(wcsrchr(dir, L'\\') - dir) : 0);
	SetDllDirectoryW(dir_buf);
	SetCurrentDirectoryW(dir_buf);

	HMODULE obs = LoadLibraryExW(argv[1], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!obs) {
		printf("FALHA obs.dll: %lu\n", GetLastError());
		return 1;
	}
	printf("obs.dll carregado em %p\n", (void *)obs);

	HMODULE plugin = LoadLibraryExW(argv[2], nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!plugin) {
		printf("FALHA plugin: %lu\n", GetLastError());
		return 1;
	}
	printf("plugin carregado em %p\n", (void *)plugin);

	auto ver = (obs_module_ver_fn)GetProcAddress(plugin, "obs_module_ver");
	auto load = GetProcAddress(plugin, "obs_module_load");
	auto unload = GetProcAddress(plugin, "obs_module_unload");
	printf("obs_module_ver=%p obs_module_load=%p obs_module_unload=%p\n", (void *)ver, (void *)load, (void *)unload);

	if (ver) {
		unsigned int v = ver();
		printf("LIBOBS_API_VER: %u.%u.%u (0x%08X)\n", (v >> 24) & 0xFF, (v >> 16) & 0xFF, v & 0xFFFF, v);
	}

	BOOL ok = FreeLibrary(plugin);
	printf("FreeLibrary(plugin)=%d\n", ok);
	FreeLibrary(obs);

	return (ver && load && unload) ? 0 : 1;
}
