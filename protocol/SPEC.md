# Protocolo Ponte — v1 (rascunho 4)

[English](SPEC.en.md) · **Português**

> Esta é a versão oficial da especificação. A tradução em inglês ([`SPEC.en.md`](SPEC.en.md)) só a acompanha; se as duas divergirem, vale esta.

Especificação da comunicação entre o **servidor** (app no Mac) e os **clientes** (payload no PS5, app Ponte no Switch, ferramenta de teste em `tools/`).

A implementação de referência em Python (`reference/`) é o gabarito executável desta especificação. Quando as duas divergirem, é bug em uma delas: corrija e registre em §10.

## 0. O que o sistema faz (e o que não faz)

- O Mac guarda a biblioteca no SSD, **identifica** cada jogo e publica um catálogo.
- No console, o cliente Ponte mostra lado a lado **o que existe no Mac** e **o que já está no console**. Você escolhe o que copiar e para qual armazenamento.
- O cliente **copia** o jogo pela rede para o armazenamento do console e o deixa pronto para jogar: instala pacotes, ou coloca imagens e pastas onde o montador as encontra.
- **Nada roda pela rede durante o jogo.** Depois da cópia, o Mac pode até estar desligado.

Princípio: **o console puxa, o Mac serve.** O Mac nunca inicia conexão com o console.

---

## 1. Formatos suportados

### 1.1 PS5 (inclui jogos de PS4 que rodam no PS5)

| `kind` | Extensão / forma | Como o Mac identifica | O que o cliente faz no console |
|---|---|---|---|
| `folder` | pasta com `sce_sys/param.json` na raiz (ex.: `PPSA12345/`) | lê `sce_sys/param.json` | copia para a área temporária e move para `<destino>/homebrew/<TITLE_ID>/` → montador registra |
| `exfat` | `.exfat` (imagem exFAT) | metadados gravados na conversão (§7.4); senão, nome do arquivo | copia para a área temporária e move para `<destino>/homebrew/` → montador monta e registra |
| `ffpkg` | `.ffpkg` (imagem UFS) | nome do arquivo (leitura interna em aberto, §9) | idem `exfat` |
| `ffpfs` | `.ffpfs` (imagem PFS, experimental) | nome do arquivo | idem `exfat` |
| `ffpfsc` | `.ffpfsc` (contêiner PFS comprimido, experimental) | nome do arquivo | idem `exfat` |
| `pkg_ps4` | `.pkg` de PS4 | cabeçalho do pacote (§1.4) | baixa para a área temporária → instalador do sistema → apaga |
| `pkg_ps5` | `.pkg` de PS5 | cabeçalho do pacote (§1.4) | idem `pkg_ps4` |
| `folder_ps4` | pasta com `sce_sys/param.sfo` | lê `param.sfo` | **a confirmar** qual carregador usar; por ora só copia |

Os formatos de imagem e pasta dependem de um **montador** no PS5 (hoje, ShadowMountPlus com kstuff), que procura jogos em `homebrew/` no armazenamento interno, no M.2/estendido e nos USB.

Para jogos de PS5 em pasta e imagem, `content` é sempre `base`: atualizações costumam vir já mescladas nesses formatos.

### 1.1.1 Perfis de compatibilidade

O que funciona em cada firmware fica em arquivos de perfil no Mac (`profiles/ps5-<firmware>.json`), não no código. Para um console, vale o perfil de **maior firmware menor ou igual** ao do console. Se não houver nenhum, vale o de menor firmware, com o aviso `profile_guessed`.

Formato do perfil:
```json
{
  "platform": "ps5",
  "firmware": "13.60",
  "kinds": {
    "exfat":      {"status": "ok"},
    "ffpkg":      {"status": "ok"},
    "folder":     {"status": "ok", "note": "alguns títulos só rodam a partir de USB"},
    "ffpfs":      {"status": "experimental"},
    "ffpfsc":     {"status": "experimental"},
    "pkg_ps4":    {"status": "ok"},
    "pkg_ps5":    {"status": "blocked", "note": "pacote .pkg de PS5 não roda neste firmware"},
    "folder_ps4": {"status": "unknown"}
  }
}
```
`status`: `ok` | `experimental` (copia, com aviso) | `unknown` (copia, com aviso) | `blocked` (item aparece como `incompatible`).

Perfil de referência do projeto: **13.60**, com homebrew habilitado a cada boot (kstuff → ShadowMountPlus → etaHEN). Nele, o caminho principal (`exfat`, `ffpkg`, `folder`) exige do payload **apenas copiar e mover**; o ShadowMountPlus faz o resto.

### 1.2 Switch

| `kind` | Extensão | Como o Mac identifica | O que o cliente faz |
|---|---|---|---|
| `nsp` | `.nsp` | contêiner PFS0: Title ID pelo nome do `.tik` interno; senão, pelo nome do arquivo | instala pelo motor do Sphaira |
| `nsz` | `.nsz` | igual ao `nsp` | instala (descompressão no console) |
| `xci` | `.xci` | nome do arquivo; com `prod.keys` no Mac, metadados completos (futuro) | instala |
| `xcz` | `.xcz` | igual ao `xci` | instala |

Title ID e tipo de conteúdo (Title ID como inteiro de 64 bits, `t`):

| Condição | `content` | `parent_title_id` |
|---|---|---|
| `t & 0xFFF == 0x000` e bit 12 (`0x1000`) desligado | `base` | — |
| `t & 0xFFF == 0x800` | `update` | `t & ~0xFFF` |
| bit 12 ligado e `t & 0xFFF != 0` | `dlc` | `(t & ~0xFFF) - 0x1000` |
| qualquer outro | `unknown` | — |

Nome de arquivo no padrão da comunidade: `Nome [0100ABCD12340000][v65536].nsp`. O Title ID é o primeiro grupo de 16 dígitos hexadecimais entre colchetes; a versão é `[vN]`, com N inteiro.

### 1.3 Arquivos compactados (qualquer plataforma)

`.zip`, `.rar`, `.7z`, `.tar` e partes divididas (`.part1.rar`/`.part01.rar`, `.7z.001`, `.zip.001`, `.z01`…). **Nunca vão para o console.** O Mac agrupa as partes de um mesmo conjunto, espera todas estarem estáveis, extrai para a biblioteca e identifica o que saiu de dentro. Senha → o Mac pede na interface. Depois de extrair, o compactado é apagado ou movido para `_originais/` (configurável). Conjunto incompleto (falta uma parte) → "precisa de atenção".

Detalhes da extração:
- Extrator: 7-Zip (`7zz`/`7z`) quando instalado; sem ele, `bsdtar` do macOS, que só cobre arquivos de uma parte sem senha. Partes divididas ou senha sem 7-Zip → "precisa de atenção" pedindo para instalar.
- Extrai para `<pasta do compactado>/<nome>.ponte-tmp/`, que a varredura ignora. No fim: se saiu um único item de dentro (o caso comum, ex.: `PPSA01234/`), ele vai direto para a pasta do compactado; senão, a pasta temporária vira `<nome>/`. Assim o jogo fica dentro do limite de 2 níveis da varredura (§7.2).
- Nome já ocupado → acrescenta ` (2)`, ` (3)`…

### 1.4 Identificação de `.pkg`

Todos os campos do cabeçalho são **big-endian**.

1. Magic nos bytes 0–3: `7F 43 4E 54` (`\x7FCNT`). Outro magic → "precisa de atenção".
2. Content ID: 36 bytes ASCII a partir de `0x40` (ex.: `UP0000-CUSA00001_00-XXXXXXXXXXXXXXXX`). O Title ID são os 9 caracteres a partir do 8º (`CUSA00001`).
3. Plataforma pelo prefixo do Title ID: `PPSA` e `PPSB` → `pkg_ps5`; demais prefixos de 4 letras (`CUSA`, `PCJS`, `PLJM`…) → `pkg_ps4`. Configurável.
4. Para `pkg_ps4`, os metadados vêm do `param.sfo` interno: tabela de entradas com `entry_count` (u32 em `0x10`) entradas de 32 bytes a partir de `table_offset` (u32 em `0x18`). Cada entrada: `id` (u32, +0), `offset` (u32, +16), `size` (u32, +20). `id 0x1000` = `param.sfo`; `id 0x1200` = `icon0.png`.
5. `CATEGORY` do `param.sfo`: `gd` → `base`; `gp` → `update`; `ac` → `dlc`; outro → `unknown`. Para `update` e `dlc`, `parent_title_id` = Title ID do próprio pacote (mesmo título).
6. Versão: `APP_VER` (base) ou `VERSION`; firmware mínimo: `SYSTEM_VER`, inteiro em BCD (`0x05050000` → `5.05`).

### 1.5 Identificação de pastas

- `sce_sys/param.json` (PS5): `titleId`, `contentId`, `contentVersion`, `localizedParameters.<defaultLanguage>.titleName`, `requiredSystemSoftwareVersion` (texto hexadecimal BCD, ex.: `0x0403000000000000` → `4.03`). Campos ausentes ficam `null`, sem impedir a identificação. Exige também `eboot.bin` na raiz; sem ele → "precisa de atenção".
- `sce_sys/param.sfo` (PS4): mesmos campos de §1.4, itens 5–6.
- Ícone: `sce_sys/icon0.png`, quando existir.

### 1.6 Arquivo lateral de metadados

Para cada item identificado, o Mac grava ao lado dele `<nome>.ponte.json` com os metadados finais (os campos de catálogo de §5.3, exceto `id` e `revision`). Serve para:
- não perder os metadados lidos da pasta quando ela vira `.exfat`;
- guardar o que você preencheu à mão em "precisa de atenção";
- dar nome e versão a `.ffpkg`/`.ffpfs` sem precisar ler a imagem.

Se o arquivo lateral existir, ele tem prioridade sobre a identificação automática. O ícone vai em `<nome>.ponte.png`.

---

## 2. Transporte

| Item | Valor |
|---|---|
| Protocolo | HTTP/1.1 sobre TCP, sem TLS (rede local) |
| Porta padrão | `47101` (configurável no Mac) |
| Corpo | JSON UTF-8 nas rotas `/api`, binário (`application/octet-stream`) nas rotas de arquivo |
| Prefixo | `/api/v1` |
| Conexões | persistentes (keep-alive) suportadas e recomendadas |

- Respostas sempre com `Content-Length` (sem chunked), para um cliente em C mínimo.
- Tamanhos e posições são inteiros de **64 bits** (jogos passam de 4 GB). Clientes em C usam `uint64_t`.
- `HEAD` funciona em toda rota de arquivo, com os mesmos cabeçalhos do `GET`.

## 3. Descoberta na rede

1. O cliente envia broadcast UDP para a porta `47100` com o texto ASCII `PONTE?1`, a partir de uma porta qualquer, e escuta nesse mesmo socket por ~1 segundo.
2. Cada servidor responde (unicast, para a porta de origem) com JSON:
   ```json
   {"ponte": 1, "name": "MacBook do João", "port": 47101, "id": "b3f1c2d4e5f60718"}
   ```
3. O cliente usa o IP de origem da resposta + `port`. Se vierem vários servidores, mostra a lista; o `id` identifica cada Mac e é a chave do pareamento salvo.

Alternativa: IP e porta fixos no arquivo de configuração do cliente.

## 4. Autenticação

- O Mac mostra um código de 6 dígitos, que muda a cada pareamento bem-sucedido.
- No primeiro contato, o cliente pede o código e chama `POST /api/v1/pair`, recebendo um `client_token` (64 caracteres hexadecimais).
- Toda requisição seguinte envia `Authorization: Bearer <client_token>`.
- Proteção: após 5 códigos errados em 1 minuto, o Mac recusa novas tentativas por 1 minuto (`429 too_many_attempts`).
- O Mac lista os consoles pareados e pode revogar qualquer um.
- O pareamento pode ser desligado no Mac ("confiar na rede local"). Nesse modo, o token é dispensado e a plataforma do cliente vem do corpo das requisições.

## 5. Rotas

Sem pareamento válido, toda rota exceto `info` e `pair` responde `401`.

### 5.1 `GET /api/v1/info` (sem autenticação)

```json
{"protocol": 1, "server": "ponte-mac/0.1.0", "name": "MacBook do João", "id": "b3f1c2d4e5f60718", "pairing_required": true}
```
O cliente envia o cabeçalho `X-Ponte-Protocol: 1` em todas as requisições. Versão major diferente → `426 protocol_mismatch`, e o cliente avisa e para.

### 5.2 `POST /api/v1/pair`

```json
{"code": "482913", "client": {"platform": "ps5", "name": "PS5 sala", "version": "0.1.0", "firmware": "13.60"}}
```
Resposta `200`: `{"client_token": "c9a1…"}`. Código errado → `401`. A plataforma informada aqui fica associada ao token.

### 5.3 `GET /api/v1/catalog?platform=ps5|switch`

Lista resumida dos itens **prontos**. Sem `platform`, usa a plataforma do token.

```json
{
  "generated_at": "2026-10-28T18:10:00Z",
  "items": [
    {
      "id": "8f3e0a1c9b2d4e57",
      "revision": "a41c09e2",
      "platform": "ps5",
      "kind": "exfat",
      "content": "base",
      "title": "Nome do Jogo",
      "title_id": "PPSA01234",
      "content_id": "UP0000-PPSA01234_00-XXXXXXXXXXXXXXXX",
      "version": "01.000.000",
      "required_firmware": "4.03",
      "size": 48318382080,
      "file_count": 1,
      "parent_title_id": null,
      "warnings": [],
      "added_at": "2026-10-27T22:10:00Z",
      "icon": "/api/v1/items/8f3e0a1c9b2d4e57/icon"
    }
  ]
}
```

Campos:
- `id`: 16 hexadecimais = início do SHA-256 de `<platform>:<caminho relativo do item na biblioteca>`. Estável enquanto o item não for movido ou renomeado.
- `revision`: 8 hexadecimais que mudam sempre que o conteúdo do item muda (derivados de tamanho e data de modificação de cada arquivo). Ver §5.5.
- `kind`: um dos valores de §1.
- `content`: `base` | `update` | `dlc` | `unknown`.
- `parent_title_id`: para `update` e `dlc`, o jogo base. O cliente agrupa por ele.
- `required_firmware`: sempre no formato `X.YY`, quando conhecido.
- `warnings`: lista de códigos — `experimental_format`, `metadata_from_filename`, `duplicate_title_id`, `profile_guessed`, `unknown_compatibility`. Os que dependem de perfil (`experimental_format`, `unknown_compatibility`, `profile_guessed`) só aparecem quando o firmware do console é conhecido: o informado no pareamento ou o último enviado em `console/state`.
- `title_id`, `content_id`, `version`, `required_firmware` e `icon` podem ser `null`. `title` nunca é vazio: na falta de nome, usa o nome do arquivo sem extensão.
- Ordem: por `title`, sem diferenciar maiúsculas.

### 5.4 `GET /api/v1/items/{id}`

Os campos de §5.3 mais a lista de arquivos:

```json
{
  "id": "8f3e0a1c9b2d4e57",
  "revision": "a41c09e2",
  "kind": "folder",
  "files": [
    {"index": 0, "path": "eboot.bin", "size": 31457280, "sha256": "ab12…"},
    {"index": 1, "path": "sce_sys/param.json", "size": 4096, "sha256": null}
  ]
}
```

- `path`: relativo à raiz do item, sempre com `/`, sem `..`, sem barra inicial, sem componentes vazios. O cliente rejeita qualquer caminho fora disso.
- Ordem dos arquivos: alfabética por `path` (ordem de bytes UTF-8), para que `index` seja estável.
- Arquivos que o Finder cria sozinho (`.DS_Store` e `._*`) nunca fazem parte de um item.
- Itens de arquivo único têm um único arquivo, com `path` igual ao nome do arquivo.
- `sha256` pode ser `null` enquanto o Mac calcula.
- Os arquivos laterais (`.ponte.json`, `.ponte.png`) nunca aparecem aqui.

### 5.5 `GET /api/v1/items/{id}/files/{index}`

Conteúdo binário. Cabeçalhos da resposta:
- `Content-Length`, `Accept-Ranges: bytes`
- `X-Ponte-Revision: <revision>`
- `X-Ponte-Sha256: <hash>` quando conhecido

Retomada:
- O cliente envia `Range: bytes=INICIO-` (ou `INICIO-FIM`) **junto com** `X-Ponte-Revision: <revision que ele tinha>`.
- Se a revisão do item mudou, o servidor responde `412 revision_changed` e o cliente recomeça aquele item do zero.
- `Range` válido → `206` + `Content-Range: bytes INICIO-FIM/TOTAL`. Sem `Range` → `200`.
- `Range` além do fim → `416 bad_range`. `INICIO` igual ao tamanho do arquivo também é `416`: o cliente deve ter checado que o arquivo já estava completo.

Para programas que baixam sem mandar cabeçalhos (o instalador de pacotes do PS5 recebe só uma URL):
- a rota aceita um nome no fim, que é ignorado: `/api/v1/items/{id}/files/{index}/{nome}` (ex.: `.../files/0/jogo.pkg`), para quem baixa enxergar a extensão;
- só nas rotas de arquivo, o token pode ir na query: `?token=<client_token>`, no lugar de `Authorization`.

### 5.6 `GET /api/v1/items/{id}/icon`

PNG ou JPEG com o `Content-Type` correto. `404` se não houver.

### 5.7 `POST /api/v1/console/state`

O cliente envia o que **já está no console**. Enviado ao abrir o app e após cada cópia.

```json
{
  "platform": "ps5",
  "firmware": "13.60",
  "storages": [
    {"id": "internal", "label": "Interno", "free": 412316860416, "total": 825000000000},
    {"id": "ext0", "label": "M.2", "free": 1700000000000, "total": 2000000000000},
    {"id": "usb0", "label": "USB", "free": 900000000000, "total": 1000000000000}
  ],
  "installed": [
    {"title_id": "PPSA01234", "version": "01.000.000", "content": "base", "storage": "ext0"},
    {"title_id": "CUSA00001", "version": "01.05", "content": "update", "storage": "internal"}
  ]
}
```
No Switch, `storages` são `nand` e `sd`, e `installed` vem da lista de aplicativos e versões do sistema (versão como inteiro, em texto: `"65536"`).

Resposta: o catálogo (§5.3) com dois campos extras por item:
- `state`, decidido nesta ordem (vale o primeiro que se aplicar):
  1. `incompatible` — o perfil marca o `kind` como `blocked`, ou `required_firmware` é maior que o firmware do console.
  2. `installed` — há no console o mesmo `title_id` e `content` com versão **maior ou igual**.
  3. `update_available` — há o mesmo `title_id` e `content`, mas com versão **menor**.
  4. `new` — caso contrário (inclusive quando o item não tem `title_id`).
- `state_reason`: texto curto para exibir, ou `null` (ex.: "pacote .pkg de PS5 não roda neste firmware").

Comparação de versões:
- PS4/PS5: separa por `.`, compara cada parte como número (`01.000.000` = `1.0.0`; `01.05` > `01.4`). Partes faltando valem 0.
- Switch: compara como inteiro.
- Firmware `X.YY`: compara como (X, YY) numérico.
- Versão `null` de qualquer lado → considera igual (`installed`), para não sugerir cópia à toa.

### 5.8 `POST /api/v1/items/{id}/report`

Resultado final de cada cópia:
```json
{"result": "installed", "storage": "ext0", "detail": null, "duration_s": 512}
```
`result`: `installed` | `copied` | `failed` | `cancelled`. `copied` = "arquivo no lugar, aguardando o montador registrar". Resposta `204`.

### 5.9 `GET /api/v1/catalog/changes?since=<generated_at>`

```json
{"changed": true, "new_items": 2, "generated_at": "2026-10-28T18:20:00Z"}
```
`new_items` conta itens com `added_at` posterior a `since`. `since` inválido → `400 bad_request`.

## 6. Erros

```json
{"error": {"code": "item_not_ready", "message": "Ainda extraindo"}}
```

| HTTP | `code` | Quando |
|---|---|---|
| 400 | `bad_request` | parâmetro ou JSON inválido |
| 401 | `unauthorized` | token ausente/inválido ou código de pareamento errado |
| 404 | `not_found` | rota, item ou arquivo inexistente |
| 409 | `item_not_ready` | item existe mas ainda está sendo preparado |
| 412 | `revision_changed` | o item mudou desde o início da cópia |
| 416 | `bad_range` | `Range` inválido |
| 426 | `protocol_mismatch` | versão major incompatível |
| 429 | `too_many_attempts` | tentativas de pareamento demais |

## 7. Lado do Mac: ciclo de vida de um item

```
detectado → estável → (extraindo) → identificando → (convertendo) → verificando → pronto
                ↘───────────────────────↘──────────────────↘──→ precisa de atenção
```

### 7.1 Estável
Tamanho e data de modificação parados por 10 s, e nenhum arquivo de download em andamento por perto (`.part`, `.crdownload`, `.download`, `.tmp`). Para pastas, vale para todos os arquivos dentro.

### 7.2 Identificando
Ordem das regras: arquivo lateral (§1.6) → pasta com `sce_sys/param.json` → pasta com `sce_sys/param.sfo` → extensão de imagem PS5 → `.pkg` (§1.4) → extensões do Switch → compactado (§1.3). Sem regra segura → "precisa de atenção", onde você escolhe plataforma, tipo e Title ID à mão (o que grava o arquivo lateral).

O Mac procura itens até 2 níveis abaixo da raiz da biblioteca. Uma pasta reconhecida como jogo não é percorrida por dentro em busca de outros itens.

### 7.3 Duplicados
Dois itens com o mesmo `platform`, `title_id`, `content` e `version` → o mais recente (`added_at`) recebe `duplicate_title_id`. Ambos continuam no catálogo.

### 7.4 Convertendo (PS5 `folder` → `exfat`, opcional, desligado por padrão no app)
Sem conversão, a pasta entra no catálogo como `folder` assim que fica estável.

Mesmo método do `mkexfat_macos.sh` do ShadowMountPlus:
1. Exige `eboot.bin` na raiz da pasta.
2. Cluster: 64 KiB quando o tamanho médio dos arquivos é ≥ 1 MiB; senão 32 KiB. Configurável (forçar 64 KiB).
3. Tamanho da imagem: soma dos arquivos arredondados para cima por cluster + 4 bytes por cluster (FAT) + 1 bit por cluster (bitmap) + 256 bytes por arquivo e por pasta + 32 MiB fixos; folga = 0,5% disso, limitada entre 64 MiB e 512 MiB; total no mínimo (dados brutos + 64 MiB); arredondado para cima em MiB.
4. `mkfile -n <MiB>m` → `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -nobrowse` (a imagem não aparece no Finder) → `newfs_exfat -b <cluster> -v <TITLE_ID>` no dispositivo inteiro (sem tabela de partição) → `diskutil mount -mountPoint <pasta>` (não precisa de `sudo`; se falhar, `mount -t exfat`) → copiar o conteúdo da pasta para a raiz da imagem → desmontar e `hdiutil detach`.
   A cópia é feita arquivo a arquivo, sem atributos estendidos (o exFAT os guardaria como `._*`) e sem `.DS_Store`.
5. Grava como `<TITLE_ID>.exfat.ponte-tmp` e renomeia para `<TITLE_ID>.exfat` no fim. Em qualquer falha: desmonta, desanexa, apaga o temporário, mantém a pasta e marca "precisa de atenção".
6. Antes de começar, confere espaço livre ≥ tamanho calculado.
7. Grava o arquivo lateral com os metadados da pasta e copia `sce_sys/icon0.png` para `<TITLE_ID>.exfat.ponte.png`.
8. Depois do sucesso, a pasta original é apagada ou movida para `_originais/` (configurável).

### 7.5 Verificando (opcional, desligado por padrão no app)
`sha256` de cada arquivo em segundo plano. O item entra no catálogo antes de terminar (`sha256: null`).
Com a conferência desligada, o servidor não calcula nem informa hashes (`sha256: null` sempre, sem `X-Ponte-Sha256`); o cliente confere só o tamanho e não relê o que já tinha numa retomada.

### 7.6 Cópias em andamento e histórico (só no Mac)
O Mac mede as cópias pelo que ele mesmo serve; nada muda para os clientes.
- Uma cópia é o par (item, console). Console = o token (o instalador do PS5 usa o mesmo, via `?token=`) ou o IP sem pareamento.
- Progresso: para cada arquivo, o maior ponto já entregue (início do `Range` + bytes enviados), então o que o console já tinha numa retomada conta. Velocidade: bytes enviados nos últimos 5 s.
- Vai para o histórico quando chega o `report` (§5.8), ou como `interrupted` depois de 2 min sem envio e sem `report`.
- `.pkg`: o `report` `installed` chega antes de o instalador do sistema baixar o pacote. O download que vier depois (até 30 min) completa o mesmo registro; ele fecha 30 s depois do último envio.
- O histórico (últimas 500 cópias) fica na pasta de estado do Mac (`history.json`), nunca na biblioteca.

## 8. Lado do cliente: regras

1. Ao abrir: descobre o Mac, envia `console/state`, mostra o catálogo anotado, agrupado por jogo (base + atualizações + DLC).
2. Você escolhe os itens e o armazenamento. O cliente confere espaço livre (`size` + 5% de margem) antes de começar.
3. **Área temporária fora de `homebrew/`**: tudo é baixado em `<destino>/ponte/tmp/<id>/`, que o montador não vigia. Só ao final o item é **movido** (renomeação no mesmo disco, instantânea) para `<destino>/homebrew/`. Isso impede o montador de registrar um jogo pela metade.
4. Retomada: se `<destino>/ponte/tmp/<id>/` existir de uma cópia anterior com a mesma `revision` (gravada em `ponte/tmp/<id>/.revision`), continua de onde parou com `Range`; senão, apaga e recomeça.
5. Confere tamanho e, se houver, `sha256` de cada arquivo antes de mover ou instalar.
6. Destino já ocupado (mesmo nome em `homebrew/`) → pergunta se substitui.
7. `pkg_*`: no PS5, o cliente entrega ao instalador do sistema a URL do arquivo no Mac (`.../files/0/<nome>.pkg?token=…`, §5.5) e o instalador baixa direto, sem passar pela área temporária (não precisa de espaço em dobro). Report `installed` = o sistema aceitou a instalação; ela continua em segundo plano no console.
8. Ordem dentro de um jogo: base → atualização → DLC.
9. Sempre envia `report`, inclusive em falha ou cancelamento.
10. Caminhos (`homebrew/`, `ponte/tmp/`, raízes de cada armazenamento) ficam no arquivo de configuração do cliente.

### 8.1 Cliente do Switch (app Ponte)

No Switch o jogo não é copiado para depois ser instalado: o arquivo vai **direto da rede para o instalador** do Sphaira (yati), conteúdo por conteúdo. Uma cópia prévia no cartão exigiria o dobro do espaço. O princípio de nunca gravar direto no destino final continua valendo: o instalador escreve em *placeholders* do sistema e só registra o conteúdo quando ele chega inteiro e conferido. Por isso, em relação às regras acima:

- **Sem área temporária** (regras 3, 4 e 7 não se aplicam). O instalador lê o arquivo com `Range` aberto (`bytes=N-`) e, se a conexão cair, pede de novo a partir de onde parou por até 60 s. Toda requisição de arquivo leva `X-Ponte-Revision`; `412` encerra aquele item como `failed` (o console atualiza a lista e o item pode ser escolhido de novo).
- **Conferência** (regra 5): no lugar do `sha256` do arquivo, o instalador confere o SHA-256 de cada NCA contra o CNMT. `X-Ponte-Sha256` é ignorado.
- **Espaço** (regra 2): soma dos `size` escolhidos + 5% contra o `free` do armazenamento de destino (`sd` ou `nand`), escolhido na hora de instalar. Para `nsz`/`xcz` isso é uma estimativa por baixo: instalado, o conteúdo ocupa o tamanho descomprimido, que o Mac não informa.
- **Ordem** (regra 8): por jogo, base → atualizações da mais antiga para a mais nova → DLC. Uma falha não interrompe os itens seguintes; um cancelamento, sim.
- **`report`** (regra 9): `installed` ao terminar, `failed` com `detail`, ou `cancelled`. Nunca `copied`.
- **Escolha rápida**: marcar um jogo que não está no console marca junto a atualização mais nova e os DLC que faltam.

`console/state` no Switch:
- `firmware`: versão do sistema como `X.YY` (ex.: 19.0.1 → `19.00`).
- `storages`: `nand` (memória do console) e `sd`.
- `installed`: um registro por conteúdo instalado, com o **Title ID do próprio conteúdo** (atualização termina em `800`; DLC usa o seu), `version` inteira em texto e `content` pelo tipo do meta (`Application` → `base`, `Patch` → `update`, `AddOnContent` → `dlc`).

Outros detalhes do cliente:
- Descoberta: envia `PONTE?1` ao broadcast da sub-rede e a `255.255.255.255`, repetindo a cada 400 ms por ~1,2 s.
- Pareamento: o nome do console é o apelido configurado no Switch. `401` em `console/state` com token salvo = pareamento revogado no Mac: o token é apagado e o código é pedido de novo.
- Com a biblioteca aberta, consulta `catalog/changes` a cada 20 s e atualiza a lista quando há novidades.
- Configuração: `/config/sphaira/ponte.ini`, uma seção por `id` de servidor (`name`, `address`, `token`).

## 9. Em aberto

- [ ] Ler `param.json` de dentro de `.exfat` sem metadados laterais (imagens que não foram criadas pelo Ponte). Leitor de exFAT somente leitura, só para achar um arquivo.
- [ ] Idem para `.ffpkg` (UFS) e `.ffpfs` (PFS), que o macOS não monta.
- [ ] Metadados completos de XCI/XCZ e nome dos jogos de Switch (exigem `prod.keys`).
- [ ] Carregador para `folder_ps4`.
- [ ] Registrar jogos direto pelo payload, sem depender do montador externo.
- [x] Progresso da cópia visível no Mac em tempo real — resolvido só do lado do Mac, sem mudar o protocolo (§7.6).

## 10. Histórico

**rascunho 4** (o protocolo continua `1`: só acréscimos compatíveis)
- Conversão para `.exfat` (§7.4), conferência por `sha256` (§7.5) e extração de compactados (§1.3) passam a ser opcionais e desligadas por padrão no app do Mac; sem hash, o cliente confere só o tamanho.
- §8.1: regras do cliente do Switch, o app Ponte (instalação direto da rede, sem área temporária; conteúdo de `console/state` no Switch).
- Rota de arquivo aceita `?token=` e um nome no fim, para o instalador do PS5 baixar `.pkg` direto do Mac (§5.5, §8 item 7).
- `.DS_Store` e `._*` ficam fora dos itens (§5.4).
- Extração de compactados detalhada: extrator, pasta temporária, achatamento (§1.3).
- Conversão para `.exfat`: montagem com `diskutil` sem `sudo`, rótulo do volume e cópia sem atributos estendidos (§7.4).
- `tools/conformance.py`: teste caixa-preta da especificação, usado na referência em Python e no servidor Swift.
- Cópias em andamento e histórico no Mac, medidos pelo servidor (§7.6); fecha o item de §9 sem mudar o protocolo.
- Marcadores de download em andamento (§7.1) revistos; a regra dos 10 s parados continua valendo para qualquer programa. Sem mudança no protocolo.
- Textos: o perfil 13.60 e o exemplo de `state_reason` (§1.1.1, §5.7) falam em "pacote .pkg de PS5". Sem mudança no protocolo.

**rascunho 3**
- Área temporária movida para fora de `homebrew/` (o montador poderia registrar um jogo pela metade).
- `id` estável definido; `revision` + `412 revision_changed` para não retomar sobre um arquivo que mudou.
- Ordem de `files` e regras de `path` fixadas, para `index` ser estável.
- Perfis de compatibilidade por firmware em arquivo, com regra de escolha.
- Regras de `state` com precedência e comparação de versões por plataforma.
- Regras exatas de Title ID do Switch e de leitura de `.pkg`, `param.sfo` e `param.json`.
- Arquivo lateral `.ponte.json` para preservar metadados após a conversão para `.exfat`.
- Keep-alive, `HEAD`, inteiros de 64 bits, `X-Ponte-Protocol`, limite de tentativas de pareamento, erros 400/412/429.
- `region` removido do catálogo (sem fonte confiável).

**rascunho 2** — todos os formatos de PS5 e Switch; `console/state`; perfil 13.60; conversão pasta → `.exfat`.

**rascunho 1** — versão inicial.
