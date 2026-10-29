# Ponte

[English](README.md) · **Português**

Biblioteca de jogos no Mac, cópia pela rede local para o PS5 e o Switch.

Seus jogos ficam numa pasta de um SSD externo ligado ao Mac. Quando quiser, você liga o console, abre o cliente Ponte, vê o que tem no Mac e o que já está no console, escolhe o que copiar e para onde. O jogo é copiado e fica no console; nada roda pela rede durante o jogo.

Formatos: PS5 em pasta, `.ffpkg`, `.exfat`, `.ffpfs`, `.ffpfsc` e `.pkg` (PS5 e PS4, incluindo atualizações e DLC); Switch em `.nsp`, `.nsz`, `.xci` e `.xcz`. Compactados (`.zip`, `.rar`, `.7z`) você extrai por conta própria dentro da biblioteca; extração automática e conversão de pastas de PS5 para `.exfat` existem como opções nos Ajustes do Mac, desligadas por padrão. Lista completa em [`protocol/SPEC.md`](protocol/SPEC.md#1-formatos-suportados).

> O Ponte só copia arquivos que já estão na sua biblioteca. Ele não traz jogos, chaves, firmware nem ferramentas para desbloquear consoles, e não tem relação com a Sony nem com a Nintendo. Use com jogos e conteúdo que você tem o direito de usar. Detalhes em [`LEGAL.pt-BR.md`](LEGAL.pt-BR.md).

## Como funciona

```
 Mac: SSD ─► observa ─► extrai ─► identifica ─► converte ─► catálogo
                                                              ▲
                              HTTP, porta 47101 (o console puxa)
                                                              │
   PS5 (payload Ponte) ── copia para homebrew/ ou instala .pkg ┤
   Switch (app Ponte) ───────── instala nsp/nsz/xci/xcz ────────┘
```

O console sempre inicia a conexão; o Mac só serve. Portas: `47100` (descoberta UDP), `47101` (HTTP do Mac), `47102` (interface web do payload no PS5). Detalhes em [`protocol/SPEC.md`](protocol/SPEC.md).

## As partes

| Parte | Pasta | O que é | Como instalar / usar |
|---|---|---|---|
| **App do Mac** | [`mac/`](mac/README.pt-BR.md) | App em Swift/SwiftUI (janela + barra de menu) que observa o SSD, extrai, identifica, converte e serve o catálogo | [`mac/README.pt-BR.md`](mac/README.pt-BR.md) |
| **Payload do PS5** | [`ps5/`](ps5/README.pt-BR.md) | ELF em C com interface web; copia para `homebrew/` ou instala `.pkg` | [`ps5/README.pt-BR.md`](ps5/README.pt-BR.md) |
| **App do Switch** | [`switch/`](switch/README.pt-BR.md) | `ponte.nro`, construído sobre o motor do Sphaira; instala direto da rede | [`switch/README.pt-BR.md`](switch/README.pt-BR.md) |
| Protocolo | [`protocol/`](protocol/SPEC.md) | Especificação v1, a fonte da verdade (oficial em português; [tradução em inglês](protocol/SPEC.en.md)) | — |
| Referência | [`reference/`](reference/README.pt-BR.md) | Servidor e identificador em Python, gabarito executável da SPEC | [`reference/README.pt-BR.md`](reference/README.pt-BR.md) |
| Ferramentas | [`tools/`](tools/README.pt-BR.md) | Cliente falso (finge ser um console), teste de conformidade do protocolo e checagem da documentação | [`tools/README.pt-BR.md`](tools/README.pt-BR.md) |
| Perfis | `profiles/` | Compatibilidade por firmware (ex.: `ps5-13.60.json`) | — |
| Docs | [`docs/`](docs/README.pt-BR.md) | Índice da documentação, padrão visual e notas de versão | — |

## Baixar pronto (sem compilar)

Versões: [Releases](https://github.com/souzaq001/pontepn-/releases). Cada versão traz `Ponte-Mac.zip`, `ponte.elf`, `ponte.nro`, os hashes e as licenças.

O CI também gera tudo a cada push. Na aba **Actions** do GitHub, abra a execução mais recente da `main` do workflow indicado e baixe em **Artifacts**:

| Artefato | Workflow | O que vem dentro |
|---|---|---|
| `Ponte-Mac` | **Mac e PS5** | `Ponte-Mac.zip` com o `Ponte.app` (não assinado: na primeira vez, **Ajustes → Privacidade e Segurança → Abrir Mesmo Assim**) |
| `Ponte-PS5` | **Mac e PS5** | `ponte.elf`, o payload do PS5 |
| `Ponte-Switch` | **Switch** | `ponte.nro`, o app do Switch (vai em `/switch/ponte/ponte.nro`) |

Para lançar uma versão nova, escreva as notas em `docs/releases/vX.Y.Z.md` (inglês e português no mesmo arquivo) e envie para a `main`: o workflow **Release** compila os três apps, cria a tag e publica os arquivos.

## Compilar e testar

Tudo a partir da raiz do repositório:

```sh
# Referência em Python (só stdlib + pytest, Python 3.10+)
python3 -m pytest reference/tests -q

# App do Mac (macOS 13+; PonteCore e PonteServer também compilam no Linux)
(cd mac && swift build && swift test)
(cd mac && ./scripts/build-app.sh)                     # gera mac/build/Ponte.app

# Protocolo, caixa-preta, contra os dois servidores
python3 tools/conformance.py --server python
python3 tools/conformance.py --server swift            # precisa do swift build antes

# Payload do PS5 (versão de teste para o computador)
make -C ps5 host && python3 ps5/tests/test_payload.py

# Cliente do Switch (núcleo do protocolo, no PC)
make -C switch/hosttest test

# Documentação: pares inglês/português, links e âncoras
python3 tools/check_docs.py
```

Compilar para o console de verdade: [`ps5/README.pt-BR.md`](ps5/README.pt-BR.md#compilar) (SDK de payloads do PS5) e [`switch/README.pt-BR.md`](switch/README.pt-BR.md#compilar) (devkitPro).

## Status

- [x] Protocolo v1 (rascunho 4)
- [x] Identificador de formatos (todos os formatos da SPEC §1)
- [x] Servidor de referência + cliente falso + teste de conformidade, testados de ponta a ponta
- [x] Servidor do Mac em Swift: núcleo, servidor e `ponte-server`, passando no teste de conformidade
- [x] App do Mac (SwiftUI): extração, conversão → `.exfat`, pareamento, cópias em andamento, histórico, ejetar o SSD
- [ ] App do Mac testado num Mac de verdade (conversão com `hdiutil`, permissão de rede local, ejetar)
- [x] Payload PS5: interface web, cópia com retomada para `homebrew/`, `.pkg` pelo instalador do sistema, fila que retoma sozinha, diagnóstico de velocidade; testado contra os dois servidores (versão de teste)
- [x] Payload PS5 testado num console de verdade (cópia de pasta e de `.exfat` para `homebrew/`, jogo aberto pelo ShadowMountPlus)
- [x] App Ponte do Switch: escrito e testado no PC contra a referência
- [x] App do Switch testado num console de verdade (tela, leitura do instalado via ns/ncm, instalação pelo yati)
- [ ] DLC de jogos de PS5 em pasta ou imagem ([SPEC §9](protocol/SPEC.md#9-em-aberto))
- [x] Documentação em inglês e português

Em aberto na especificação: [SPEC §9](protocol/SPEC.md#9-em-aberto).

## Como contribuir

- Todo o trabalho fica na `main`. Para algo maior, abra um branch curto e faça merge de volta assim que o CI passar.
- Mudou o protocolo? Atualize primeiro `protocol/SPEC.md` e o histórico dela (§10), depois a tradução `protocol/SPEC.en.md`, a referência, os testes, o servidor Swift e os clientes.
- Código em inglês; comentários e textos da interface em português.
- Documentação nas duas línguas: `NOME.md` em inglês e `NOME.pt-BR.md` em português. Mudou uma, mude a outra; `tools/check_docs.py` confere.
- Regras completas (também usadas por assistentes de IA): [`CLAUDE.md`](CLAUDE.md).

## Licenças

- O Ponte é distribuído sob a licença **0BSD** ([`LICENSE`](LICENSE)): use, copie, altere e redistribua como quiser, sem condições.
- Exceção: `switch/app/` é baseado no [Sphaira](https://github.com/ITotalJustice/sphaira) e por isso é **GPLv3** ([`switch/app/LICENSE`](switch/app/LICENSE)), assim como o `ponte.nro`.
- `ps5/src/vendor/cJSON.*` é [cJSON](https://github.com/DaveGamble/cJSON) (MIT).
- Aviso legal e marcas: [`LEGAL.pt-BR.md`](LEGAL.pt-BR.md).
