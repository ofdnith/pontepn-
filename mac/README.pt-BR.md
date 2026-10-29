# Servidor do Mac

[English](README.md) · **Português**

App de barra de menu em Swift/SwiftUI que observa a biblioteca no SSD, prepara os jogos e serve o catálogo para os consoles (ver [`../protocol/SPEC.md`](../protocol/SPEC.md)). Faz o mesmo que a [referência em Python](../reference/), validado pelo mesmo [teste de conformidade](../tools/conformance.py).

Requisitos: macOS 13 ou mais novo, com as ferramentas de linha de comando do Xcode (`xcode-select --install`). Para compactados divididos ou com senha: `brew install sevenzip`.

## Estrutura

| Alvo | O quê |
|---|---|
| `PonteCore` | identificador de formatos, biblioteca (varredura, estabilidade, `id`/`revision`, duplicados, hashes), perfis, estado do console, plano do `.exfat`. Só Foundation. |
| `PonteServer` | HTTP/1.1 (sockets POSIX, keep-alive, `Range`), descoberta UDP, pareamento, rotas da SPEC e o preparo: extrair compactados e converter pasta → `.exfat`. |
| `ponte-server` | servidor de linha de comando, com as mesmas opções da referência em Python. |
| `PonteMac` | o app de barra de menu. |

`PonteCore` e `PonteServer` também compilam no Linux, onde os testes rodam no CI ou em qualquer máquina sem Mac. A conversão para `.exfat` usa `mkfile`, `hdiutil`, `newfs_exfat` e `diskutil`, então de verdade só roda no macOS; o fluxo é testado com ferramentas falsas.

## Compilar e testar

Na pasta `mac/`:

```sh
swift build                 # tudo
swift test                  # testes do núcleo e do servidor
./scripts/build-app.sh      # monta build/Ponte.app (arraste para /Applications)
```

Teste de conformidade do protocolo, na raiz do repositório (precisa do `swift build` antes):

```sh
python3 tools/conformance.py --server swift
python3 tools/conformance.py --server python    # a referência, para comparar
```

## Usar o app

Jeito mais fácil, sem compilar: baixe o artefato `Ponte-Mac` da última execução (aba **Actions** do GitHub → workflow **Mac e PS5** → execução mais recente da `main` → **Artifacts** → `Ponte-Mac`); dentro vem o `Ponte-Mac.zip`, descompacte e arraste o `Ponte.app` para **Aplicativos**. Como o app não é assinado pela Apple, na primeira vez o macOS bloqueia: abra **Ajustes do Sistema → Privacidade e Segurança** e clique em **Abrir Mesmo Assim** (ou clique com o botão direito no app → **Abrir**).

1. Abra o `Ponte.app` como qualquer app: abre uma janela e aparece no Dock. Também fica um ícone na barra de menu.
2. Em **Ajustes** (engrenagem), escolha a pasta da biblioteca no SSD.
3. O código de pareamento aparece na janela. Digite no console na primeira vez.

Fechar a janela não para o servidor (os consoles continuam copiando); para reabrir, clique no ícone do Dock ou em **Abrir o Ponte** na barra de menu. Para parar de vez: **Sair**.

Na primeira vez que um console procurar o Mac, o macOS pede permissão de **rede local** para o Ponte: aceite. Se o firewall do macOS estiver ligado, permita conexões de entrada também.

O que a janela mostra:

- **Código de pareamento** e o endereço (`IP:47101`), para digitar no console se a descoberta automática não funcionar.
- **Biblioteca**: quantos jogos de PS5 e de Switch estão prontos, quantos ainda estão chegando e o que está sendo preparado (extraindo ou convertendo, com progresso).
- **Precisa de atenção**: itens que não deu para identificar (você escolhe plataforma, tipo e Title ID, e o app grava o `.ponte.json`), compactados com senha, conjuntos incompletos e falhas, com "tentar de novo".
- **Copiando agora**: cada cópia em andamento, com barra de progresso, quanto já foi ("1,2 GB de 4,5 GB"), velocidade, tempo restante, o console e o arquivo atual. Na retomada, o que o console já tinha conta no progresso. Uma cópia parada mostra há quanto tempo.
- **Consoles pareados**, com opção de revogar.
- **Histórico**: as últimas cópias com resultado (instalado, copiado, cancelado, falhou ou interrompido), console, tamanho, duração e velocidade média. **Ver tudo** (ou **Histórico de cópias…** na barra de menu) abre uma janela com a tabela completa, busca por jogo, console ou resultado, e **Limpar histórico**. Fica guardado entre aberturas do app (últimas 500 cópias) e continua visível com o SSD ejetado.

Uma cópia vira "interrompida" quando o console para de baixar por 2 minutos sem dizer como terminou (queda de rede, console desligado, SSD ejetado). Se o console retomar depois, aparece como uma cópia nova. No `.pkg` do PS5, o console diz "instalado" assim que entrega o pacote ao instalador do sistema; o download que o instalador faz em seguida aparece em "Copiando agora" ("o console está baixando o pacote para instalar") e completa o mesmo registro do histórico.

**Ejetar o SSD**: botão **Ejetar** ao lado do caminho da biblioteca (e **Ejetar …** na barra de menu), quando a biblioteca está num disco externo. Ejetar pelo Finder (ou Utilitário de Disco) também funciona: o Ponte recebe o pedido do macOS antes de desmontar, solta os arquivos (fecha as conexões, para o preparo, a varredura e os hashes) e deixa o disco sair, sem "disco em uso". Se outro programa impedir, o Ponte volta a servir sozinho em 20 s. O botão do app tem a vantagem de perguntar antes quando um console está copiando.

Enquanto o app está aberto, a varredura e o cálculo de hashes leem o SSD com prioridade baixa (cedem a vez a você no Finder), e a imagem temporária da conversão para `.exfat` não aparece no Finder.

- Sem nada acontecendo, ejeta na hora.
- Com um console copiando (envio agora ou nos últimos 30 s), pergunta: **Esperar terminar** (ejeta sozinho quando as cópias acabarem; dá para **Ejetar agora** ou **Não ejetar** enquanto espera), **Ejetar agora** (a cópia para e o console retoma de onde parou quando o SSD voltar) ou **Cancelar**.
- Uma extração ou conversão em andamento é interrompida e recomeça do zero quando o SSD voltar; a pasta ou o compactado original continua lá.
- Enquanto o SSD está fora, o servidor fica desligado (para o console é como se o Mac estivesse desligado). Ao conectar o SSD de novo, ele volta sozinho.
- Se outro programa estiver usando o disco, o app diz qual e continua servindo.

Ajustes: nome na rede, converter pastas de PS5 em `.exfat` (**desligado**: a conversão demora e ocupa espaço), extrair compactados (**desligado**: extraia você mesmo dentro da biblioteca), guardar originais em `_originais/` (ligado; desligado, apaga), conferir as cópias com SHA-256 (**desligado**: as cópias começam na hora e o console confere só o tamanho), exigir pareamento (ligado) e abrir ao iniciar sessão. Também em **Ponte → Ajustes… (⌘,)**.

Onde ficam as coisas: estado (id do servidor, consoles pareados, hashes já calculados, histórico de cópias) e perfis de firmware em `~/Library/Application Support/Ponte/`. Os perfis podem ser editados ali ("Abrir pasta de perfis de firmware" nos ajustes); os que vêm com o app só são copiados quando não existem.

## Servidor de linha de comando

```sh
.build/debug/ponte-server --library /Volumes/SSD/Jogos
.build/debug/ponte-server --library /Volumes/SSD/Jogos --convert-folders --extract
```

`--help` lista as opções. Sem `--convert-folders`/`--extract` ele se comporta exatamente como a referência em Python (só serve o que já está pronto).

## Como funciona o preparo

- **Varredura** a cada 5 s (pastas no SSD externo nem sempre geram eventos confiáveis; a varredura é barata depois que os jogos viram arquivo único). Hashes `sha256` em segundo plano, guardados entre execuções.
- **Pasta de PS5** estável → fica fora do catálogo (o console recebe `409` se pedir) → vira `<TITLE_ID>.exfat` com o arquivo lateral e o ícone → a pasta vai para `_originais/`.
- **Compactado** completo e estável → extraído para uma pasta temporária `.ponte-tmp` → o que saiu de dentro entra na biblioteca → as partes vão para `_originais/`.
- Uma falha vira "precisa de atenção" e não é repetida sozinha.
