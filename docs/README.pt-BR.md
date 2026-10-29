# Documentação

[English](README.md) · **Português**

Índice de tudo que está documentado no repositório. Visão geral e status: [`README.pt-BR.md`](../README.pt-BR.md).

## Para usar

| Quero… | Onde |
|---|---|
| Instalar e configurar o app do Mac (biblioteca, pareamento, ejetar o SSD) | [`mac/README.pt-BR.md`](../mac/README.pt-BR.md#usar-o-app) |
| Instalar o payload no PS5 e abrir a interface web | [`ps5/README.pt-BR.md`](../ps5/README.pt-BR.md#instalar-no-ps5) |
| Medir a velocidade da cópia no PS5 | [`ps5/README.pt-BR.md`](../ps5/README.pt-BR.md#diagnóstico-de-velocidade) |
| Instalar o app Ponte no Switch | [`switch/README.pt-BR.md`](../switch/README.pt-BR.md#instalar-no-console) |
| Saber quais formatos funcionam | [`protocol/SPEC.md` §1](../protocol/SPEC.md#1-formatos-suportados) |
| Ajustar compatibilidade por firmware | `profiles/` e [SPEC §1.1.1](../protocol/SPEC.md#111-perfis-de-compatibilidade) |

## Para desenvolver

| Assunto | Onde |
|---|---|
| Protocolo (rotas, erros, ciclo de vida, regras dos clientes) | [`protocol/SPEC.md`](../protocol/SPEC.md) |
| Regras de trabalho, mapa do código e estado atual | [`CLAUDE.md`](../CLAUDE.md) |
| Referência em Python e cliente falso | [`reference/README.pt-BR.md`](../reference/README.pt-BR.md) |
| Teste de conformidade do protocolo | [`tools/README.pt-BR.md`](../tools/README.pt-BR.md) |
| Compilar e testar o app do Mac | [`mac/README.pt-BR.md`](../mac/README.pt-BR.md#compilar-e-testar) |
| Compilar e testar o payload do PS5 | [`ps5/README.pt-BR.md`](../ps5/README.pt-BR.md#compilar) |
| Compilar e testar o app do Switch | [`switch/README.pt-BR.md`](../switch/README.pt-BR.md#compilar) |
| O que o Ponte muda no Sphaira | [`switch/app/PONTE.pt-BR.md`](../switch/app/PONTE.pt-BR.md) |
| CI e artefatos (`Ponte-Mac`, `Ponte-PS5`, `Ponte-Switch`) | `.github/workflows/mac-ps5.yml` e `.github/workflows/switch.yml` |
| Padrão visual: cores, barras de progresso, textos | [`DESIGN.pt-BR.md`](DESIGN.pt-BR.md) |
