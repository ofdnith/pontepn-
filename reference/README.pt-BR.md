# Implementação de referência (Python)

[English](README.md) · **Português**

Gabarito executável do [protocolo](../protocol/SPEC.md). O app do Mac em Swift deve se comportar igual a este servidor; o cliente falso em [`tools/fake_client.py`](../tools/fake_client.py) valida os dois.

Só usa a biblioteca padrão do Python (3.10+). Os testes usam `pytest`.

## O que tem aqui

| Módulo | O quê | SPEC |
|---|---|---|
| `ponte_ref/identify.py` | identifica plataforma, tipo e metadados de cada item | §1, §7.2 |
| `ponte_ref/formats/` | leitores de `param.sfo`, `.pkg`, `param.json`, PFS0 (NSP/NSZ), Title ID do Switch, compactados | §1.2–§1.5 |
| `ponte_ref/library.py` | varredura da biblioteca, estabilidade, `id`/`revision`, duplicados, hashes | §5.3, §5.4, §7 |
| `ponte_ref/profiles.py` | perfis de compatibilidade por firmware (`../profiles/`) | §1.1.1 |
| `ponte_ref/state.py` | cruzamento com o que já está no console | §5.7 |
| `ponte_ref/exfat_plan.py` | tamanho e cluster da imagem `.exfat` | §7.4 |
| `ponte_ref/server.py` | servidor HTTP + descoberta UDP + pareamento | §2–§6 |

O que **não** está aqui, porque depende do macOS e fica no app Swift: extrair compactados e criar a imagem `.exfat` (a referência só calcula o plano e lista os comandos).

## Rodar o servidor

```sh
cd reference
python3 -m ponte_ref.server --library /Volumes/SSD/Jogos
```

Ele mostra o código de pareamento. Opções: `--port` (47101), `--discovery-port` (47100), `--no-pairing`, `--stable-seconds` (10), `--rescan` (5), `--state-dir` (`~/.ponte-ref`).

## Testar com o cliente falso

Em outro terminal, na raiz do repositório:

```sh
python3 tools/fake_client.py discover          # acha o servidor na rede
python3 tools/fake_client.py pair 123456       # código mostrado pelo servidor
python3 tools/fake_client.py catalog
python3 tools/fake_client.py download <id> --dest ./console-falso
python3 tools/fake_client.py state --installed instalados.json
```

`./console-falso` imita o console: `ponte/tmp/` é a área temporária, `homebrew/` é onde o montador do PS5 procuraria jogos, `installed/` simula pacotes instalados.

## Testes

```sh
python3 -m pytest reference/tests -q
```

Cobrem cada formato com arquivos sintéticos, a varredura da biblioteca e o protocolo de ponta a ponta: pareamento e limite de tentativas, catálogo, `Range`, retomada depois de queda, revisão alterada no meio da cópia (`412`), hash errado, destino ocupado, item ainda não pronto (`409`) e caminhos maliciosos.
