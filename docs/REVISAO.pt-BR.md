# Revisão dos três apps: velocidade, progresso, visual e boas práticas

[English](REVISAO.md) · **Português**

Levantamento feito em outubro de 2026, cobrindo o app do Mac, o payload do PS5 e o app do Switch. Cada item diz **o que foi achado**, **o que mudou** e, quando ainda está em aberto, **o que falta**. O padrão visual que resultou desta revisão está em [`DESIGN.pt-BR.md`](DESIGN.pt-BR.md).

Legenda: ✅ feito · 🔶 em aberto (proposto)

## 1. Velocidade de transferência

A cópia passa por quatro etapas, e a mais lenta define a velocidade: **disco do Mac → envio do Mac → rede → recepção, conferência e gravação no console**. Na cópia o Mac tem prioridade sobre todo o resto que ele faz.

### Mac (servidor)

| Achado | Impacto | Situação |
|---|---|---|
| O envio lia 1 MiB do SSD, esperava, mandava pela rede, esperava. Disco e rede nunca trabalhavam juntos, e cada pedaço criava um buffer novo. | alto | ✅ `sendfile`: o kernel lê do disco e escreve no socket sem passar pelo app, com leitura adiantada (`F_RDAHEAD`/`F_RDADVISE`). Fora do macOS, `pread` num buffer fixo de 4 MiB. |
| Buffer de envio do socket no padrão do sistema. | médio | ✅ `SO_SNDBUF` de 4 MiB. |
| O Mac podia entrar em repouso no meio de uma cópia (`userInitiatedAllowingIdleSystemSleep`), derrubando o console. | alto (cópias longas) | ✅ Enquanto há envio aberto, o app segura o repouso automático e fica com prioridade de interação. Fechar a tampa ainda faz dormir, como deve. |
| Extração (7-Zip) e conversão para `.exfat` disputavam o SSD com o envio. | alto (durante preparo) | ✅ O preparo cede a vez: o extrator é **suspenso** enquanto um console copia e retomado depois; a conversão pausa entre blocos. Tudo de preparo roda em prioridade baixa (QoS `utility`). |
| Threads de envio em `userInitiated`. | baixo | ✅ `userInteractive`: a prioridade mais alta para CPU e disco. |
| Cálculo de hashes lia em blocos de 1 MiB. | baixo | ✅ 4 MiB (e ele já esperava as cópias terminarem). |

### PS5 (payload)

| Achado | Impacto | Situação |
|---|---|---|
| Uma conexão TCP nova **para cada arquivo** (`Connection: close`), com criação de thread por arquivo. Pastas de jogo têm milhares de arquivos. | muito alto (pastas) | ✅ Keep-alive: uma conexão e uma thread de rede por cópia inteira (teste: 6 arquivos, 1 conexão); pastas já criadas são lembradas. |
| SHA-256 em C puro (~210 MB/s), em série com a gravação: com disco de 200 MB/s, o conjunto cai para ~120 MB/s. O processador do PS5 tem instrução SHA própria. | alto | ✅ Instrução SHA do processador quando existe: **~1,2 GB/s** (5,5×), conferida contra a versão em C por um teste próprio. |
| Retomada relia o arquivo parcial inteiro para conferir, sem progresso e sem atender a cancelar/pausar. | alto (retomada) | ✅ Com barra e velocidade, atendendo a pausar e cancelar, e 5,5× mais rápida. 🔶 Guardar pontos de conferência para nem precisar reler. |
| `SO_RCVBUF` fixo desliga o ajuste automático do FreeBSD; o valor efetivo era sobrescrito por qualquer conexão (consultas, ícones). | baixo a médio | ✅ Valor por conexão; consultas e ícones sem o buffer grande. 🔶 Comparar no console `rcvbuf_kb=0` (automático) com o padrão e ficar com o melhor. |
| Mac fora do ar só era percebido depois de 60 s. | médio | ✅ 20 s. |

### Switch (app)

| Achado | Impacto | Situação |
|---|---|---|
| Entre o download (curl) e o instalador há um buffer de **64 KB**. Cheio, ele **pausa** o curl, que só volta no próximo aviso de progresso; nesse meio tempo a janela TCP fecha e o Mac para de mandar. | muito alto | ✅ 8 MB só para o download do Ponte; a escrita espera espaço em vez de pausar; leituras de 512 KB do socket; sem apagar o começo do buffer a cada leitura. |
| Buffer TCP de recepção começando em 64 KB. | alto | ✅ 1 MB (máximo continua 4 MB). |
| Aberto pelo Álbum, o sistema limita a rede do app (máximo de 256 KB). | alto | ✅ Aviso no topo da tela do Ponte dizendo como abrir. |
| Instalar lê o `.cnmt.nca` (no fim do NSP) primeiro: cada pulo reabre a conexão. | médio | 🔶 Ler em ordem desligaria "pular o que já está instalado"; fica como está. |
| Descompressão de NSZ numa só thread. | baixo a médio | 🔶 Só pesa com rede por cabo USB. |

## 2. Progresso e velocidade em tudo

| Operação | Antes | Agora |
|---|---|---|
| Mac: cópia para um console | barra, velocidade, tempo restante | ✅ igual, no componente comum |
| Mac: extração de compactado | só um spinner, sem número | ✅ barra (percentual do 7-Zip), bytes extraídos, velocidade, tempo restante, "pausado" quando cede a vez |
| Mac: conversão pasta → `.exfat` | barra | ✅ barra + bytes + velocidade + tempo restante |
| Mac: conferência (sha256) | invisível | ✅ barra, velocidade, arquivos na fila |
| Mac: barra de menu | só cópias | ✅ cópias e preparo |
| PS5: fila | barra só enquanto copiava | ✅ resumo da fila (itens, quanto falta, velocidade, tempo) e barra em todos os estados |
| PS5: conferência na retomada e no fim | invisível (barra parada) | ✅ fase própria com barra e velocidade |
| PS5: mover para `homebrew/`, entregar `.pkg` | só texto | ✅ fase com barra indeterminada |
| PS5: diagnóstico de velocidade | só texto | ✅ barra, MB/s ao vivo e tabela de resultados |
| Switch: instalação | barra, velocidade crua (pulava a cada segundo), "0 segundos restantes" com velocidade zero | ✅ bytes feitos e total, velocidade em média móvel, tempo restante só quando há velocidade |
| Switch: conectando, consultando o Mac | só texto | ✅ indicador girando |
| Switch: fila inteira | — | 🔶 barra só por item |

## 3. Visual

- ✅ Uma paleta para os três apps (a do PS5, que era a mais completa: turquesa `#3FB6C4`), com versão clara para o Mac. Tabela em [`DESIGN.pt-BR.md`](DESIGN.pt-BR.md).
- ✅ Mesma anatomia de progresso nos três: título e percentual, barra, `feito de total · velocidade · faltam`, detalhe.
- ✅ Mesmos textos de tamanho (base 10, vírgula decimal), velocidade e tempo.

## 4. Nomes

- ✅ Workflows: **Mac e PS5** (`mac-ps5.yml`) e **Switch** (`switch.yml`), no lugar de "CI" e "switch-app".
- ✅ Artefatos: `Ponte-Mac` (`Ponte-Mac.zip`), `Ponte-PS5` (`ponte.elf`), `Ponte-Switch` (`ponte.nro`).

## 5. Boas práticas de código

### Switch
- ✅ `toupper` com `unsigned char` (comportamento indefinido com acentos).
- 🔶 Consultas ao Mac sem prazo (só o report tem 5 s): um Mac que some sem responder deixa a tela esperando até o B.

### PS5
- ✅ `queue.json` gravado sob trava própria (antes duas threads podiam corromper o arquivo).
- ✅ `fsync` antes de renomear: queda de energia não deixa arquivo vazio com o nome final.
- ✅ Report e notificação não leem mais o job depois de liberado (podia ler lixo).
- ✅ Tamanho e caminhos vindos do Mac validados; ids validados contra injeção de cabeçalho.
- ✅ Hash errado baixa o arquivo de novo uma vez antes de falhar (a mensagem já prometia isso).
- ✅ Tempos pelo relógio monotônico (o relógio do PS5 pode pular no boot).
- ✅ 4 testes novos (keep-alive, fases, hash refeito, ids inválidos) + teste do SHA-256.
- 🔶 Servidor web do payload sem limite de conexões simultâneas.

### Mac
- ✅ Testes novos para o medidor de velocidade, o progresso dos hashes e a pausa do preparo durante cópias.
- ✅ Saída dos programas externos lida numa thread à parte (o pipe nunca enche) e só o fim guardado.
- 🔶 O app atualiza o retrato inteiro a cada 1 s pelo `Timer`; dá para publicar só o que mudou (menos redesenho com o app aberto).
- 🔶 `PonteServer` e `Library` usam `@unchecked Sendable` com travas manuais; migrar para `actor` quando o projeto for para Swift 6.
- 🔶 Sem assinatura e notarização: o macOS bloqueia na primeira abertura. Com uma conta de desenvolvedor Apple, assinar no CI.

## 6. UX

### PS5
- ✅ Foco do controle não se perde mais a cada 1,5 s (a lista não é reconstruída).
- ✅ Letras maiores e botões de 48 px na TV; duas colunas em telas largas; funciona no celular.
- ✅ Confirmação antes de cancelar, substituir ou trocar de Mac; botão Copiar travado enquanto envia.
- ✅ Erros dizem o que aconteceu e o que fazer; barras acessíveis a leitores de tela.
- ✅ Campo de IP com teclado que tem ponto no celular; consulta pausa com a página escondida.

### Switch
- ✅ Tema com as cores da marca como padrão (quem tinha o tema padrão do Sphaira salvo passa para ele).
- ✅ Aviso quando aberto pelo Álbum (rede lenta) dizendo como abrir certo.
- 🔶 Mensagens de erro da instalação ainda saem como código (`yati 0x…`).
- 🔶 Desmarcar um jogo não desmarca a atualização e os DLC que foram marcados junto.

### Mac
- ✅ Ajustes também em **Ponte → Ajustes… (⌘,)**, como em qualquer app do Mac.
- ✅ Estado nunca só pela cor: barra pausada vem com o motivo escrito.
- 🔶 Notificação do sistema quando uma cópia termina ou falha com o app em segundo plano.
- 🔶 Mostrar a velocidade da rede do Mac (Wi-Fi ou cabo) ao lado do endereço: Wi-Fi é a causa mais comum de cópia lenta.
