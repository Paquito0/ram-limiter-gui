# RAM Limiter Pro

Aplicativo Windows que reduz o consumo de RAM de processos específicos (Discord, Chrome, Steam, Spotify, etc.) fazendo trim do working set periodicamente.

## Como funciona

O app monitora uma lista de processos definida em `config.txt` e chama `EmptyWorkingSet` em cada um a cada N milissegundos, forçando o Windows a descartar paginas de memoria nao usadas. O Windows pode recarrega-las sob demanda depois.

## Requisitos

- Windows 7 ou superior
- Privilegios de administrador (obrigatorio para `EmptyWorkingSet` em outros processos)

## Compilacao

**Compilador:** MSYS2 MinGW64 em `C:\msys64\mingw64\bin`

```powershell
$env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;" + $env:PATH
$windres = "C:\msys64\mingw64\bin\windres.exe"
$gcc = "C:\msys64\mingw64\bin\gcc.exe"

# Compila o recurso de versao
& $windres version.rc -O coff -o version.res

# Build GUI
& $gcc -O2 -mwindows -o RamLimiter.exe RamLimiter.c version.res -lkernel32 -luser32 -lshell32 -ladvapi32 -lpsapi -lcomctl32
```

## Uso

1. Execute `RamLimiter.exe` como administrador
2. O arquivo `config.txt` e criado automaticamente na primeira execucao
3. Adicione processos pelo botao "+ Add" ou edite `config.txt` manualmente
4. O icone aparece na system tray (clique direito para abrir/sair)
5. Marque "Iniciar com Windows" para rodar automaticamente no logon

## Configuracao (`config.txt`)

```
interval=5000        # ms entre trims (1000-600000)
min_memory=0         # MB minimo para trimar (0 = sempre trima)
start_minimized=1    # iniciar minimizado no tray (0/1)

# processos abaixo, um por linha, sem .exe
discord
chrome
steam
spotify
```

Linhas comecando com `#` sao ignoradas.

## Arquitetura

| Arquivo | Proposito |
|---------|-----------|
| `RamLimiter.c` | Codigo fonte (Win32 API + system tray) |
| `version.rc` | Recurso de versao do executavel |
| `version.res` | Build artifact do version.rc (gerado) |
| `config.txt` | Configuracao (gerado na 1a execucao) |
| `LICENSE` | Licenca MIT |
| `AGENTS.md` | Instrucoes para o opencode (IA) |

## Fluxo principal

1. `WinMain` → verifica admin → `config_load()` → cria janela → `create_tray_icon()` → `WorkerThread`
2. `WorkerThread`: loop com `config_load()` → `trim_all()` → sleep
3. `trim_all()`: para cada processo, encontra PID via `CreateToolhelp32Snapshot` e chama `EmptyWorkingSet`
4. UI atualiza a cada 1s via `WM_TIMER` → `update_display()`

## Licenca

MIT - veja [LICENSE](LICENSE).
