# Ferramentas

[English](README.md) · **Português**

## `fake_client.py`

Finge ser um console: descobre o servidor, pareia, lista o catálogo, informa o estado do console e copia itens seguindo as regras do cliente (SPEC §8): área temporária fora de `homebrew/`, retomada com `Range`, revisão, conferência de tamanho e `sha256`, e `report` no fim.

Não depende da implementação de referência, então serve para validar qualquer servidor Ponte, inclusive o app do Mac. Uso em [`reference/README.pt-BR.md`](../reference/README.pt-BR.md#testar-com-o-cliente-falso).

## `conformance.py`

Teste caixa-preta do protocolo: sobe um servidor Ponte como processo separado, sobre uma biblioteca sintética, e confere a SPEC só pela rede (descoberta, pareamento e limite de tentativas, catálogo, detalhes, ícone, `HEAD`/`Range`/`416`, `412` de revisão, `409`, hashes, `console/state` com perfis, `changes`, cópia completa e retomada com o cliente falso, token que sobrevive ao reinício, modo sem pareamento).

```sh
python3 tools/conformance.py --server python     # referência
python3 tools/conformance.py --server swift      # mac/.build/debug/ponte-server
python3 tools/conformance.py --cmd "meu-servidor {extra} --library {library} --port {port} ..."
```

Qualquer servidor que aceite as opções da referência (`--library`, `--port`, `--discovery-port`, `--state-dir`, `--profiles`, `--rescan`, `--stable-seconds`, `--name`, `--no-pairing`) e imprima `Código de pareamento: NNNNNN` ao subir pode ser testado.

## `check_docs.py`

Confere a documentação: cada `NOME.md` (inglês) tem o seu `NOME.pt-BR.md` (português) e vice-versa, os dois têm o seletor de idioma, e todo link relativo e toda âncora existem. Fora da regra de pares: a SPEC (oficial em `protocol/SPEC.md`, tradução em `protocol/SPEC.en.md`), o `CLAUDE.md` e as notas em `docs/releases/`. O CI roda a cada push.

```sh
python3 tools/check_docs.py
```
