# Payload do PS5

[English](README.md) · **Português**

Payload em C (ELF) para o [SDK de payloads do PS5](https://github.com/ps5-payload-dev/sdk). Ele fala com o Mac pelo [protocolo Ponte](../protocol/SPEC.md) e mostra uma **interface web** onde você escolhe o que copiar:

```
http://<ip-do-ps5>:47102
```

No PS5, o jeito mais fácil é o **ícone "Ponte" na tela inicial**: ao subir, o payload cria uma pasta mínima em `/data/ponte/tile/PNTE00001/` e registra no sistema (copia `sce_sys` para `/user/app` e `/user/appmeta`, liga a pasta em `/system_ex/app` e chama o instalador, como fazem outros payloads de 13.60). Abrir o ícone abre o navegador do PS5 direto na interface. Também dá para abrir no celular ou no computador, no endereço que a notificação mostra quando o payload sobe.

O ícone só funciona enquanto o payload estiver rodando (é ele que serve a página).

## O que faz

- **Acha o Mac sozinho** (descoberta UDP na porta 47100). Com mais de um Mac, ou se a rede bloquear a descoberta, você escolhe ou digita o IP na interface.
- **Pareamento** com o código de 6 dígitos do app do Mac, uma vez só (o token fica em `/data/ponte/config.json`).
- **Catálogo** do Mac com o que já está no console: `novo`, `no console`, `atualização disponível`, `incompatível` (perfil de firmware). Agrupado por jogo (base, atualização, DLC), com ícones.
- **Cópia** para o armazenamento escolhido (interno, M.2/estendido ou USB), uma de cada vez:
  - pastas e imagens (`.exfat`, `.ffpkg`, `.ffpfs`, `.ffpfsc`) vão primeiro para `<armazenamento>/ponte/tmp/`, que o ShadowMountPlus não vigia, e só no fim são movidas para `<armazenamento>/homebrew/`, de onde ele monta e registra;
  - queda de conexão ou cancelamento: tentar de novo continua de onde parou (`Range`), desde que o item não tenha mudado no Mac;
  - cada arquivo é conferido por tamanho e `sha256` antes de ir para `homebrew/`;
  - destino já ocupado: a interface pergunta se substitui;
  - `.pkg`: entregue ao instalador do sistema pela URL do Mac, que baixa direto (sem ocupar espaço em dobro). A instalação continua em segundo plano no console.
- **Pausa e retomada sozinha:** se o Mac sair do ar (desligou, dormiu, a rede caiu) ou o USB de destino ainda não estiver montado, a cópia fica **pausada** e continua sozinha (a cada 15 s). A fila fica guardada em `/data/ponte/queue.json`: desligou o console no meio, na próxima vez que o payload subir ele retoma o que estava pendente, sem você abrir a página. **Pausar** e **Continuar** na página; o que você pausou continua pausado até você mandar continuar. **Cancelar** tira da fila (o que já veio fica guardado: copiar de novo continua de onde parou).
- **Progresso de cada etapa**, no padrão visual do Ponte ([docs/DESIGN.md](../docs/DESIGN.pt-BR.md)): título e percentual, barra, `1,2 GB de 4,5 GB · 38 MB/s · faltam 2 min` e uma linha com a etapa e o arquivo (`copiando · arquivo 3 de 120: data/big.bin`). As etapas são: consultando o Mac, conferindo o que já tinha (numa retomada), copiando, conferindo, movendo para `homebrew/` (inclusive apagar a cópia antiga ao substituir) e entregando ao instalador. Etapas sem tamanho mostram a barra indeterminada. A conferência também mostra velocidade e atende Pausar/Cancelar. No topo, um resumo da fila: itens, quanto falta, velocidade somada e tempo.
- **Velocidade**: média dos últimos ~5 s; parado há mais de 2 s mostra 0. Um aviso discreto na tela a cada 25% (com a velocidade), que dá para desligar na página.
- **Uma conexão para o item inteiro (keep-alive):** todos os arquivos de um item vêm pela mesma conexão com o Mac, e a thread de rede fica viva a cópia toda. Em pastas com muitos arquivos pequenos isso evita abrir uma conexão TCP por arquivo. Se a conexão cair entre um arquivo e outro, o payload reconecta sozinho.
- **Rede e disco em paralelo:** uma thread lê do Mac e enche buffers inteiros (4 × 4 MB por padrão); outra confere o `sha256` e grava em blocos grandes. O `sha256` usa as instruções SHA do processador do PS5 (SHA-NI) quando existem, cerca de 5× mais rápido que o código C (~1,2 GB/s contra ~220 MB/s numa máquina de teste), e por isso não segura mais a cópia. Cada arquivo leva um `fsync` antes de ir para `homebrew/`.
- **Medições:** cada cópia mostra velocidade da rede, do disco e do hash (e se usou SHA-NI), tempo esperando cada um, buffer TCP efetivo, qual foi o gargalo e quantas conexões e requisições usou. O mesmo texto vai para o log.
- **Arquivo que chega diferente do Mac** (`sha256` não confere): é apagado e baixado de novo do zero, uma vez, sozinho. Se falhar de novo, a cópia para e diz para conferir a rede e o disco.
- **Mac que some no meio:** 20 s sem dados e a cópia pausa e tenta de novo sozinha.
- **Report** de cada cópia para o Mac (aparece em "Últimas cópias" no app).

## Diagnóstico de velocidade

Na página, a seção **Diagnóstico de velocidade** mede cada parte separada (até 15 s por teste), com barra e velocidade ao vivo enquanto roda. O resultado aparece numa tabela:

- **Testar rede:** baixa o maior item do catálogo do Mac e descarta (sem hash, sem disco), com o buffer TCP dos ajustes, com o padrão do sistema e com leituras de 64 KB.
- **Testar disco:** grava no armazenamento escolhido em "Copiar para" (em `ponte/tmp/`, apagado no fim) com blocos de 16 KB, 256 KB e 4 MB.

Os mesmos testes pela URL, para usar de outro aparelho:

```
http://<ip-do-ps5>:47102/api/diag/net?item=<id>&mb=1024&read_kb=4096&rcvbuf_kb=4096
http://<ip-do-ps5>:47102/api/diag/disk?storage=usb0&mb=1024&block_kb=4096
```

Progresso do teste em andamento (o que a página usa para a barra): `GET /api/diag/progress` → `{running, kind, label, done, total, speed, elapsed_s}`.

Ajustes para testar sem recompilar (na mesma seção, ficam em `/data/ponte/settings.json`): **Buffer TCP** (`rcvbuf_kb`, 0 = padrão do sistema; se o sistema recusar o valor, o payload tenta a metade até 256 KB) e **Bloco da cópia** (`chunk_kb`).

Montador: depois de mover um jogo para `homebrew/`, o payload pede ao ShadowMountPlus uma varredura nova (`POST /api/v1/scan` com `{"reset_attempts":true}`, na API local dele, que vem ligada no ShadowMountPlus 1.7). Ele já varre sozinho a cada ~15 s; o pedido só adianta e zera as tentativas que tinham falhado (útil ao substituir um jogo). O endereço fica em `mounter_rescan` (padrão `127.0.0.1:10101/api/v1/scan`; vazio = não pede), editável em **Cópias → Montador (ShadowMountPlus)**. Se o montador não responder, a cópia continua valendo e o motivo vai para `/data/ponte/ponte.log`.

## Interface

Feita para a TV (1920×1080, controle do PS5) e para o celular: letras e botões maiores na TV (botões com pelo menos 48 px de altura), biblioteca e cópias lado a lado em telas largas, uma coluna no celular. A página atualiza a lista de cópias sem refazê-la, então o foco do controle não salta de Pausar/Cancelar. Ela pergunta antes de Cancelar, Substituir e "Outro Mac" (que apaga o pareamento), e para de consultar o payload enquanto está em segundo plano. Os erros dizem o que aconteceu e o que fazer.

### Cópias em `/api/status`

Cada item de `jobs` tem os campos de antes (`seq`, `item_id`, `title`, `kind`, `content`, `storage`, `state`, `done`, `total`, `speed`, `eta_s`, `message`, `manual_pause`, `stats`, `error`) e mais:

| Campo | O quê |
|---|---|
| `phase` | etapa em andamento: `query`, `verify_partial`, `download`, `verify`, `move`, `install`; `null` fora do andamento |
| `phase_done`, `phase_total` | bytes da etapa (no download, os mesmos de `done`/`total`); `null` quando a etapa não tem tamanho |
| `phase_speed` | velocidade da etapa em B/s (hash na conferência, rede no download) |
| `file`, `file_index`, `file_count` | arquivo atual e a posição dele no item (1 a n) |
| `started_ms` | quando a cópia começou (epoch, ms) |
| `connections`, `requests` | conexões TCP e requisições ao Mac nesta cópia |

`speed` e `eta_s` usam a média dos últimos ~5 s (0/`null` se nada chegou nos últimos 2 s). Durações, velocidade e esperas usam o relógio monotônico, que não muda se o relógio do console for acertado.

## Instalar no PS5

1. Baixe o `ponte.elf` da última execução do CI (aba **Actions** do GitHub → workflow **Mac e PS5** → execução mais recente da `main` → **Artifacts** → `Ponte-PS5`), ou compile (abaixo).
2. Para testar uma vez: envie para o carregador de ELF do etaHEN (porta 9021):
   ```sh
   socat -t 99999999 - TCP:<ip-do-ps5>:9021 < ponte.elf
   ```
3. Para ficar de vez: coloque o `ponte.elf` no autoload, **depois do etaHEN**, do mesmo jeito que os outros payloads da sua cadeia (kstuff → ShadowMountPlus → etaHEN → Ponte). Ele roda em segundo plano até o console desligar; se já estiver rodando, uma segunda cópia avisa e sai.

O payload não precisa de nada além do que o ambiente de homebrew já carrega. A configuração fica em `/data/ponte/`.

## Compilar

Precisa do SDK instalado (`PS5_PAYLOAD_SDK`) e de `clang`/`lld` 15 ou mais novos:

```sh
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
make                     # build/ponte.elf
make send PS5_HOST=192.168.0.20
```

## Testar sem console

`make host` compila o mesmo código para Linux/macOS, trocando só a camada do console (`src/platform_host.c`): o "console" vira uma pasta, e o instalador de `.pkg` falso baixa a URL como o do PS5 faria. O teste sobe o servidor do Mac, o payload, e faz tudo pela interface web: achar o Mac, parear, catálogo, cópia de pasta e imagem, queda e retomada, destino ocupado, item que mudou no meio, `.pkg` e reinício.

`make host` também compila `build/sha256-selftest`, que confere o `sha256` com SHA-NI contra o código C (vetores conhecidos e centenas de tamanhos aleatórios, inteiros e em pedaços) e mede os dois. O teste de ponta a ponta roda ele primeiro.

```sh
make host
make selftest                                   # só o sha256 (confere e mede)
python3 tests/test_payload.py                   # contra a referência em Python
python3 tests/test_payload.py --server swift    # contra o servidor Swift (compile mac/ antes)
```

## Organização

| Arquivo | O quê |
|---|---|
| `src/main.c` | inicialização, interface web (rotas `/api/...`), vigia do Mac |
| `src/ponte.c` | cliente do protocolo: descoberta, pareamento, chamadas JSON, configuração |
| `src/jobs.c` | fila de cópias, retomada, conferência, mover para `homebrew/`, instalar `.pkg`, report; rede e disco em threads separadas, com medições |
| `src/diag.c` | diagnóstico de velocidade: rede e disco separados |
| `src/http.c` | HTTP/1.1 mínimo (cliente com conexão persistente, e servidor) |
| `src/platform_ps5.c` | notificações, armazenamentos, jogos instalados, firmware, `sceAppInstUtil` |
| `src/platform_host.c` | o mesmo, para testes num computador |
| `src/util.c` | arquivos, relógio, velocidade, `sha256` (C e SHA-NI) |
| `ui/index.html` | a interface (embutida no payload na compilação) |
| `tests/sha256_selftest.c` | confere e mede o `sha256` |
| `src/vendor/cJSON.*` | [cJSON](https://github.com/DaveGamble/cJSON) (MIT) |

## Limitações conhecidas

- O que está instalado vem de `/user/app`, `/user/appmeta`, `/user/patch` e `/user/addcont`, mais o que estiver em `homebrew/`. DLC aparece sem versão (conta como instalado se o título tiver algum DLC).
- Imagens em `homebrew/` não têm versão no console; uma imagem nova do mesmo jogo aparece como "no console".
- O caminho SHA-NI foi conferido num PC com SHA-NI, não num PS5. Ele só entra se o `CPUID` disser que o processador tem as instruções; senão fica o código C.
- Ainda não testado num PS5 de verdade: as chamadas do sistema (notificação, `sceAppInstUtilInstallByPackage`) seguem o que outros payloads de 13.60 usam.
