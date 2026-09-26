# Makefile — Noether ER-301 package
#
# Cross-compiles Noether.cpp + SWIG wrapper into
# lib/am335x/libnoether.so for the ER-301 (AM3358, ARM Cortex-A8).
#
# Follows er-301/scripts/tutorial.mk exactly for am335x flags:
#   - ARM flags: -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard -mabi=aapcs
#                -Dfar= -D__DYNAMIC_REENT__
#   - Linker: -nostdlib -nodefaultlibs -r  (relocatable ET_REL, NOT -shared)
#   - No -fPIC for am335x!  The firmware's ELF loader uses absolute relocations;
#     -fPIC generates GOT-relative code + _GLOBAL_OFFSET_TABLE_ which the loader
#     cannot resolve.
#   - SWIG flags: -no-old-metatable-bindings -nomoduleglobal -small -fvirtual -fcompact
#   - Lua headers: libs/lua54  (the ER-301 firmware runs Lua 5.4)
#
# ── PREREQUISITES ────────────────────────────────────────────────────────────
#
#   macOS (recommended path):
#     Install Docker Desktop, then:
#       make docker-image
#       make swig-docker ER301_SDK=~/er-301
#       make docker-build ER301_SDK=~/er-301
#
#   Linux / CI (native cross-compiler):
#     sudo apt-get install swig gcc-arm-none-eabi binutils-arm-none-eabi \
#                          libnewlib-arm-none-eabi libstdc++-arm-none-eabi-newlib
#     make TOOLCHAIN=native ER301_SDK=/path/to/er-301
#
# ── VARIABLES ─────────────────────────────────────────────────────────────────

# Path to the cloned odevices/er-301 repo (provides SDK headers).
ER301_SDK ?= $(HOME)/er-301

# Set TOOLCHAIN=native to use arm-none-eabi-g++ directly (Linux / CI).
# Default uses Docker (macOS-friendly).
TOOLCHAIN ?= docker

PKG     := noether
MODULE  := libnoether
VERSION := 0.8.1
ARCH    := am335x

SRCDIR  := src
OUTDIR  := lib/$(ARCH)
OUTLIB  := $(OUTDIR)/$(MODULE).so
OBJS_DIR := $(OUTDIR)/obj

SWIG_FILE := $(SRCDIR)/$(MODULE).swig
SWIG_WRAP := $(SRCDIR)/$(MODULE)_wrap.cpp

# ── COMPILER FLAGS ────────────────────────────────────────────────────────────

# Common flags shared by all compilation units.
CXXFLAGS_COMMON := \
	-std=c++11 \
	-ffunction-sections \
	-fdata-sections \
	-ffast-math \
	-fno-builtin-sincosf \
	-fno-stack-protector \
	-fno-exceptions \
	-D__DYNAMIC_REENT__ \
	-mabi=aapcs \
	-DNDEBUG \
	-D_GLIBCXX_USE_CXX11_ABI=0 \
	-I$(ER301_SDK) \
	-I$(ER301_SDK)/libs/lua54 \
	-I$(SRCDIR) \
	-Wall \
	-Wno-unused-parameter

# ARM Cortex-A8 flags — from tutorial.mk's CFLAGS.am335x.
CXXFLAGS_ARM := \
	-mcpu=cortex-a8 \
	-mfpu=neon \
	-mfloat-abi=hard \
	-Dfar=

# DSP code: optimise for speed + NEON vectorisation.
CXXFLAGS_DSP := $(CXXFLAGS_COMMON) -O2 $(CXXFLAGS_ARM)

# SWIG wrapper: optimise for size (lots of generated boilerplate).
# NOTE: NO -include compat_swig.h here — the #define fprintf macro conflicts
# with `using ::fprintf` in <cstdio>.  fprintf references in the wrap .o are
# resolved by compat.cpp at partial-link time.
CXXFLAGS_WRAP := $(CXXFLAGS_COMMON) -Os $(CXXFLAGS_ARM)

# compat.cpp: compiled WITHOUT -ffast-math so GCC does not combine
# sinf+cosf back into a recursive sincosf call inside our sincosf stub.
CXXFLAGS_COMPAT := \
	-std=c++11 \
	-ffunction-sections \
	-fdata-sections \
	-fno-stack-protector \
	-fno-exceptions \
	-D__DYNAMIC_REENT__ \
	-mabi=aapcs \
	-DNDEBUG \
	-D_GLIBCXX_USE_CXX11_ABI=0 \
	-I$(ER301_SDK) \
	-I$(SRCDIR) \
	-O1 \
	$(CXXFLAGS_ARM)

# Partial-link (relocatable ET_REL).  The ER-301 firmware's custom ELF loader
# resolves all symbols from its own static export table at load time.
LDFLAGS := -nostdlib -nodefaultlibs -r

# ── TOOLCHAIN ─────────────────────────────────────────────────────────────────

CXX   := arm-none-eabi-g++
STRIP := arm-none-eabi-strip

DOCKER_IMAGE := er301-crosscompile:latest

# ── OBJECT FILES ──────────────────────────────────────────────────────────────

OBJ_ENG    := $(OBJS_DIR)/Noether.o
OBJ_WRAP   := $(OBJS_DIR)/libnoether_wrap.o
OBJ_COMPAT := $(OBJS_DIR)/compat.o

# ── PHONY TARGETS ─────────────────────────────────────────────────────────────

.PHONY: all swig build docker-image docker-build swig-docker pkg dist clean help \
        hosttest check-api check-ports check-version prune-old-pkgs

all: help

# ── dist: one-shot cross-compile + package ────────────────────────────────────
# `make pkg` alone fails on macOS because the .so prerequisite falls back to a
# native arm-none-eabi compile (no host toolchain).  `dist` runs the Docker
# cross-build first, then packages — the reliable one-command path.
# REMINDER: bump VERSION (Makefile + assets/toc.lua) on every change you flash —
# the ER-301 caches the .so per version, so a same-version reinstall can keep
# running the stale binary.
dist:
	$(MAKE) swig-docker  ER301_SDK=$(ER301_SDK)
	$(MAKE) docker-build ER301_SDK=$(ER301_SDK)
	$(MAKE) pkg

# ── SWIG: generate the Lua binding wrapper ────────────────────────────────────
# Use `make swig-docker` instead if you don't have swig on your Mac.

swig: $(SWIG_WRAP)

$(SWIG_WRAP): $(SWIG_FILE) $(SRCDIR)/Noether.h $(SRCDIR)/NoetherDisplay.h $(SRCDIR)/NoetherReel.h $(SRCDIR)/NoetherScale.h $(SRCDIR)/NoetherLut.h
	@echo ">>> SWIG: generating Lua wrapper..."
	swig -c++ -lua \
		-no-old-metatable-bindings \
		-nomoduleglobal \
		-small \
		-fvirtual \
		-fcompact \
		-I$(ER301_SDK) \
		-I$(SRCDIR) \
		-o $@ $<
	@echo ">>> SWIG done: $@"

# ── TWO-PHASE BUILD ───────────────────────────────────────────────────────────
# Phase 1: compile each .cpp → .o
# Phase 2: partial-link all .o → .so

build: $(OUTLIB)
	@echo ">>> Built: $(OUTLIB)"

$(OUTDIR):
	mkdir -p $@

$(OBJS_DIR): | $(OUTDIR)
	mkdir -p $@

$(OBJ_ENG): $(SRCDIR)/Noether.cpp $(SRCDIR)/Noether.h $(SRCDIR)/NoetherReel.h $(SRCDIR)/NoetherScale.h $(SRCDIR)/NoetherLut.h | $(OBJS_DIR)
	@echo ">>> CC Noether.cpp"
	$(CXX) $(CXXFLAGS_DSP) -c -o $@ $<

$(OBJ_WRAP): $(SWIG_WRAP) | $(OBJS_DIR)
	@echo ">>> CC libnoether_wrap.cpp"
	$(CXX) $(CXXFLAGS_WRAP) -c -o $@ $<

$(OBJ_COMPAT): $(SRCDIR)/compat.cpp | $(OBJS_DIR)
	@echo ">>> CC compat.cpp"
	$(CXX) $(CXXFLAGS_COMPAT) -c -o $@ $<

$(OUTLIB): $(OBJ_ENG) $(OBJ_WRAP) $(OBJ_COMPAT) | $(OUTDIR)
	@echo ">>> LINK (relocatable) $(OUTLIB)"
	$(CXX) $(LDFLAGS) -o $@ $(OBJ_ENG) $(OBJ_WRAP) $(OBJ_COMPAT)
	$(STRIP) --strip-unneeded $(OUTLIB)

# ── DOCKER: build the cross-compile image ────────────────────────────────────

docker-image:
	docker build -t $(DOCKER_IMAGE) -f Dockerfile .

# ── SWIG inside Docker ────────────────────────────────────────────────────────
# Use this on macOS instead of `make swig` (no local swig needed).

swig-docker: docker-image
	$(eval SDK_ABS := $(shell realpath $(ER301_SDK) 2>/dev/null))
	@test -n "$(SDK_ABS)" || \
		{ echo "ERROR: ER301_SDK path '$(ER301_SDK)' does not exist."; \
		  echo "       Clone it first:  git clone https://github.com/odevices/er-301 ~/er-301"; \
		  exit 1; }
	@echo ">>> SWIG (inside Docker) ..."
	docker run --rm \
		-v "$(CURDIR)":/build \
		-v "$(SDK_ABS)":/er301_sdk \
		-w /build \
		$(DOCKER_IMAGE) \
		swig -c++ -lua \
			-no-old-metatable-bindings \
			-nomoduleglobal \
			-small \
			-fvirtual \
			-fcompact \
			-I/er301_sdk \
			-Isrc \
			-o $(SWIG_WRAP) $(SWIG_FILE)
	@echo ">>> SWIG done: $(SWIG_WRAP)"

# ── DOCKER BUILD ──────────────────────────────────────────────────────────────
# Cross-compiles inside Docker; mounts this dir and the SDK live.

docker-build: docker-image | $(OUTDIR)
	@test -f "$(SWIG_WRAP)" || \
		{ echo "ERROR: SWIG wrapper not generated yet."; \
		  echo "       Run first:  make swig-docker ER301_SDK=~/er-301"; \
		  exit 1; }
	$(eval SDK_ABS := $(shell realpath $(ER301_SDK) 2>/dev/null))
	@test -n "$(SDK_ABS)" || \
		{ echo "ERROR: ER301_SDK path '$(ER301_SDK)' does not exist."; \
		  echo "       Clone it first:  git clone https://github.com/odevices/er-301 ~/er-301"; \
		  echo "       Then retry:      make docker-build ER301_SDK=~/er-301"; \
		  exit 1; }
	@echo ">>> Docker cross-compile: $(OUTLIB) (SDK=$(SDK_ABS)) ..."
	docker run --rm \
		-v "$(CURDIR)":/build \
		-v "$(SDK_ABS)":/er301_sdk \
		-w /build \
		$(DOCKER_IMAGE) \
		make build TOOLCHAIN=native ER301_SDK=/er301_sdk
	@echo ">>> Done: $(OUTLIB)"

# ── PACKAGE ───────────────────────────────────────────────────────────────────
# Assembles the installable .pkg file (a flat zip containing the Lua assets
# and the compiled .so).  Run after docker-build.

PKGDIR  := build/$(ARCH)
PKGFILE := $(PKGDIR)/$(PKG)-$(VERSION).pkg

pkg: check-api check-ports check-version check-lua $(PKGFILE) prune-old-pkgs

# KEEP THE HISTORY. LIST, DO NOT DELETE. (Nick, 0.2.27)
#
# This used to delete every package except the current one, because the device
# will happily install an OLD build sitting in its packages folder and a week
# was lost to exactly that. The warning is still worth printing — but deleting
# was the wrong remedy: when 0.2.26 broke the unit, the known-good 0.2.25 .pkg
# had already been destroyed and could not be handed back.
#
# So: print what else is here, loudly, and leave it alone. Visibility was the
# useful half; the deletion was not.
prune-old-pkgs:
	@found=`ls $(PKGDIR)/$(PKG)-*.pkg 2>/dev/null | grep -v '$(PKG)-$(VERSION).pkg' || true`; \
	if [ -n "$$found" ]; then \
	  echo ">>> NOTE: other packages are present. The device can install ANY of"; \
	  echo "    these — copy only the one you want to the card:"; \
	  for f in $$found; do echo "      $$f"; done; \
	fi; true
	@echo ">>> current: $(PKGDIR)/$(PKG)-$(VERSION).pkg"




# Every method the Lua calls on the head must be OUTSIDE the `#ifndef SWIGLUA`
# region, or SWIG will not bind it and it will be nil on the device. This is a
# hard gate on packaging because nothing else catches it: it builds, links and
# installs cleanly, then crashes the moment the menu opens. (v0.1.3-0.1.5.)
check-api:
	@sh tools/check-swig-api.sh src/Noether.h assets/Noether.lua assets/LoopView.lua assets/ReelView.lua

# Every od::Inlet / od::Outlet must be addInput()/addOutput()'d, and every
# port Lua connects to on the head must be one of them. 0.8.0 declared and
# read mBarsIn and Lua connected to "Bars", but the constructor never
# registered it — green on the host (the stub addInput is a no-op), a failed
# connect on the device.
check-ports:
	@sh tools/check-ports.sh src/Noether.h src/Noether.cpp assets/Noether.lua

# A Lua file-local used ABOVE its declaration is a nil GLOBAL — no syntax
# error, no load error, no warning, just "attempt to call a nil value" at
# runtime. v0.2.23 shipped `bankName(` and `ok_channels(` called from
# Landau:setSample about 80 lines above their declarations, and v0.2.24 added
# `trace(` the same way. Twenty device tests did not find it; this script did,
# in under a second.
check-lua:
	@tools/check-lua-scope.sh assets/*.lua
	@tools/check-lua-parse.sh assets/*.lua


# The version the unit DISPLAYS must equal the version it is PACKAGED as.
check-version:
	@sh tools/check-version.sh Makefile assets/toc.lua \
		assets/Noether.lua assets/LoopView.lua assets/ReelView.lua

$(PKGFILE): $(OUTLIB) assets/toc.lua assets/Noether.lua assets/LoopView.lua assets/ReelView.lua | $(PKGDIR)
	@echo ">>> PKG $(PKGFILE)"
	cd assets && zip -j ../$(PKGFILE) toc.lua Noether.lua LoopView.lua ReelView.lua
	cd $(OUTDIR) && zip -j ../../$(PKGFILE) libnoether.so
	zip -j $(PKGFILE) LICENSE NOTICE
	@echo ">>> Done: $(PKGFILE)"

$(PKGDIR):
	mkdir -p $@

# ── CLEAN ─────────────────────────────────────────────────────────────────────

clean:
	rm -f $(SWIG_WRAP) $(OUTLIB)
	rm -rf $(OBJS_DIR)
	rm -f $(PKGDIR)/$(PKG)-*.pkg

# ── HELP ──────────────────────────────────────────────────────────────────────

help:
	@echo ""
	@echo "Noether build targets (recommended order for macOS):"
	@echo "  make docker-image                           Build the Docker image (once)"
	@echo "  make swig-docker ER301_SDK=~/er-301         Generate SWIG wrapper (inside Docker)"
	@echo "  make docker-build ER301_SDK=~/er-301        Cross-compile inside Docker"
	@echo "  make pkg                                    Package into build/am335x/noether-$(VERSION).pkg"
	@echo ""
	@echo "Alternatives:"
	@echo "  make swig ER301_SDK=~/er-301                Generate SWIG wrapper (host, needs brew install swig)"
	@echo "  make build TOOLCHAIN=native ER301_SDK=...   Cross-compile natively (Linux only)"
	@echo "  make clean                                  Remove generated files"
	@echo ""

# ── host tests (no hardware) ────────────────────────────────────────────────
hosttest: check-api check-ports check-version check-lua
	g++ -std=c++11 -O2 -ffast-math -Itest/host -Isrc src/Noether.cpp test/host/main.cpp -o test/t
	./test/t ident && ./test/t length && ./test/t seam && ./test/t pulse && ./test/t tri && ./test/t speed \
	  && ./test/t sos && ./test/t extend && ./test/t voct && ./test/t persist \
	  && ./test/t detent && ./test/t aa && ./test/t viz && ./test/t undo && ./test/t stop && ./test/t clock && ./test/t bars \
	  && ./test/t nan && ./test/t cpu
	g++ -std=c++17 -O1 -g -fsanitize=address,undefined -Itest/host -Isrc \
	  src/Noether.cpp test/host/main.cpp -o test/t_asan
	./test/t_asan asan
