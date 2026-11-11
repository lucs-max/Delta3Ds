# Delta3DS top-level Makefile
#
#   make            build both 3DS versions (needs devkitARM)
#   make new        New 3DS / New 2DS XL build  -> Delta3DS.3dsx
#   make old        Old 3DS / 2DS build         -> Delta3DS-old.3dsx
#   make cia        both builds packed as .cia (needs makerom + bannertool)
#   make host       desktop test runner         -> build_host/delta3ds_host
#   make test       run the test suite on the desktop
#   make clean

HOST_CC     ?= cc
HOST_CFLAGS ?= -std=gnu11 -O1 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
               -Wno-format-truncation -Wno-sign-compare
HOST_SRC    := $(wildcard src/core/*.c) src/host/main_host.c src/host/platform_host.c

.PHONY: all new old cia package-cia host test asan clean

# CIA packaging (FBI can install these from a QR code)
MAKEROM    ?= makerom
BANNERTOOL ?= bannertool
VERSION_MAJOR ?= 0
VERSION_MINOR ?= 1
VERSION_MICRO ?= 0
CIA_COMMON  = -f cia -rsf meta/app.rsf -icon build_cia/icon.icn -banner build_cia/banner.bnr -target t \
              -major $(VERSION_MAJOR) -minor $(VERSION_MINOR) -micro $(VERSION_MICRO)

all: new old

new:
	@$(MAKE) --no-print-directory -f Makefile.3ds CONSOLE=new

old:
	@$(MAKE) --no-print-directory -f Makefile.3ds CONSOLE=old

cia:
	@$(MAKE) --no-print-directory new old
	@$(MAKE) --no-print-directory package-cia

# Packs already-built Delta3DS.elf / Delta3DS-old.elf (used by CI).
package-cia: build_cia/banner.bnr build_cia/icon.icn
	$(MAKEROM) $(CIA_COMMON) -o Delta3DS.cia -elf Delta3DS.elf \
		-DAPP_TITLE="Delta3DS" -DAPP_PRODUCT_CODE="CTR-H-D3DS" -DAPP_UNIQUE_ID=0xD3D50 \
		-DAPP_SYSTEM_MODE=64MB -DAPP_SYSTEM_MODE_EXT=124MB -DAPP_CPU_SPEED=804MHz -DAPP_ENABLE_L2_CACHE=true
	$(MAKEROM) $(CIA_COMMON) -o Delta3DS-old.cia -elf Delta3DS-old.elf \
		-DAPP_TITLE="Delta3DS Old3DS" -DAPP_PRODUCT_CODE="CTR-H-D3DO" -DAPP_UNIQUE_ID=0xD3D51 \
		-DAPP_SYSTEM_MODE=64MB -DAPP_SYSTEM_MODE_EXT=Legacy -DAPP_CPU_SPEED=268MHz -DAPP_ENABLE_L2_CACHE=false

build_cia/silence.wav:
	@mkdir -p build_cia
	python3 -c "import wave; w=wave.open('$@','wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(22050); w.writeframes(bytes(22050)); w.close()"

build_cia/banner.bnr: meta/banner.png build_cia/silence.wav
	$(BANNERTOOL) makebanner -i meta/banner.png -a build_cia/silence.wav -o $@

build_cia/icon.icn: meta/icon.png
	@mkdir -p build_cia
	$(BANNERTOOL) makesmdh -s "Delta3DS" -l "GameMaker runner - play your own games" \
		-p "Delta3DS contributors" -i meta/icon.png -o $@

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
	@rm -rf build_host build_cia *.cia
