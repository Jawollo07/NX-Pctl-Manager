# Headless Atmosphere/Stratosphere sysmodule build.
#
# Usage:
#   make -C sysmodule ATMOSPHERE_ROOT=/path/to/pinned/Atmosphere nx_release
#
# ATMOSPHERE_ROOT must point to a complete Atmosphere source checkout at the
# revision documented in docs/sysmodule-build.md. It is deliberately external:
# this repository does not vendor or silently track a moving Atmosphere branch.

ifndef ATMOSPHERE_ROOT
$(error Set ATMOSPHERE_ROOT to the pinned Atmosphere source checkout)
endif

ATMOSPHERE_ROOT := $(abspath $(ATMOSPHERE_ROOT))
MODULE_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

.PHONY: all nx_release nx_debug clean

all: nx_release

nx_release:
	@$(MAKE) -f $(MODULE_DIR)/system_module.mk \
		ATMOSPHERE_ROOT="$(ATMOSPHERE_ROOT)" \
		ATMOSPHERE_MAKEFILE_TARGET=nx_release \
		ATMOSPHERE_BUILD_NAME=release \
		ATMOSPHERE_BOARD=nx-hac-001 ATMOSPHERE_CPU=arm-cortex-a57

nx_debug:
	@$(MAKE) -f $(MODULE_DIR)/system_module.mk \
		ATMOSPHERE_ROOT="$(ATMOSPHERE_ROOT)" \
		ATMOSPHERE_MAKEFILE_TARGET=nx_debug \
		ATMOSPHERE_BUILD_NAME=debug \
		ATMOSPHERE_BUILD_FOR_DEBUGGING=1 \
		ATMOSPHERE_BOARD=nx-hac-001 ATMOSPHERE_CPU=arm-cortex-a57

clean:
	@$(MAKE) -f $(MODULE_DIR)/system_module.mk clean \
		ATMOSPHERE_ROOT="$(ATMOSPHERE_ROOT)" \
		ATMOSPHERE_MAKEFILE_TARGET=nx_release \
		ATMOSPHERE_BUILD_NAME=release \
		ATMOSPHERE_BOARD=nx-hac-001 ATMOSPHERE_CPU=arm-cortex-a57
