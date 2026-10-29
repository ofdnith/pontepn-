#!/usr/bin/env python3
"""Confere a documentação: pares inglês/português, seletor de idioma, links e âncoras.

Regras (CLAUDE.md):
- `NOME.md` é a versão em inglês e `NOME.pt-BR.md` a versão em português; uma não existe sem a outra,
  e cada uma tem um link para a outra (o seletor de idioma).
- Exceções: a SPEC (oficial em `protocol/SPEC.md`, em português; tradução em `protocol/SPEC.en.md`),
  `CLAUDE.md`, as notas de versão em `docs/releases/` e licenças de terceiros.
- Todo link relativo aponta para um arquivo que existe; toda âncora (`#...`) existe no arquivo de destino,
  com as regras do GitHub para gerar âncoras a partir dos títulos.

Uso: python3 tools/check_docs.py   (sai com 1 se achar problema)
"""
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent

# Arquivos fora da regra de pares.
UNPAIRED = {"CLAUDE.md"}
SPEC_PAIRS = {"protocol/SPEC.md": "protocol/SPEC.en.md", "protocol/SPEC.en.md": "protocol/SPEC.md"}


def md_files():
    out = subprocess.run(["git", "ls-files", "-co", "--exclude-standard", "*.md"],
                         cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return sorted(p for p in out.splitlines() if (ROOT / p).exists())


def exempt(rel):
    return (rel in UNPAIRED or rel.startswith("docs/releases/") or "LICENSE" in rel
            or rel.startswith("switch/app/sphaira/"))


def partner(rel):
    if rel in SPEC_PAIRS:
        return SPEC_PAIRS[rel]
    if rel.endswith(".pt-BR.md"):
        return rel[: -len(".pt-BR.md")] + ".md"
    return rel[: -len(".md")] + ".pt-BR.md"


def strip_code(text):
    """Tira blocos de código cercados (links e títulos ali dentro não contam)."""
    lines, fenced = [], False
    for line in text.splitlines():
        if line.lstrip().startswith("```"):
            fenced = not fenced
            lines.append("")
            continue
        lines.append("" if fenced else line)
    return "\n".join(lines)


def slug(heading):
    h = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", heading)  # [texto](link) -> texto
    h = h.replace("`", "").strip().lower()
    h = re.sub(r"[^\w\- ]", "", h)
    return h.replace(" ", "-")


def anchors(path, cache={}):
    if path not in cache:
        seen, result = {}, set()
        for line in strip_code(path.read_text(encoding="utf-8")).splitlines():
            m = re.match(r"^#{1,6}\s+(.*?)\s*#*\s*$", line)
            if not m:
                continue
            s = slug(m.group(1))
            n = seen.get(s, 0)
            seen[s] = n + 1
            result.add(s if n == 0 else f"{s}-{n}")
        cache[path] = result
    return cache[path]


LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")


def main():
    files = md_files()
    problems = []
    for rel in files:
        path = ROOT / rel
        text = strip_code(path.read_text(encoding="utf-8"))

        if not exempt(rel):
            other = partner(rel)
            if not (ROOT / other).exists():
                problems.append(f"{rel}: falta a outra língua ({other})")
            elif not any(Path(t).name == Path(other).name for t in re.findall(r"\]\(([^)#\s]+)", text)):
                problems.append(f"{rel}: falta o seletor de idioma (link para {Path(other).name})")

        for target in LINK.findall(text):
            if re.match(r"^[a-z]+:", target):  # http:, https:, mailto:
                continue
            target = unquote(target)
            file_part, _, anchor = target.partition("#")
            dest = path if not file_part else (path.parent / file_part).resolve()
            if not dest.exists():
                problems.append(f"{rel}: link quebrado → {target}")
                continue
            if anchor and dest.suffix == ".md" and anchor not in anchors(dest):
                problems.append(f"{rel}: âncora inexistente → {target}")

    for p in problems:
        print(p)
    print(f"{len(files)} arquivos .md conferidos, {len(problems)} problema(s).")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
