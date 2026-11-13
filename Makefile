# Static for Playdate. `make` builds the simulator and device targets into
# Static.pdx (needs the Playdate SDK and the Arm GNU toolchain on PATH);
# `make pdc` builds the simulator target only.
HEAP_SIZE      = 8388208
STACK_SIZE     = 61800

PRODUCT = Static.pdx

SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif
ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

VERSION := $(shell sed -n 's/^version=//p' Source/pdxinfo)
HELIX = src/third_party/helix
VPATH += src:$(HELIX):$(HELIX)/libhelix-mp3:$(HELIX)/libhelix-aac

SRC = src/main.c src/net.c src/audio.c src/dec.c src/player.c src/bench.c src/report.c \
      src/sched.c src/slots.c src/json.c src/mp3probe.c src/mp4.c src/syscalls.c src/textfold.c src/third_party/qrcodegen.c \
      $(HELIX)/helix_memory.c $(wildcard $(HELIX)/libhelix-mp3/*.c) $(wildcard $(HELIX)/libhelix-aac/*.c)
UINCDIR = src $(HELIX) $(HELIX)/libhelix-mp3 $(HELIX)/libhelix-aac $(HELIX)/utils
UASRC =
UDEFS = -DUSE_DEFAULT_STDLIB
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

# A zip testers can sideload at play.date/account/sideload
dist: all
	@grep -q '#define APP_VERSION "$(VERSION)"' src/common.h || { echo "APP_VERSION in src/common.h must match pdxinfo ($(VERSION))"; exit 1; }
	@if $(GCC)$(TRGT)objdump -d $(OBJDIR)/pdex.elf | awk '/<_calloc_r>:/,/^$$/' | grep -qE '<(calloc|malloc)>'; then \
		echo "_calloc_r calls calloc or malloc, which loops forever on the device (see src/syscalls.c)"; exit 1; fi
	rm -f Static-$(VERSION).pdx.zip
	zip -qr Static-$(VERSION).pdx.zip $(PRODUCT)
	@echo built Static-$(VERSION).pdx.zip
