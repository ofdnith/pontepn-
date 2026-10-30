# Ponte no Switch

[English](README.md) · **Português**

O **Ponte** é um app próprio para o Switch (`ponte.nro`). Ele abre direto na lista de Macs, pareia com o código de 6 dígitos, mostra a biblioteca ao lado do que já está no console e instala jogos, atualizações e DLC direto da rede (`.nsp`, `.nsz`, `.xci`, `.xcz`).

Regras específicas do Switch: [SPEC §8.1](../protocol/SPEC.md#81-cliente-do-switch-app-ponte).

## Instalar no console

1. Baixe o `ponte.nro`: aba **Actions** deste repositório → workflow **Switch** → execução mais recente da `main` → artefato **Ponte-Switch**. Descompacte o zip.
2. Copie para o cartão em `/switch/ponte/ponte.nro`.
3. No Switch com Atmosphère, abra o menu de homebrew **segurando R ao abrir um jogo** (pelo Álbum o homebrew tem pouca memória) e escolha **Ponte**.

Se a tela só piscar e voltar, o app não chegou a abrir: confira se copiou o `.nro` (e não o zip) e se o Atmosphère está atualizado.

## Como fica

```
Ponte  192.168.0.12:47101 • 3 itens • 2 marcados (15.3 GB)          microSD 212.0 GB
──────────────────────────────────────────────────────────────────────────────────────
 [■] [ícone] Jogo Legal                                                         Novo
             Jogo • NSP • 14.2 GB
   [■] Jogo Legal                                                               Novo
       Atualização v131072 • NSP • 1.1 GB
   [ ] Jogo Legal                                                          Instalado
       DLC • NSZ • 210 MB
 [ ] [ícone] Outro Jogo                                     Mais nova que a instalada
──────────────────────────────────────────────────────────────────────────────────────
 A Marcar   X Instalar 2   Y Atualizar   − Limpar   B Voltar   ZR Opções   + Sair
```

- **Lista de Macs**: os salvos e os achados na rede. A conecta, X busca de novo, Y digita um endereço, − esquece um Mac salvo, B ou + sai.
- **Biblioteca**: agrupada por jogo (base, atualizações, DLC). A marca/desmarca (marcar um jogo novo já marca a atualização mais nova e os DLC que faltam), − marca tudo que é novo ou limpa, X instala, Y atualiza. Itens incompatíveis mostram o motivo no rodapé.
- **Instalar**: pergunta microSD ou memória do console, confere o espaço (+5%) e instala em sequência; cada item gera um `report` no Mac. Na primeira instalação o app pede para ligar a instalação de jogos (uma vez só).
- **Durante a instalação**: Y apaga a tela (só no modo portátil; qualquer botão acende) e o console não entra em descanso sozinho. O botão HOME leva ao menu e a cópia **continua por trás**; ao voltar, a caixa mostra onde parou. Abrir outro jogo fecha o Ponte (o Switch só roda um por vez) e cancela a instalação. Isso vale aberto segurando R num jogo; pelo Álbum o menu HOME pausa a cópia, que segue ao voltar. Se o console dormir (botão de energia), a cópia tenta retomar por `Range` por até 60 s depois que a rede volta.
- **ZR Opções**: idioma, tema e opções de instalação do motor.
- A lista se atualiza sozinha quando o Mac ganha jogos novos (consulta a cada 20 s).

## Velocidade

- O download entra no instalador por um buffer de 8 MB (no Sphaira são 64 KB, que pausam a transferência a todo momento) e o buffer TCP de recepção começa em 1 MB.
- **Abra pelo menu de homebrew segurando R num jogo.** Aberto pelo Álbum, o sistema dá ao app bem menos memória de rede e a cópia fica muito mais lenta; o Ponte avisa no topo da tela quando isso acontece.
- A caixa de instalação mostra quanto já veio, a velocidade e o tempo restante.

## Código

```
switch/app/        o app: motor de instalação e interface do Sphaira + a tela do Ponte
switch/hosttest/   o núcleo do protocolo compilado para o PC e testado contra a referência
```

O Ponte é construído sobre o [Sphaira](https://github.com/ITotalJustice/sphaira) (GPLv3), que já tem um instalador maduro de NSP/NSZ/XCI/XCZ. Por isso o app também é **GPLv3** (`switch/app/LICENSE`). O que o Ponte acrescenta e muda está em [`app/PONTE.pt-BR.md`](app/PONTE.pt-BR.md).

## Compilar

O CI (`.github/workflows/switch.yml`, workflow **Switch**) compila a cada push em `switch/`, `protocol/` ou `reference/` e gera o artefato **Ponte-Switch**: motor enxuto, sem FTP/MTP, HDD por USB e música.

Localmente, com o devkitPro (`dkp-pacman -S switch-dev` e as libs que o README do Sphaira lista):

```sh
cd switch/app
cmake --preset Ponte
cmake --build --preset Ponte
# build/Ponte/ponte.nro
```

## Testar sem o console

`hosttest/` compila o mesmo `ponte_proto.cpp` do app para o PC e roda o fluxo do console contra o servidor de referência: descoberta, código errado (`401`), pareamento, `console/state`, `catalog/changes`, revisão velha (`412`), download com queda no meio e retomada por `Range`, `report` e protocolo de outra versão (`426`).

```sh
make -C switch/hosttest test
```

O que esses testes **não** cobrem, e precisa ser visto no console: a tela em si, a leitura do que está instalado (ns/ncm), a descoberta por broadcast numa rede de verdade e a instalação pelo yati.
