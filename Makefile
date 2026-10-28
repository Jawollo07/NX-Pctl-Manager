# Plutonium/libnx build. Requires devkitPro and the Plutonium submodule.
# Central-server transport stays opt-in, matching the previous Makefile default.
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := nx_pctl_manager
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include
ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS      := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) -D__SWITCH__
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
LDFLAGS     = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*).map
PLUTONIUM   := $(CURDIR)/extern/plutonium/Plutonium
LIBDIRS     := $(PORTLIBS) $(LIBNX) $(PLUTONIUM)
LIBS        := -lpu -lSDL2_mixer -lopusfile -lopus -lmodplug -lmpg123 -lvorbisidec -logg -lSDL2_ttf -lSDL2_gfx -lSDL2_image -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lwebp -lpng -ljpeg -lfreetype -lharfbuzz -lz -lbz2 -lnx

REMOTE_CONTROL ?= 0
ifeq ($(REMOTE_CONTROL),1)
CXXFLAGS += -DNX_PCTL_REMOTE_CONTROL=1
LIBS += -lcurl -lmbedtls -lmbedx509 -lmbedcrypto
endif

.PHONY: all clean dist
all: $(BUILD)
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

dist: all
	@rm -rf out && mkdir -p out/switch
	@cp $(TARGET).nro out/switch/
	@cd out && zip -r ../$(TARGET).zip .
	@rm -rf out

else
.PHONY: all
DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).nro
ifeq ($(strip $(NO_NACP)),)
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
else
$(OUTPUT).nro: $(OUTPUT).elf
endif
$(OUTPUT).elf: $(OFILES)
$(OFILES_SRC): $(HFILES_BIN)
-include $(DEPENDS)
endif
