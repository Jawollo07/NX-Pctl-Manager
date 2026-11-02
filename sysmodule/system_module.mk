/*
 * NX-Pctl-Manager Stratosphere build rules.
 * Adapted from Atmosphere-NX/Atmosphere stratosphere/TioServer/system_module.mk.
 * Copyright (c) Atmosphere-NX contributors.
 *
 * This file is distributed under GPL-2.0-or-later.
 */
# Stratosphere sysmodule rules; upstream build configuration is pinned externally.
ifndef ATMOSPHERE_ROOT
$(error ATMOSPHERE_ROOT must point to a complete Atmosphere checkout)
endif

THIS_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
CURRENT_DIRECTORY := $(abspath $(dir $(THIS_MAKEFILE)))

TARGET := nx_pctl_manager
SOURCES := source
INCLUDES := include
DATA :=
CONFIG_JSON := nx_pctl_manager.json

include $(ATMOSPHERE_ROOT)/libraries/config/templates/stratosphere.mk

ATMOSPHERE_SYSTEM_MODULE_TARGETS := nsp

ifneq ($(__RECURSIVE__),1)

export TOPDIR := $(CURRENT_DIRECTORY)
export VPATH := $(foreach dir,$(SOURCES),$(TOPDIR)/$(dir)) \
	$(foreach dir,$(DATA),$(TOPDIR)/$(dir))

CFILES := $(call FIND_SOURCE_FILES,$(SOURCES),c)
CPPFILES := $(call FIND_SOURCE_FILES,$(SOURCES),cpp)
SFILES := $(call FIND_SOURCE_FILES,$(SOURCES),s)
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif

export OFILES := $(addsuffix .o,$(BINFILES)) \
	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(TOPDIR)/$(dir)) \
	$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
	$(foreach dir,$(AMS_LIBDIRS),-I$(dir)/include) \
	-I$(TOPDIR)/$(ATMOSPHERE_BUILD_DIR)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
	$(foreach dir,$(AMS_LIBDIRS),-L$(dir)/$(ATMOSPHERE_LIBRARY_DIR))

export BUILD_EXEFS_SRC := $(TOPDIR)/$(EXEFS_SRC)
export APP_JSON := $(TOPDIR)/$(CONFIG_JSON)

.PHONY: clean all check_lib

all: $(ATMOSPHERE_OUT_DIR) $(ATMOSPHERE_BUILD_DIR) \
	$(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere/$(ATMOSPHERE_LIBRARY_DIR)/libstratosphere.a
	@$(MAKE) __RECURSIVE__=1 OUTPUT=$(TOPDIR)/$(ATMOSPHERE_OUT_DIR)/$(TARGET) \
		DEPSDIR=$(TOPDIR)/$(ATMOSPHERE_BUILD_DIR) \
		--no-print-directory -C $(ATMOSPHERE_BUILD_DIR) -f $(THIS_MAKEFILE)

$(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere/$(ATMOSPHERE_LIBRARY_DIR)/libstratosphere.a: check_lib
	@$(SILENTCMD)echo "Checked libstratosphere."

ifeq ($(ATMOSPHERE_CHECKED_LIBSTRATOSPHERE),1)
check_lib:
else
check_lib:
	@$(MAKE) --no-print-directory -C $(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere \
		-f $(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere/libstratosphere.mk
endif

$(ATMOSPHERE_OUT_DIR) $(ATMOSPHERE_BUILD_DIR):
	@[ -d $@ ] || mkdir -p $@

clean:
	@rm -rf $(ATMOSPHERE_OUT_DIR) $(ATMOSPHERE_BUILD_DIR)

else

.PHONY: all
DEPENDS := $(OFILES:.o=.d)

all: $(foreach target,$(ATMOSPHERE_SYSTEM_MODULE_TARGETS),$(OUTPUT).$(target))

$(OUTPUT).kip: $(OUTPUT).elf
$(OUTPUT).nsp: $(OUTPUT).nso $(OUTPUT).npdm
$(OUTPUT).nso: $(OUTPUT).elf
$(OUTPUT).elf: $(OFILES)

$(OFILES): $(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere/$(ATMOSPHERE_LIBRARY_DIR)/libstratosphere.a

%.npdm: %.npdm.json
	@npdmtool $< $@

-include $(DEPENDS)

endif
