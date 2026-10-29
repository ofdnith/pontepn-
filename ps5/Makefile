# Payload Ponte para o PS5.
#
#   make                 payload do PS5 (precisa do SDK: export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk)
#   make host            versão de teste para Linux/macOS (o "console" é uma pasta)
#   make test            testa a versão de teste contra o servidor do Mac (referência em Python)
#   make selftest        confere o sha256 (SHA-NI contra o C portátil) e mede a velocidade
#   make send PS5_HOST=192.168.0.20   envia o payload para o carregador de ELF (porta 9021)

SRC := src/main.c src/tile.c src/http.c src/ponte.c src/jobs.c src/diag.c src/util.c src/platform_common.c src/vendor/cJSON.c
GEN := build/ui_page.h build/ui_icon.h
CFLAGS_COMMON := -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -O2 -Isrc -Ibuild

PS5_HOST ?= ps5
PS5_PORT ?= 9021

.PHONY: all host test selftest clean send

all: build/ponte.elf

build/ui_page.h: ui/index.html tools/embed.py
	@mkdir -p build
	python3 tools/embed.py ui/index.html $@ UI_PAGE

build/ui_icon.h: assets/icon0.png tools/embed.py
	@mkdir -p build
	python3 tools/embed.py assets/icon0.png $@ UI_ICON

# Nenhuma biblioteca da Sony é ligada na compilação: um .sprx que falte ou dependa de outro
# (libSceAppInstUtil precisa de libSceIpmi antes) faz o carregador recusar o ELF em silêncio.
# Tudo da Sony é resolvido na hora do uso, com dlopen/dlsym (src/platform_ps5.c).
build/ponte.elf: $(SRC) src/platform_ps5.c $(GEN) $(wildcard src/*.h)
ifndef PS5_PAYLOAD_SDK
	$(error PS5_PAYLOAD_SDK não definido: instale o SDK de payloads do PS5)
endif
	@mkdir -p build
	$(PS5_PAYLOAD_SDK)/bin/prospero-clang $(CFLAGS_COMMON) -Wno-unknown-warning-option -Wno-unreachable-code-generic-assoc -o $@ $(SRC) src/platform_ps5.c \
		-lpthread
	$(PS5_PAYLOAD_SDK)/bin/prospero-strip $@
	@# trava: só as bibliotecas que qualquer payload do SDK já usa
	@extra=$$(readelf -d $@ | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | \
		grep -v -x -e libkernel_web.sprx -e libSceLibcInternal.sprx -e libSceNet.sprx || true); \
	if [ -n "$$extra" ]; then echo "ponte.elf depende de bibliotecas extras: $$extra" >&2; rm -f $@; exit 1; fi

host: build/ponte-host build/sha256-selftest

build/ponte-host: $(SRC) src/platform_host.c $(GEN) $(wildcard src/*.h)
	@mkdir -p build
	$(CC) $(CFLAGS_COMMON) -DPONTE_HOST -g -o $@ $(SRC) src/platform_host.c -lpthread

build/sha256-selftest: tests/sha256_selftest.c src/util.c src/util.h
	@mkdir -p build
	$(CC) $(CFLAGS_COMMON) -o $@ tests/sha256_selftest.c src/util.c

selftest: build/sha256-selftest
	build/sha256-selftest

test: build/ponte-host build/sha256-selftest
	build/sha256-selftest
	python3 tests/test_payload.py

send: build/ponte.elf
	$(PS5_PAYLOAD_SDK)/bin/prospero-deploy -h $(PS5_HOST) -p $(PS5_PORT) $<

clean:
	rm -rf build
