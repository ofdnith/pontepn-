# Compila o núcleo do protocolo do cliente Switch para o PC e testa contra o
# servidor de referência. SPHAIRA_DIR aponta para o código do app (switch/app)
# por padrão; YYJSON_DIR, para o yyjson 0.12.0 (o mesmo do Sphaira).
SPHAIRA_DIR ?= ../app
YYJSON_DIR  ?= build/yyjson
CXXFLAGS    ?= -std=c++20 -O1 -g -Wall -Wextra -Werror

build/ponte_host: ponte_host.cpp $(SPHAIRA_DIR)/sphaira/source/utils/ponte_proto.cpp $(SPHAIRA_DIR)/sphaira/include/utils/ponte_proto.hpp | $(YYJSON_DIR)
	$(CC) -O1 -c $(YYJSON_DIR)/src/yyjson.c -o build/yyjson.o
	$(CXX) $(CXXFLAGS) -I$(SPHAIRA_DIR)/sphaira/include -I$(YYJSON_DIR)/src \
		ponte_host.cpp $(SPHAIRA_DIR)/sphaira/source/utils/ponte_proto.cpp build/yyjson.o -o $@

build/yyjson:
	mkdir -p build
	git clone -q --depth 1 -b 0.12.0 https://github.com/ibireme/yyjson.git $@

test: build/ponte_host
	build/ponte_host unit
	cd ../.. && SPHAIRA_DIR=$(abspath $(SPHAIRA_DIR)) python3 -m pytest switch/hosttest -q

clean:
	rm -rf build

.PHONY: test clean
