CC ?= gcc
# Where everything is: laplace.env's names (source Laplace-Operations/laplace.env first, or set them); unset, the
# deployment target's.
LAPLACE_SRC ?= /repos/src
LAPLACE_DEPSRC ?= $(LAPLACE_SRC)
LAPLACE_BUILD ?= /repos/build
LAPLACE_DEPS ?= /repos/deps
LAPLACE_PG_DIR ?= /usr/local/pgsql
LAPLACE_ICU_DIR ?= $(LAPLACE_DEPS)/icu78
INTEL ?= /opt/intel/oneapi/compiler/latest/lib
NATIVE := $(LAPLACE_BUILD)/Laplace-Native/icx-release
ICU := $(LAPLACE_ICU_DIR)
PG := $(LAPLACE_PG_DIR)
CFLAGS := -O3 -march=native -std=c11 -Wall -Wextra -fPIC -I$(LAPLACE_SRC)/Laplace-Native/include -I$(LAPLACE_DEPSRC)/tree-sitter/lib/include -I$(PG)/include -I.
LIBS := $(NATIVE)/liblaplace_text.a -L$(NATIVE) -llaplace -L$(ICU)/lib -licui18n -licuuc -licudata -L$(INTEL) -lirc -ltree-sitter -ldl -lpthread -lm
RPATH := -Wl,--disable-new-dtags -Wl,-rpath,$(NATIVE) -Wl,-rpath,$(ICU)/lib -Wl,-rpath,$(INTEL) -Wl,-rpath,$(PG)/lib

.PHONY: all clean

all: liblpm.so lpm

liblpm.so: lpm.c lpm.h
	$(CC) $(CFLAGS) -shared -o $@ lpm.c $(LIBS) $(RPATH)

ENGINE_SRC := $(LAPLACE_SRC)/Laplace-Engine/src

lpm: main.c api.c api.h liblpm.so lpm.h db_bridge.c $(ENGINE_SRC)/read.c
	$(CC) $(CFLAGS) -fopenmp -I$(ENGINE_SRC) -o $@ main.c api.c db_bridge.c $(ENGINE_SRC)/read.c -L. -llpm $(LIBS) -L$(PG)/lib -lpq $(RPATH) -Wl,-rpath,'$$ORIGIN' -fopenmp

clean:
	rm -f liblpm.so lpm
