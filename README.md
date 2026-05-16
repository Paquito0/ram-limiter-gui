# RAM Limiter Pro

Aplicativo Windows (GUI) que monitora e limita o consumo de RAM de processos selecionados, rodando discretamente na bandeja do sistema.

## Funcionalidades

- **Tray icon discreto** — roda na bandeja do sistema, sem ocupar espaço na taskbar
- **Iniciar minimizado** — opção de iniciar direto na bandeja, sem abrir a janela
- **Iniciar com Windows** — integração via Task Scheduler (nível mais alto de privilégio)
- **Lista de processos configurável** — adicione/remova processos pela UI ou editando `config.txt`
- **Trim automático** — usa `SetProcessWorkingSetSize` para liberar memória dos processos monitorados
- **Intervalo configurável** — de 1 a 600 segundos
- **Limite mínimo de memória** — só trimar processos acima de X MB
- **Estatísticas em tempo real** — memória ativa, processos trimados, memória liberada, erros
- **Tema escuro** — interface com cores escuras para menor impacto visual
- **Instância única** — mutex impede múltiplas cópias rodando ao mesmo tempo
- **Zero dependências** — binário standalone, sem runtime necessário

## Requisitos

- Windows 10/11 (x64)
- Privilégios de **Administrador** (obrigatório para `SetProcessWorkingSetSize`)

## Instalação

1. Baixe o `RamLimiter-GUI.exe` da [última release](https://github.com/Paquito0/ram-limiter-gui/releases)
2. Execute como Administrador (clique direito → "Executar como administrador")
3. Na primeira execução, o `config.txt` é criado automaticamente com processos padrão

## Uso

### Interface

| Seção | Descrição |
|-------|-----------|
| **Processos** | Lista de processos monitorados. Use `+ Add` para adicionar, `- Remove` para remover |
| **Intervalo** | Tempo em milissegundos entre cada ciclo de trim (1000–600000) |
| **Min MB** | Só trimar processos que usam mais que X MB de RAM (0 = trimar sempre) |
| **Iniciar com Windows** | Cria tarefa no Task Scheduler para iniciar no login |
| **Iniciar minimizado** | Inicia direto na bandeja, sem mostrar a janela |
| **Salvar** | Salva configurações no `config.txt` |

### Tray Icon

- **Clique esquerdo** — abre a janela
- **Clique direito** — menu com opções "Abrir" e "Sair"
- **Fechar a janela (X)** — esconde, não encerra o app. Use "Sair" no tray para encerrar

## Config.txt

O arquivo `config.txt` fica na mesma pasta do `.exe` e é recarregado automaticamente quando modificado.

```ini
###############################################################
#               RAM Limiter Pro - Configuracao                #
###############################################################

# --- TEMPO DE ATUALIZACAO ---
interval=5000

# --- LIMITE MINIMO DE MEMORIA ---
min_memory=0

# --- INICIAR MINIMIZADO ---
start_minimized=0

###############################################################
#                    LISTA DE PROCESSOS                       #
###############################################################
# Adicione nomes de processos abaixo (um por linha, sem .exe)

discord
chrome
steam
spotify
```

### Formato

| Diretiva | Valor | Padrão | Descrição |
|----------|-------|--------|-----------|
| `interval` | 1000–600000 | 5000 | Milissegundos entre ciclos de trim |
| `min_memory` | 0–10000 | 0 | MB mínimo para trimar (0 = sempre) |
| `start_minimized` | 0 ou 1 | 0 | Iniciar minimizado na bandeja |
| Linhas soltas | texto | — | Nome do processo (sem `.exe`) |
| Linhas com `#` | — | — | Comentários |

## Compilação

### Pré-requisitos

- [MSYS2](https://www.msys2.org/) com toolchain MinGW64

### Build

```powershell
$env:PATH = "C:\msys64\mingw64\bin;C:\msys64\usr\bin;" + $env:PATH
$gcc = "C:\msys64\mingw64\bin\gcc.exe"

# Build GUI
& $gcc -O2 -mwindows -o RamLimiter-GUI.exe ram-limiter-gui.c -lkernel32 -luser32 -lshell32 -ladvapi32 -lpsapi -lcomctl32
```

### Flags

| Flag | Propósito |
|------|-----------|
| `-O2` | Otimização de performance |
| `-mwindows` | App GUI (sem console) |
| `-lpsapi` | `GetProcessMemoryInfo` |
| `-lcomctl32` | System Tray |

## Arquitetura

```
WinMain
  ├── is_admin()          → Verifica privilégios de administrador
  ├── CreateMutexW()      → Garante instância única
  ├── InitializeCriticalSection()
  ├── config_load()       → Carrega config.txt
  ├── CreateWindowEx()    → Cria janela principal
  ├── create_tray_icon()  → Registra ícone na bandeja
  └── WorkerThread        → Loop principal
        ├── config_load() → Recarrega se arquivo mudou (cache por timestamp)
        └── trim_all()    → Trim cada processo na lista
              ├── pid_of_process()   → Encontra PID via Toolhelp32Snapshot
              └── trim_pid()         → SetProcessWorkingSetSize(-1, -1)
```

### Thread Safety

- `g_cs` (Critical Section) protege `g_procs` e `g_procCount`
- Contadores (`g_totalTrimmed`, `g_totalErrors`, `g_totalFreed`) usam `Interlocked*`
- `config_load()` usa cache `g_configLastMod` para evitar reload desnecessário

### Como o Trim Funciona

`SetProcessWorkingSetSize(hProcess, -1, -1)` força o Windows a mover páginas de memória do processo para o arquivo de paginação, liberando RAM física. O processo continua funcionando normalmente — o Windows realoca páginas conforme necessário.

## Estrutura de Arquivos

```
ram-limiter-gui.c    → Código fonte principal (GUI Win32)
main.c               → Versão console (legado, não compilado)
config.txt           → Configurações + lista de processos (gerado auto.)
RamLimiter-GUI.exe   → Binário compilado
version.rc           → Resource file (versão, ícone)
AGENTS.md            → Notas de desenvolvimento
```

## Changelog

### v1.0.0

- GUI com tema escuro e system tray
- Lista de processos configurável via UI e config.txt
- Iniciar com Windows (Task Scheduler)
- Iniciar minimizado na bandeja
- Estatísticas em tempo real
- Mutex de instância única
- `GetTickCount64` para evitar overflow em 49 dias
- Proteção contra `g_procCount` negativo ao remover

## Licença

Uso pessoal.
