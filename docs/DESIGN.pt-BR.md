# Padrão visual do Ponte

[English](DESIGN.md) · **Português**

Os três apps (Mac, PS5 e Switch) usam as mesmas cores, a mesma anatomia de progresso e os mesmos textos de tamanho, velocidade e tempo. Quem já usou um reconhece o outro.

## Cores

Tema escuro (PS5 e Switch sempre; Mac no modo escuro):

| Token | Hex | Uso |
|---|---|---|
| `bg` | `#10141C` | fundo |
| `panel` | `#181E29` | cartões, linhas, painéis |
| `line` | `#273042` | bordas, divisórias, **trilha das barras** |
| `input` | `#0D1118` | campos e botões secundários |
| `text` | `#E8ECF3` | texto principal |
| `muted` | `#8E99AD` | texto secundário (status, detalhes) |
| `accent` | `#3FB6C4` | **turquesa Ponte**: ação principal, barra em andamento, velocidade, foco |
| `accent-ink` | `#062A2F` | texto sobre `accent` |
| `ok` | `#4CC38A` | concluído, instalado, conectado |
| `warn` | `#E5B450` | pausado, esperando, precisa de atenção |
| `bad` | `#EF6B6B` | falhou, erro |

Tema claro (só o Mac, no modo claro do sistema): `accent #0B8794`, `ok #1E9E63`, `warn #B97F06`, `bad #D23C3C`, trilha `#E2E7EE`, cartão `#F3F5F8`. Texto e fundo seguem o sistema.

Regras:
- **A cor nunca é a única pista**: estado sempre também em texto ("pausado", "falhou") ou ícone.
- Barra em andamento = `accent`; pausada/esperando = `warn`; concluída = `ok`; falhou = `bad`; trilha = `line`.
- Uma única cor de destaque por tela. As plataformas (PS5, Switch) não têm cor própria; usam ícone ou texto.

## Linha de progresso

Tudo que leva mais de um instante e tem tamanho mostra a mesma linha, em todos os apps:

```
[ícone] Título ................................. 37%
████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
1,2 GB de 4,5 GB · 38 MB/s · faltam 2 min
detalhe: etapa, console, arquivo atual
```

- **Percentual** à direita do título, na cor da barra. Sem total conhecido: barra indeterminada (faixa que corre) e sem percentual.
- **Status**: `<feito> de <total> · <velocidade> · faltam <tempo>`, na ordem, só o que se souber. Pausado: o motivo no lugar ("pausado: um console está copiando; continua sozinho").
- **Detalhe**: uma linha, cortada no meio se for longa.
- Velocidade: média dos últimos ~5 s; parado há mais de 2 s mostra 0 (não a média antiga).
- Tempo restante: pelos bytes quando há total, senão pelo percentual; abaixo de 1 min, "falta menos de 1 min".

Onde aparece:

| App | Operações com barra e velocidade |
|---|---|
| Mac | cópias para os consoles, extração de compactados, conversão pasta → `.exfat`, conferência (sha256) |
| PS5 | conferência do que já tinha (retomada), download, conferência final, mover para `homebrew/`, entrega do `.pkg`, diagnóstico |
| Switch | instalação de cada item (download + gravação), fila inteira |

## Textos

- Tamanhos em base 10, como o Finder: `1,2 GB`, `38 MB/s` (vírgula decimal em português).
- Duração: `45 s`, `12 min`, `1 h 5 min`.
- Estados: `na fila`, `copiando`, `pausado`, `conferindo`, `movendo`, `instalando`, `concluído`, `falhou`, `cancelado`.
- Mensagens de erro dizem o que aconteceu **e** o que fazer ("O Mac não respondeu. Confira se o app está aberto; a cópia continua sozinha quando ele voltar.").

## Onde está no código

| App | Arquivo |
|---|---|
| Mac | `mac/Sources/PonteMac/Brand.swift` (`Brand`, `ProgressRow`, `Fmt`) |
| PS5 | variáveis CSS no topo de `ps5/ui/index.html` |
| Switch | `switch/app/sphaira/source/ui/menus/ponte.cpp` (cores da marca) e o tema padrão |
