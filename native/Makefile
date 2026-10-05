CC ?= gcc
NATIVE := /repos/build/Laplace-Native/icx-release
ICU := /repos/deps/icu78
INTEL := /opt/intel/oneapi/compiler/latest/lib
PG := /usr/local/pgsql
CFLAGS := -O3 -march=native -std=c11 -Wall -Wextra -fPIC -I/repos/src/Laplace-Native/include -I/repos/src/tree-sitter/lib/include -I$(PG)/include -I.
LIBS := $(NATIVE)/liblaplace_text.a -L$(NATIVE) -llaplace -L$(ICU)/lib -licui18n -licuuc -licudata -L$(INTEL) -lirc -ltree-sitter -ldl -lpthread -lm
RPATH := -Wl,--disable-new-dtags -Wl,-rpath,$(NATIVE) -Wl,-rpath,$(ICU)/lib -Wl,-rpath,$(INTEL) -Wl,-rpath,$(PG)/lib

.PHONY: all clean

all: liblpm.so lpm

liblpm.so: lpm.c lpm.h
	$(CC) $(CFLAGS) -shared -o $@ lpm.c $(LIBS) $(RPATH)

ENGINE_SRC := /repos/src/Laplace-Engine/src

lpm: main.c api.c api.h liblpm.so lpm.h db_bridge.c $(ENGINE_SRC)/read.c
	$(CC) $(CFLAGS) -fopenmp -I$(ENGINE_SRC) -o $@ main.c api.c db_bridge.c $(ENGINE_SRC)/read.c -L. -llpm $(LIBS) -L$(PG)/lib -lpq $(RPATH) -Wl,-rpath,'$$ORIGIN' -fopenmp

clean:
	rm -f liblpm.so lpm
