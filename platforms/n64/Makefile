ifndef GAME
$(error GAME is required; use director64 build --game <slug>)
endif
ifndef WORK
$(error WORK is required)
endif
DIRECTOR_VERSION ?= 6
EXTENDED_D6 ?= 0
GENERATED_POINTER ?= 0
CONTROLLER_COUNT ?= 1
REPLAY_SCENARIO ?= 1
PROFILE ?= release
ifeq ($(filter $(PROFILE),release probe),)
$(error PROFILE must be release or probe)
endif
BUILD_DIR := $(WORK)/n64/$(PROFILE)
TARGET := $(GAME)$(if $(filter probe,$(PROFILE)),-probe,)
ROOT ?= /workdir
SOURCE_DIRS := $(addprefix $(ROOT)/,platforms/n64 runtime/director runtime/lingo runtime/interaction games/$(GAME)/runtime) $(WORK)/aot/c
.DEFAULT_GOAL := all
include $(N64_INST)/include/n64.mk
# Link with the runtime's copy of libdragon's linker script, which gives the
# static sections and the heap start a fixed footing in the data cache's
# period (see the header of n64.ld).
N64_LDFLAGS := $(patsubst -Tn64.ld,-T$(ROOT)/platforms/n64/n64.ld,$(N64_LDFLAGS))
ifeq ($(DIRECTOR_VERSION),10)
# The D10 shared script casts expand several MiB of overlay code per stage
# next to 800x600 stage planes; size-optimized code across the target keeps
# the resident controller window and the planes inside the 8 MiB console.
# Appended after n64.mk's -O2 inside its target-specific flag sets.
N64_C_AND_CXX_FLAGS += -Os
endif
# The pinned n64.mk emits --padding 0 when metadata is set at include time,
# but n64tool requires a suffix for one-digit sizes. Let n64metadata pad last.
N64_ROM_METADATA := $(BUILD_DIR)/metadata.ini
N64_TOOLFLAGS += --padding 0B

CFLAGS += $(addprefix -I,$(SOURCE_DIRS) $(ROOT)/runtime/storage $(ROOT)/runtime) -I$(WORK)/aot/c -I$(WORK)/director/c
CFLAGS += -DDIRECTOR64_DIRECTOR_VERSION=$(DIRECTOR_VERSION) -DDIRECTOR64_EXTENDED_D6=$(EXTENDED_D6)
# The ports the ROM header declares are the ports the runtime reads, so the
# number of on-screen cursors never disagrees with the menu's player count.
CFLAGS += -DDIRECTOR64_CONTROLLER_COUNT=$(CONTROLLER_COUNT)
CFLAGS += -DDIRECTOR64_ENTRY_MOVIE='"$(ENTRY_MOVIE)"'
# symbols.c is the converter's symbol table for this game (aot/c), resident with the runtime.
SOURCES := director_main.c director.c lingo_runtime.c platform.c input.c shape.c pointer.c symbols.c
ifeq ($(GAME),willy-werkel-cars)
CFLAGS += -DDIRECTOR64_WILLY=1
endif
ifeq ($(GAME),loewenzahn-1)
CFLAGS += -DDIRECTOR64_PRINTING=1
SOURCES += print_ui.c
endif
ifneq ($(filter 1,$(EXTENDED_D6))$(filter $(DIRECTOR_VERSION),7 8 10),)
SOURCES += text_input.c
endif
ifeq ($(DIRECTOR_VERSION),5)
SOURCES += d5_video.c
LDFLAGS += --wrap=sys_hw_memset
endif
SOURCES += $(notdir $(filter-out %/director_replay.c,$(wildcard $(ROOT)/games/$(GAME)/runtime/*.c)))
N64_MKDFS_ROOT := $(WORK)/filesystem
AOT_NAMES := $(notdir $(basename $(wildcard $(WORK)/aot/c/*_dxr.c $(WORK)/aot/c/*_cxt.c)))
DSOS := $(addprefix $(N64_MKDFS_ROOT)/code/,$(addsuffix .dso,$(AOT_NAMES)))
# Diagnostic input ELF names embedded in DSO must match in release/probe DFS.
$(DSOS): BUILD_DIR := $(WORK)/n64/release
$(WORK)/n64/release/aot/%.o: $(WORK)/aot/c/%.c $(ROOT)/runtime/lingo/lingo_runtime.h $(WORK)/n64/build-context.json $(ROOT)/platforms/n64/Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(WORK)/n64/release/metadata/%.o: $(WORK)/director/c/%_scene.c $(ROOT)/runtime/director/director.h $(ROOT)/runtime/director/cursor.h $(WORK)/n64/build-context.json $(ROOT)/platforms/n64/Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(foreach name,$(AOT_NAMES),$(eval $(N64_MKDFS_ROOT)/code/$(name).dso: $(WORK)/n64/release/aot/$(name).o $(WORK)/n64/release/metadata/$(name).o))

# SDK rules use TARGET as a pathname; keep the filename inside BUILD_DIR flat.
ROM_NAME := $(notdir $(TARGET))
$(BUILD_DIR)/$(TARGET).externs: $(DSOS)
$(BUILD_DIR)/$(TARGET).elf: $(BUILD_DIR)/$(TARGET).externs
$(BUILD_DIR)/$(TARGET).elf: $(ROOT)/platforms/n64/n64.ld
$(BUILD_DIR)/$(TARGET).msym: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).externs
$(TARGET).z64: $(BUILD_DIR)/$(TARGET).msym $(BUILD_DIR)/$(TARGET).dfs $(N64_ROM_METADATA)
$(BUILD_DIR)/$(TARGET).dfs: $(DSOS) FORCE
ifeq ($(PROFILE),probe)
CFLAGS += -DDIRECTOR64_FULL_REPLAY=1 -DDIRECTOR64_REPLAY_SCENARIO=$(REPLAY_SCENARIO)
SOURCES += director_replay.c
$(BUILD_DIR)/director_replay.o: $(WORK)/director/c/replay.inc FORCE
endif
define source_rule
$(BUILD_DIR)/$(notdir $(1:.c=.o)): $(1)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(CFLAGS) -c $$< -o $$@
endef
$(foreach source,$(SOURCES),$(foreach dir,$(SOURCE_DIRS),$(foreach path,$(wildcard $(dir)/$(source)),$(eval $(call source_rule,$(path))))))
OBJS := $(addprefix $(BUILD_DIR)/,$(SOURCES:.c=.o))
$(OBJS) $(DSOS): $(WORK)/n64/build-context.json $(ROOT)/platforms/n64/Makefile
$(BUILD_DIR)/$(TARGET).elf: $(OBJS)
$(TARGET).z64: N64_ROM_TITLE = "$(ROM_TITLE)"
$(TARGET).z64: N64_ROM_SAVETYPE = flashram
$(TARGET).z64: N64_ROM_CONTROLLER1 = n64
$(TARGET).z64: N64_ROM_CONTROLLER2 = $(if $(filter 2 3 4,$(CONTROLLER_COUNT)),n64,none)
$(TARGET).z64: N64_ROM_CONTROLLER3 = $(if $(filter 3 4,$(CONTROLLER_COUNT)),n64,none)
$(TARGET).z64: N64_ROM_CONTROLLER4 = $(if $(filter 4,$(CONTROLLER_COUNT)),n64,none)
$(TARGET).z64: N64_ROM_REGIONFREE = 1
$(TARGET).z64: N64_ROM_RTC =
all: $(TARGET).z64
FORCE:
-include $(wildcard $(BUILD_DIR)/*.d)
.PHONY: all FORCE
