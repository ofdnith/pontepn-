# O que é este diretório

[English](PONTE.md) · **Português**

Código do app **Ponte** para o Switch. É o [Sphaira](https://github.com/ITotalJustice/sphaira) (GPLv3, de ITotalJustice e colaboradores) no commit `338348e74b6d278bc570a9be5373d25197bf6c8d`, com a tela do Ponte e os ajustes abaixo. O Sphaira entra como motor: instalador (yati), interface, rede e tradução. A licença continua GPLv3 (`LICENSE`).

## Acrescentado

| Arquivo | O quê |
|---|---|
| `sphaira/include/utils/ponte_proto.hpp`, `sphaira/source/utils/ponte_proto.cpp` | protocolo Ponte v1 em C++ puro (JSON com yyjson, agrupamento, ordem de instalação). Sem libnx: compila e é testado no PC por `../hosttest`. |
| `sphaira/include/utils/ponte.hpp`, `sphaira/source/utils/ponte.cpp` | Macs salvos (`/config/sphaira/ponte.ini`), descoberta UDP, chamadas HTTP, inventário do console (ns/ncm). |
| `sphaira/include/ui/menus/ponte.hpp`, `sphaira/source/ui/menus/ponte.cpp` | a tela. |
| `assets/ponte/icon.jpg` | ícone do app. |

## Mudado em relação ao Sphaira

- `sphaira/source/main.cpp`: abre direto na tela do Ponte, sem o menu do Sphaira.
- `sphaira/CMakeLists.txt`: nome **Ponte**, versão própria (`PONTE_VERSION`), ícone, saída `ponte.nro`, define `PONTE_STANDALONE` (na tela: + sai, ZR abre as opções). A versão do Sphaira (`sphaira_VERSION`) fica como está: é a do motor.
- `CMakePresets.json`: preset **Ponte**, igual ao "Lite" do Sphaira mas com NSZ.
- `sphaira/source/yati/source/http.cpp` (+ `.hpp`): construtor com cabeçalhos extras (Bearer, `X-Ponte-*`); leituras de 512 KB do socket e buffer de 8 MB (abaixo).
- `sphaira/include/utils/devoptab_common.hpp`, `sphaira/source/utils/devoptab_common.cpp`: `PushPullThreadData::UseLargeBlockingBuffer()`. No Sphaira o buffer entre o curl e o instalador tem 64 KB e, cheio, **pausa** o curl, que só volta no próximo aviso de progresso; nesse meio tempo a janela TCP fecha e o Mac para de mandar. Com a opção (só o download do Ponte), o buffer tem 8 MB e a escrita espera espaço em vez de pausar. O já lido sai do começo do buffer de vez em quando, não a cada leitura. As montagens de rede do Sphaira continuam como antes.
- `sphaira/source/main.cpp`: buffer TCP de recepção começa em 1 MB (antes 64 KB) no modo aplicativo.
- `sphaira/source/ui/progress_box.cpp`: bytes feitos e total, velocidade em média móvel e sem o "0 segundos restantes" quando a velocidade ainda é zero.
- `assets/romfs/themes/ponte_theme.ini` + `sphaira/include/app.hpp`, `sphaira/source/app.cpp`: tema com as cores da marca, padrão do Ponte; quem tinha o tema padrão do Sphaira salvo passa para ele.
- `sphaira/source/yati/yati.cpp`: avisa a tela quando algo é instalado.
- `sphaira/include/app.hpp`: `App::SetRunInBackground()` (contagem de referências): enquanto o Ponte instala, `appletSetFocusHandlingMode(NoSuspend)` e o menu HOME não congela o app; no fim volta a `SuspendHomeSleep`. Só no modo aplicativo.
- `sphaira/source/app.cpp` (`App::Loop`): fora de foco não desenha e dorme 50 ms por volta (sem o vsync o laço giraria sem parar).
- `sphaira/source/ui/progress_box.cpp` (+ `.hpp`): linha de dica opcional acima dos botões; velocidade calculada pelo tempo real entre amostras (fora de foco a caixa não é desenhada).
- `sphaira/source/ui/menus/main_menu.cpp`: registra a tela (só vale se o menu do Sphaira for usado).
- `assets/romfs/i18n/pt.json`: textos em português.
- `.github/` do Sphaira removido; o CI é o `.github/workflows/switch.yml` deste repositório.

## Configuração e o menu de homebrew

Desde a 0.1.1 o Ponte guarda as opções em `/config/ponte/` (`config.ini`, `ponte.ini`), separadas das do Sphaira. Temas, traduções extras e cache do motor continuam em `/config/sphaira/` e `/switch/sphaira/cache/`.

**O Ponte nunca mexe no `/hbmenu.nro`.** A 0.1.0 herdava a configuração do Sphaira e, se ali estivesse ligada a opção "Replace hbmenu on exit", ao sair copiava o `ponte.nro` por cima do menu de homebrew: o menu de homebrew passava a abrir o Ponte. Na 0.1.1:
- `App::GetReplaceHbmenuEnable()` sempre responde `false` e a opção some das configurações;
- o bloco de `~App()` que copia o app para `/hbmenu.nro` não é compilado;
- se o Ponte perceber que está rodando como `/hbmenu.nro`, oferece restaurar o menu de homebrew (de `/switch/hbmenu.nro`, o backup que o Sphaira faz, ou de `/switch/sphaira/sphaira.nro`).

## Ainda não feito (proposto)

- Ler o arquivo em ordem (`IsStream()`): evitaria reconectar quando o instalador pula para o `.cnmt.nca` no fim do NSP, mas desliga "pular o que já está instalado" e outras conversões do yati. Fica como está.
- Progresso da fila inteira (bytes de todos os itens) e velocidade medida na rede, não na gravação (no NSZ a gravação é maior que o que chega).

## Atualizar o motor

Para trazer uma versão nova do Sphaira: aplique as mudanças acima sobre o novo código (o diff entre `338348e` e este diretório) e rode `make -C ../hosttest test` e o CI.
