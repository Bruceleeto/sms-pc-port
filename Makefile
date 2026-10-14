# Super Mario Sunshine PC port (Linux)
#
#   make assets        extract the disc image in rom/ into game/
#   make -j6           build build/linux-32/sms (ARCH=64 for 64-bit)
#   make run           play (from game/ after make assets, else the image in rom/)
#   make standalone    build sms-standalone, with the game's files inside
#   make clean         delete build/
#   make assets-clean  delete game/
#   make distclean     both
#   make update        pull upstream (chasem-dev) and update the decomp
#
# The shell scripts (build.sh, run.sh, clean.sh) still work; see BUILD.md.

ARCH     ?= 32
BUILD    := build/linux-$(ARCH)
GAME     := game
EXE      := $(BUILD)/sms
UPSTREAM := https://github.com/chasem-dev/sms-pc-port.git

# The first disc image in rom/ (its name may have spaces: always quoted below).
ROM := $(shell ls rom/*.iso rom/*.gcm rom/*.ciso rom/*.ISO rom/*.GCM rom/*.CISO 2>/dev/null | head -n 1)

ifeq ($(filter $(ARCH),32 64),)
$(error ARCH must be 32 or 64)
endif

.DEFAULT_GOAL := all
.PHONY: all assets run standalone clean assets-clean distclean update help FORCE

all: $(EXE)

# The decomp submodule, checked out on first use.
decomp/configure.py:
	git submodule update --init decomp

$(BUILD)/CMakeCache.txt: | decomp/configure.py
	@if [ $(ARCH) = 32 ] && ! echo 'int main(){return 0;}' | $${CXX:-g++} -m32 -x c++ - -o /dev/null 2>/dev/null; then \
		echo "g++ -m32 does not link: install the 32-bit packages (BUILD.md#linux), or use ARCH=64." >&2; exit 1; fi
	cmake -S . -B $(BUILD) -G "Unix Makefiles" -DSMS_ARCH=$(ARCH) -DSMS_GX_BUILD_TESTS=OFF \
		-DSMS_BUNDLE_DISC="$(if $(ROM),$(CURDIR)/$(ROM))"

# CMake tracks the sources; the recursive make shares this make's -j.
$(EXE): $(BUILD)/CMakeCache.txt FORCE
	+$(MAKE) -C $(BUILD) --no-print-directory sms
	@echo "Built $(EXE). Play: make run"

standalone: $(EXE)
	@[ -n "$(ROM)" ] || { echo "No disc image in rom/ (.iso, .gcm or .ciso)." >&2; exit 1; }
	cmake -B $(BUILD) -DSMS_BUNDLE_DISC="$(CURDIR)/$(ROM)"
	+$(MAKE) -C $(BUILD) --no-print-directory sms_standalone

assets: $(GAME)/sys/fst.bin

$(GAME)/sys/fst.bin:
	@[ -n "$(ROM)" ] || { echo "No disc image in rom/: put your GMSE01 .iso, .gcm or .ciso there." >&2; exit 1; }
	python3 tools/extract_disc.py --disc "$(ROM)" --out $(GAME)
	@touch $@

run: $(EXE)
	@if [ -f $(GAME)/sys/fst.bin ]; then SMS_DISC_ROOT=$(GAME)/files ./$(EXE); \
	elif [ -n "$(ROM)" ]; then ./$(EXE) "$(ROM)"; \
	else echo "No game: run make assets with your disc image in rom/." >&2; exit 1; fi

clean:
	rm -rf build

assets-clean:
	rm -rf $(GAME) $(GAME).tmp

distclean: clean assets-clean

update:
	@git remote get-url upstream >/dev/null 2>&1 || git remote add upstream $(UPSTREAM)
	git pull upstream main
	git submodule update --init --recursive

help:
	@sed -n '3,11p' Makefile | sed 's/^# \{0,1\}//'
