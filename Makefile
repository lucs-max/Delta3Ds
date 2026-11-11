# Delta3DS top-level Makefile
#
#   make            build both 3DS versions (needs devkitARM)
#   make new        New 3DS / New 2DS XL build  -> Delta3DS.3dsx
#   make old        Old 3DS / 2DS build         -> Delta3DS-old.3dsx
#   make host       desktop test runner         -> build_host/delta3ds_host
#   make test       run the test suite on the desktop
#   make clean

HOST_CC     ?= cc
HOST_CFLAGS ?= -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
               -Wno-format-truncation -Wno-sign-compare
HOST_SRC    := $(wildcard src/core/*.c) src/host/main_host.c src/host/platform_host.c

.PHONY: all new old host test asan clean

all: new old

new:
	@$(MAKE) --no-print-directory -f Makefile.3ds CONSOLE=new

old:
	@$(MAKE) --no-print-directory -f Makefile.3ds CONSOLE=old

host: build_host/delta3ds_host

build_host/delta3ds_host: $(HOST_SRC) $(wildcard src/core/*.h src/platform/*.h)
	@mkdir -p build_host
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_SRC) -lm

asan:
	@mkdir -p build_host
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=address,undefined -fno-omit-frame-pointer \
		-o build_host/delta3ds_host_asan $(HOST_SRC) -lm

test: host
	@tests/run_tests.sh build_host/delta3ds_host

clean:
	@$(MAKE) --no-print-directory -f Makefile.3ds CONSOLE=new clean || true
	@$(MAKE) --no-print-directory -f Makefile.3ds CONSOLE=old clean || true
	@rm -rf build_host
