# Ponte — guia para IA

Biblioteca de jogos no Mac que copia jogos pela rede local para um PS5 com homebrew habilitado e um Switch com Atmosphère. O Mac só serve; o console sempre inicia a conexão e puxa. Não é jogar pela rede: depois da cópia o jogo fica no console.

## Fonte da verdade

- **`protocol/SPEC.md`** define formatos, rotas, erros e o ciclo de vida dos itens. Leia as seções relevantes antes de mexer em qualquer lado (Mac, PS5, Switch ou referência). Se o código e a SPEC discordarem, a SPEC vence, ou a SPEC é corrigida primeiro.
- **`reference/`** é o gabarito executável da SPEC em Python. O servidor Swift tem que se comportar igual a ele; `tools/conformance.py` testa os dois pela rede.
- **`profiles/*.json`** guardam o que funciona em cada firmware. Nada que dependa de firmware ou de versão de montador vai fixo no código.

## Mapa

```
protocol/SPEC.md        especificação do protocolo v1 (rascunho 4), oficial, em português
protocol/SPEC.en.md     tradução em inglês da SPEC (só acompanha a oficial)
reference/ponte_ref/    referência em Python: identificador, biblioteca, estado, plano exFAT, servidor HTTP/UDP
reference/tests/        testes (pytest) com arquivos sintéticos
tools/fake_client.py    cliente falso que finge ser um console
tools/conformance.py    teste caixa-preta da SPEC contra qualquer servidor (--server python|swift)
tools/check_docs.py     confere a documentação: pares inglês/português, links e âncoras
profiles/               compatibilidade por firmware (ps5-13.60.json)
mac/                    Swift Package (macOS 13+): ver abaixo
ps5/                    payload em C (SDK de payloads do PS5) com interface web na porta 47102
switch/app/             app Ponte do Switch (ponte.nro): motor do Sphaira (GPLv3) + tela do Ponte; ver switch/app/PONTE.pt-BR.md
switch/hosttest/        núcleo C++ do protocolo do app, testado no PC contra a referência
docs/                   índice da documentação, padrão visual, revisão, notas de versão
LEGAL.md, LICENSE       aviso legal e licença (0BSD; switch/app/ é GPLv3)
.github/workflows/      mac-ps5.yml ("Mac e PS5"): referência, Swift no macOS, payload; artefatos Ponte-Mac e Ponte-PS5
                        switch.yml ("Switch"): hosttest e ponte.nro; artefato Ponte-Switch
```

Dentro de `mac/Sources/`:

| Alvo | O quê |
|---|---|
| `PonteCore` | identificador de formatos, biblioteca, perfis, estado do console, plano do `.exfat`. Só Foundation; compila no Linux. |
| `PonteServer` | HTTP/1.1 com sockets POSIX, descoberta UDP, pareamento, rotas, e o `Preparer` (extrair compactados, converter pasta → `.exfat`). Compila no Linux. |
| `ponte-server` | servidor de linha de comando, mesmas opções da referência. |
| `PonteMac` | app SwiftUI (janela + barra de menu): `AppModel.swift` (estado e ações), `Views.swift`, `PonteMacApp.swift`. Só macOS. |

O cliente do Switch é o app **Ponte** em `switch/app/` (app próprio, não o Sphaira). Ele usa o Sphaira como motor e por isso é GPLv3. O núcleo do protocolo (`switch/app/sphaira/source/utils/ponte_proto.cpp`) não depende do libnx e é testado por `switch/hosttest/`. O fork `souzaq001/sphaira` (branch `mac-library`) foi o primeiro passo e não é mais usado.

## Branches

- Tudo vive na `main`. Os antigos `mac-app` e `ps5-velocidade` (app do Mac e payload do PS5) foram mesclados nela. Trabalho maior pode ir num branch curto, mesclado de volta assim que o CI passar.
- O CI roda em todo push; o build do macOS (que compila o `PonteMac`) só existe lá, e o `ponte.nro` só é gerado no `switch.yml`. Sem Swift ou devkitPro na máquina, confira o resultado no GitHub Actions antes de dar algo como pronto.

## Comandos

```sh
python3 -m pytest reference/tests -q                     # referência (só stdlib + pytest, Python 3.10+)
(cd mac && swift build && swift test)                    # núcleo, servidor e app
python3 tools/conformance.py --server swift              # protocolo contra o servidor Swift (precisa do swift build)
python3 tools/conformance.py --server python             # o mesmo contra a referência
(cd mac && ./scripts/build-app.sh)                       # gera mac/build/Ponte.app
make -C ps5 host && python3 ps5/tests/test_payload.py    # payload (versão de teste) contra a referência
make -C switch/hosttest test                             # cliente Switch no PC contra a referência
cd reference && python3 -m ponte_ref.server --library /Volumes/SSD/Jogos --no-pairing
python3 tools/fake_client.py discover | pair <código> | catalog | download <id> --dest ./console-falso | state
python3 tools/check_docs.py                              # documentação: pares, links e âncoras
```

## Estado atual

- [x] Protocolo v1 (rascunho 4)
- [x] Referência em Python, cliente falso e teste de conformidade
- [x] Servidor do Mac em Swift, passando na conformidade
- [x] App do Mac: biblioteca, extração, conversão → `.exfat`, pareamento, atenção, cópias em andamento, histórico, ejetar o SSD
- [ ] App do Mac testado num Mac de verdade (conversão com `hdiutil`, permissão de rede local, ejetar)
- [x] Payload PS5: interface web, cópia com retomada para `homebrew/`, `.pkg` pelo instalador do sistema; ícone na tela inicial; pausa/retomada automática com fila guardada em `/data/ponte/queue.json`; velocidade, avisos a cada 25% e diagnóstico de velocidade; depois de cada cópia pede ao ShadowMountPlus uma varredura nova (`mounter_rescan`, API local `127.0.0.1:10101`)
- [ ] Payload PS5 testado num console de verdade
- [x] App Ponte do Switch (`switch/app`): escrito e testado no PC contra a referência; durante a instalação a tela apaga (Y) e o app não é suspenso no menu HOME (`App::SetRunInBackground`, só no modo aplicativo); não pula "já instalado" pelo ncm e, se o yati não instalar nada, o item termina como `failed` com o motivo
- [ ] App do Switch testado no console (tela, leitura do instalado via ns/ncm, instalação pelo yati, cópia com o menu HOME aberto)

Em aberto na SPEC (§9): ler metadados de dentro de `.exfat`/`.ffpkg`/`.ffpfs` sem arquivo lateral, metadados de XCI com `prod.keys`, carregador de pastas de PS4, registrar jogos direto pelo payload.

## Como o app do Mac funciona (resumo)

- `AppModel` guarda os ajustes em `UserDefaults`, cria um `PonteServer` por biblioteca (`restart()`) e atualiza um retrato para a interface a cada segundo (`refresh()`).
- Estado persistente (id do servidor, tokens, cache de hashes, histórico de cópias, perfis) fica em `~/Library/Application Support/Ponte/`, nunca no SSD.
- Cópias: `TransferTracker` (`PonteServer/Transfers.swift`) mede o que o servidor envia, pelos avisos `TransferHooks` do `HTTPServer`. Dá `currentTransfers` (progresso, velocidade, arquivo atual) e `history` (`history.json`). Regras na SPEC §7.6.
- Ejetar: `PonteServer.activeTransfers()` diz o que está sendo copiado (envio agora, ou nos últimos 30 s sem `report`; vem do mesmo `TransferTracker`). Com cópias, o app pergunta: esperar terminar, ejetar agora ou cancelar. `stopAndWait()` fecha conexões, interrompe o preparo (desanexando a imagem de conversão) e para varredura e hashes; depois `diskutil eject`. O servidor volta sozinho quando o disco é montado de novo. Ejetar por fora (Finder, `diskutil`) passa pelo `DiskWatcher` (Disk Arbitration): aprova a desmontagem só depois de `stopAndWait()`; se o disco não sair em 20 s, volta a servir. Não muda o protocolo: para o console é o mesmo que o Mac sair do ar.

## Regras de trabalho

- Mudou o protocolo → atualize `protocol/SPEC.md` **e** o histórico dela (§10), depois a referência e os testes, depois o servidor Swift e os clientes (PS5 e Switch). `tools/conformance.py` tem que continuar passando nos dois servidores.
- Mudou a referência → rode `python3 -m pytest reference/tests -q` antes de commitar. Formato novo ganha teste com arquivo sintético.
- `reference/` usa só a biblioteca padrão do Python. `PonteCore` e `PonteServer` só usam Foundation e têm que compilar no Linux (use `#if os(macOS)` para o que for do macOS).
- Nunca grave direto no destino final: cópias vão para `*.ponte-tmp` ou `ponte/tmp/` e só são renomeadas no fim (SPEC §7.4 e §8). No Switch, o instalador grava em placeholders do sistema (SPEC §8.1).
- Código e nomes de código em inglês; comentários e textos da interface em português.
- Documentação em duas línguas: `NOME.md` em inglês (o que o GitHub mostra primeiro) e `NOME.pt-BR.md` em português, com o seletor de idioma no topo. Mudou uma, mude a outra no mesmo commit. Exceções: a SPEC (oficial em português em `protocol/SPEC.md`, tradução em `protocol/SPEC.en.md`), este `CLAUDE.md` (só português) e as notas em `docs/releases/` (as duas línguas no mesmo arquivo). `python3 tools/check_docs.py` confere pares, links e âncoras; o CI roda.
- Linguagem neutra na documentação: o Ponte copia o que já está na biblioteca do usuário. Não cite fontes de jogos, programas de download nem termos de pirataria; não inclua chaves, firmware, partes de SDK proprietário, exploits, arte de jogos comerciais ou marcas de terceiros (ver `LEGAL.md`).
- Itens marcados em "Em aberto" (SPEC §9) não devem ser resolvidos por suposição; proponha e confirme.
- Atualize a seção "Estado atual" deste arquivo, o checklist do `README.md` e do `README.pt-BR.md` e o README da parte que mudou (`mac/`, `ps5/` ou `switch/`, nas duas línguas) quando algo mudar para o usuário.

## Ambiente de referência

- PS5 no firmware 13.60 com homebrew habilitado (kstuff → ShadowMountPlus → etaHEN, nenhum deles incluído aqui); o payload Ponte entra no fim da cadeia. Caminho principal: copiar `.exfat`, `.ffpkg` ou pasta para `<destino>/homebrew/`; o ShadowMountPlus monta e registra. `.pkg` de PS4 instala; `.pkg` de PS5 está bloqueado nesse firmware.
- Switch 1 com Atmosphère; o app Ponte (`ponte.nro`) é o cliente.
- Biblioteca num SSD externo no Mac. Uso pessoal, rede local, sem TLS.
