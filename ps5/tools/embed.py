#!/usr/bin/env python3
"""Gera um cabeçalho C com um arquivo de texto embutido: embed.py entrada.html saida.h NOME"""
import sys

src, dst, name = sys.argv[1:4]
data = open(src, "rb").read()
lines = []
for i in range(0, len(data), 96):
    chunk = data[i:i + 96]
    lines.append('"' + "".join(f"\\x{b:02x}" for b in chunk) + '"')
with open(dst, "w") as f:
    f.write(f"/* gerado de {src.split('/')[-1]} por tools/embed.py; não edite */\n")
    f.write(f"static const char {name}[] =\n" + "\n".join(lines) + ";\n")
    f.write(f"#define {name}_LEN ((size_t)(sizeof({name}) - 1))\n")
