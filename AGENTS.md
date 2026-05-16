# RAM-Limiter-Pro

## Compilacao

**Compilador:** MSYS2 MinGW64 em `C:\msys64\mingw64\bin`

```powershell
$env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;" + $env:PATH
$gcc = "C:\msys64\mingw64\bin\gcc.exe"

# Build GUI (Win32 API) - principal
& $gcc -O2 -mwindows -o RamLimiter-GUI.exe ram-limiter-gui.c -lkernel32 -luser32 -lshell32 -ladvapi32 -lpsapi -lcomctl32

# Build console (legacy)
& $gcc -O2 -mwindows -o RamLimiter-Pro.exe main.c -lkernel32 -luser32 -lshell32 -ladvapi32 -lpsapi
```

- `-mwindows` = App GUI (sem console)
- `-lpsapi` para `GetProcessMemoryInfo`
- `-lcomctl32` para System Tray

## Peculiaridades importantes

- **Config parsing:** Usa `fopen`/`fgets` (char/ASCII) com `WideCharToMultiByte`/`MultiByteToWideChar` (CP_UTF8) para converter entre WCHAR da UI e char do arquivo. **Nao use `_wfopen_s`/`fwprintf`** — causam corrupcao de caracteres no MinGW.
- **config.txt:** Criado automaticamente ao lado do `.exe` via `GetModuleFileNameW`. Se nao existir, cria com defaults (discord, chrome, steam, spotify).
- **Admin obrigatorio:** O app requer privilegios de administrador para `SetProcessWorkingSetSize`. Se nao for admin, mostra MessageBox e sai.
- **config_load() com cache:** Usa `g_configLastMod` (timestamp do arquivo) para evitar recarregar a cada ciclo do WorkerThread. Sem isso, a lista em memoria e resetada a cada 5s.
- **Iniciar com Windows:** Usa Task Scheduler (`schtasks /Create /TN "RAMLimiterPro" /SC ONLOGON /RL HIGHEST /F`) — nao usa registro do Windows. Checkbox na UI liga/desliga.

## Config.txt formato

```
interval=5000        # ms entre trims (1000-600000)
min_memory=0         # MB minimo para trimar (0-10000)
# linhas com # sao comentarios
discord              # nome do processo sem .exe
steam
```

## Arquitetura

| Arquivo | Proposito |
|---------|-----------|
| `ram-limiter-gui.c` | Codigo fonte GUI (Win32 API) — principal |
| `main.c` | Codigo fonte console (legacy) |
| `RamLimiter-GUI.exe` | Build GUI com system tray |
| `config.txt` | Lista de processos + configuracoes |
| `Program.cs` / `RamLimiter.csproj` | Versao antiga C# (referencia, nao usar) |

## Fluxo principal

1. `WinMain` → verifica admin → `config_load()` → cria janela → `create_tray_icon()` → `WorkerThread`
2. `WorkerThread`: loop com `config_load()` → `trim_all()` → sleep (intervalo)
3. `trim_all()`: para cada processo em `g_procs`, encontra PID via `CreateToolhelp32Snapshot`, chama `SetProcessWorkingSetSize(hp, -1, -1)`
4. UI atualiza a cada 1s via `WM_TIMER` → `update_display()`
