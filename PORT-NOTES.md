# win-capture-audio — port para OBS 33

Este diretório é um fork de manutenção do
[bozbez/win-capture-audio](https://github.com/bozbez/win-capture-audio)
(GPL-2.0), portado para o OBS Studio 33 e com correções de estabilidade.

O upstream está parado desde janeiro de 2024 e o último release publicado é o
`2.2.3-beta` (julho de 2022). Este fork mantém o mesmo `id` de fonte
(`audio_capture`) e o mesmo nome de DLL (`win-capture-audio.dll`), então as
cenas existentes continuam funcionando ao substituir o binário.

> **Importante — `.version = 1`:** o OBS monta o ID versionado de uma fonte como
> `<id>_v<version>` (`obs_register_source_s`, em `libobs/obs-module.c`). Com
> `.version = 0` o ID registrado é só `audio_capture`; com `.version = 1` vira
> `audio_capture_v1`. As cenas gravam esse ID versionado no campo
> `versioned_id`, então **os dois builds precisam usar o mesmo número**. Os
> builds anteriores deste plugin usavam `1` (o upstream nunca definiu
> `.version`), e é o valor que este fork mantém. Sintoma de divergência:
> `Source ID 'audio_capture_v1' not found` no log e as fontes sem áudio.

## O que mudou

### Build

- `CMakeLists.txt` reescrito: compila contra o layout de SDK local
  (`deps/sdk/include` + `deps/sdk/lib/libobs.lib`) e continua aceitando
  `find_package(libobs)` para builds no padrão do OBS.
- `buildspec.json` para ferramentas que esperam o formato do
  `obs-plugintemplate`.
- Versão do plugin agora vai no recurso do Windows (`2.4.0.0`), visível nas
  propriedades do arquivo.
- `tools/prepare-sdk.ps1` gera o SDK a partir dos headers da tag do OBS e de uma
  import library criada a partir do **seu** `obs.dll`. Nada de baixar o bundle de
  dependências inteiro (~1,5 GB).
- `tools/build.ps1`, `tools/install-local.ps1` e `tools/package.ps1` para
  compilar, instalar e empacotar.

### Correções de estabilidade (foco: travamentos ao remover/fechar)

| Problema | Correção |
| --- | --- |
| `Update()` era chamado no construtor **antes** do thread existir e usava `worker_tid` não inicializado; a mensagem podia ir para outro thread do processo | `worker_tid` virou `std::atomic`, começa em 0 e só é lido depois de válido; a configuração inicial é aplicada diretamente no início do worker |
| `GetMessage(&msg, (HWND)-1, WM_USER, 0)` — handle inválido, retorno `-1` tratado como sucesso | loop corrigido (`GetMessageW(&msg, NULL, 0, 0)`, trata `0` e `-1`), ignora mensagens alheias |
| `~AudioCapture()` retornava cedo sem desregistrar os mixers, deixando ponteiros pendurados nos helpers | `StopCapture()` sempre roda no destrutor; `join()` só quando o thread é joinable e com `wait` limitado |
| Helper podia sobreviver ao `Mixer` e chamar `SubmitPacket` em memória liberada | `helper_manager.Shutdown()` no `obs_module_unload` e no destrutor do manager; registro/remoção sempre pareados |
| Fila do mixer sem limite (crescimento indefinido se o thread travar) | limite de 200 pacotes, descarte do mais antigo, log em nível debug |
| Salto de timestamp gigante fazia `mix.resize()` alocar sem limite | reset do buffer quando o gap passa de 10 s |
| `ForwardToMixers` segurava o lock enquanto chamava os mixers | copia a lista sob lock e envia fora dele |
| `~AudioCaptureHelper` dava `join()` em thread possivelmente não iniciada | guarda `joinable()` e só sinaliza evento existente |
| Vazamento de referência COM em `DeviceWatcher` (dois `AddRef`) e de `SessionKey` em `RemoveSession` | referências balanceadas; `unique_ptr` nos ponteiros passados por mensagem |
| `nChannels` recebia o enum `speaker_layout` direto (7.1 virava 7 canais) | usa `get_audio_channels()` |
| Threads de trabalho usavam APIs de áudio/COM sem `CoInitializeEx` (o `CoCreateInstance` do enumerador de dispositivos falhava com `CO_E_NOTINITIALIZED` e a lista de sessões vinha vazia) | `CoInitializeEx` (MTA) no thread do `SessionMonitor` e no thread de captura do helper |
| `GetSessionControl2()`/`QueryInterface` podiam devolver `nullptr` e eram desreferenciados | checagem de `nullptr` antes do uso |
| Exceção no meio do `DeviceWatcher` deixava uma notificação registrada apontando para objeto destruído | `try/catch` que desregistra antes de propagar |
| `obs_properties_add_button` (depreciado no OBS 33) | `obs_properties_add_button2` com `private_data` |
| `.version` ausente no `obs_source_info` fazia o plugin registrar `audio_capture` em vez de `audio_capture_v1`, e as fontes das cenas existentes viravam `Source ID 'audio_capture_v1' not found` | `.version = 1`, igual aos builds anteriores |
| COM era inicializado no **construtor** do helper (thread que cria, ou seja o worker da fonte) e o `CoUninitialize` pareado podia rodar na thread que destrói o helper — no `obs_module_unload`, a thread principal do OBS, derrubando um `CoInitialize` do próprio OBS. O `capture_thread`, que é quem chama `ActivateAudioInterfaceAsync`, ficava **sem** COM | `CoInitializeEx` (MTA, RAII) dentro de `Capture()`, no thread que realmente usa a API; membro `couninit` removido do helper |
| `ActivateAudioInterfaceAsync` só aceita completion handler *agile*; sem `FtmBase` a chamada falha com `E_ILLEGAL_METHOD_CALL` (`0x8000000E`) | já estava correto (`CompletionHandler` herda `FtmBase`) — documentado porque foi a causa de um falso positivo no diagnóstico |
| `GetCurrentTimestamp()` com `static` não sincronizado e divisão inteira imprecisa | `std::call_once` + conversão sem overflow |

### Interface

- Tradução pt-BR corrigida ("Adicionar a partir de sessões ativas").
- Nenhuma opção removida: continuam disponíveis vários executáveis por fonte,
  modo "excluir", modo hotkey e lista de sessões ativas.

## Compilar

Requisitos: Visual Studio 2022 (Build Tools com C++), CMake 3.28+, OBS instalado
(para gerar a import library do `obs.dll`).

```powershell
.\tools\prepare-sdk.ps1 -ObsVersion 33.0.1
.\tools\build.ps1 -Config Release
```

Saída:

- `build\Release\win-capture-audio.dll`
- `release\` — layout portátil pronto para copiar na raiz do OBS
- `dist\win-capture-audio-2.4.0-win64.zip` (via `tools\package.ps1`)

Para compilar contra o OBS inteiro (CI/padrão upstream), configure com
`-DOBS_SDK_BUILD=OFF` e forneça o pacote `libobs` do OBS.

## Instalar

```powershell
# feche o OBS antes
.\tools\install-local.ps1
```

O script faz backup da DLL anterior (`win-capture-audio.dll.bak-<data>`) e copia
a DLL e os locales. Rodar como administrador quando o OBS está em
`C:\Program Files`.

Instalação manual: copie `release\obs-plugins\64bit\win-capture-audio.dll` para
`<OBS>\obs-plugins\64bit\` e `release\data\obs-plugins\win-capture-audio\` para
`<OBS>\data\obs-plugins\win-capture-audio\`.

## Verificar

Sem abrir o OBS, dá para conferir se o binário carrega no `obs.dll` instalado:

```powershell
.\tools\smoke-test.ps1
```

Ele compila `tools/smoke-test.cpp`, carrega o `obs.dll` do OBS, carrega a DLL do
plugin, confere `obs_module_load`/`obs_module_unload`/`obs_module_ver` e imprime
a `LIBOBS_API_VER` esperada (`33.0.1`).

Dentro do OBS, no log (`%APPDATA%\obs-studio\logs`) deve aparecer:

```
[win-capture-audio] Version 2.4.0 (<hash>)
```

## Status da instalação

- Instalado em `C:\Program Files\obs-studio` em 20/11/2026 (elevado).
- DLL antiga preservada em
  `obs-plugins\64bit\win-capture-audio.dll.bak-20261120-192608`.
- `win-capture-audio.dll` = `368128` bytes, `FileVersion 2.4.0.0`, 15 locales
  copiados.
- Para reverter: feche o OBS, apague a DLL nova e renomeie o `.bak-*` de volta.

## Diagnóstico fora do OBS

O caminho de captura pode ser validado sem abrir o OBS: o plugin usa
`ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)` com
`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`. Um programa mínimo que repete
esse padrão confirma, por exemplo, `ActivateAudioInterfaceAsync`,
`Initialize`, `Start` e a contagem de frames recebidos. Dois detalhes que
custam horas se errados:

1. `CoInitializeEx` **no thread que chama a API** (não no que cria o objeto).
2. O completion handler precisa ser *agile* (`FtmBase`). Sem isso,
   `ActivateAudioInterfaceAsync` retorna `0x8000000E` (`E_ILLEGAL_METHOD_CALL`)
   — que parece problema de permissão ou de versão do Windows, mas não é.

## Relação com a fonte nativa do OBS

Desde o OBS 28 existe a fonte nativa **Captura de Áudio de Aplicativo (BETA)**
(`wasapi_process_output_capture`), que usa a mesma API do Windows
(`ActivateAudioInterfaceAsync` + `AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS`). Para
capturar um único aplicativo, a nativa é a opção mais estável e sem manutenção.

Este plugin continua útil quando você precisa de:

- vários executáveis em uma única fonte;
- modo "excluir" (capturar tudo menos os apps escolhidos);
- modo hotkey (capturar a janela em primeiro plano);
- lista de sessões ativas dentro das propriedades.

## Limitações conhecidas

- Requer Windows 10 2004+ atualizado (mesma limitação da API).
- Processos elevados só são capturados com o OBS também elevado.
- O formato de áudio (canais/taxa) é lido na criação da primeira fonte; mudar a
  configuração de áudio do OBS exige recriar a fonte.
