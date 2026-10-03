CC ?= gcc
NATIVE := /repos/build/Laplace-Native/icx-release
ICU := /repos/deps/icu78
INTEL := /opt/intel/oneapi/compiler/latest/lib
CFLAGS := -O3 -march=native -std=c11 -Wall -Wextra -fPIC -I/repos/src/Laplace-Native/include -I/repos/src/tree-sitter/lib/include -I.
LIBS := $(NATIVE)/liblaplace_text.a -L$(NATIVE) -llaplace -L$(ICU)/lib -licui18n -licuuc -licudata -L$(INTEL) -lirc -ltree-sitter -ldl -lpthread -lm
RPATH := -Wl,--disable-new-dtags -Wl,-rpath,$(NATIVE) -Wl,-rpath,$(ICU)/lib -Wl,-rpath,$(INTEL)

.PHONY: all clean

all: liblpm.so lpm

liblpm.so: lpm.c lpm.h
	$(CC) $(CFLAGS) -shared -o $@ lpm.c $(LIBS) $(RPATH)

lpm: main.c liblpm.so lpm.h
	$(CC) $(CFLAGS) -o $@ main.c -L. -llpm -L$(NATIVE) -llaplace $(RPATH) -Wl,-rpath,'$$ORIGIN'

clean:
	rm -f liblpm.so lpm
