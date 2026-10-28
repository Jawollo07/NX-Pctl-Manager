# NX-Pctl-Manager: devkitPro + libnx + Plutonium build.
.SUFFIXES:
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif
TOPDIR ?= $(abspath $(CURDIR))
include $(DEVKITPRO)/libnx/switch_rules
TARGET := nx_pctl_manager
BUILD := build
SOURCES := source source/action
DATA := data
INCLUDES := include source
ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS := -g -Wall -Wextra -O2 -ffunction-sections $(ARCH) -D__SWITCH__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17
LDFLAGS = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*).map
PLUTONIUM_DIR := $(TOPDIR)/extern/plutonium
PLUTONIUM_LIBDIR := $(PLUTONIUM_DIR)/Plutonium
LIBDIRS := $(PORTLIBS) $(LIBNX) $(PLUTONIUM_LIBDIR)
LIBS := -lpu -lSDL2_mixer -lopusfile -lopus -lmodplug -lmpg123 -lvorbisidec -logg -lSDL2_ttf -lSDL2_gfx -lSDL2_image -lSDL2 -lEGL -lGLESv2 -lglapi -ldrm_nouveau -lwebp -lpng -ljpeg -lfreetype -lharfbuzz -lz -lbz2 -lnx
REMOTE_CONTROL ?= 0
ifeq ($(REMOTE_CONTROL),1)
CXXFLAGS += -DNX_PCTL_REMOTE_CONTROL=1
LIBS += -lcurl -lmbedtls -lmbedx509 -lmbedcrypto
endif
ifneq ($(BUILD),$(notdir $(CURDIR)))
export OUTPUT := $(CURDIR)/$(TARGET)
export TOPDIR := $(CURDIR)
export VPATH := $(foreach dir,$(SOURCES),$(TOPDIR)/$(dir)) $(foreach dir,$(DATA),$(TOPDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))
ifeq ($(strip $(CPPFILES)),)
export LD := $(CC)
else
export LD := $(CXX)
endif
export OFILES_BIN := $(addsuffix .o,$(BINFILES))
export OFILES_SRC := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN := $(addsuffix .h,$(subst .,_,$(BINFILES)))
export INCLUDE := $(foreach dir,$(INCLUDES),-I$(TOPDIR)/$(dir)) $(foreach dir,$(LIBDIRS),-I$(dir)/include) -I$(DEVKITPRO)/libnx/include -I$(TOPDIR)/$(BUILD)
CFLAGS += $(INCLUDE)
CXXFLAGS += $(INCLUDE)
# The app is compiled by a second make process in $(BUILD). Explicitly export
# the augmented flags so libnx and project headers remain visible there.
export CFLAGS CXXFLAGS
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export APP_TITLE := Pctl Manager
export APP_AUTHOR := Taylor
export APP_VERSION := 3.2.0
export APP_ICON := $(CURDIR)/icon.jpg
export NROFLAGS += --romfsdir=$(CURDIR)/resources
.PHONY: all clean dist plutonium
all: $(BUILD)
plutonium:
	@$(MAKE) --no-print-directory -C $(PLUTONIUM_DIR)
$(BUILD): plutonium
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
clean:
	@$(MAKE) --no-print-directory -C $(PLUTONIUM_DIR) clean
	@rm -rf $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(TARGET).zip
dist: all
	@rm -rf out && mkdir -p out/switch
	@cp $(TARGET).nro out/switch/
	@cd out && zip -r ../$(TARGET).zip .
	@rm -rf out
else
.PHONY: all
DEPENDS := $(OFILES:.o=.d)
all: $(OUTPUT).nro
$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)
$(OFILES_SRC): $(HFILES_BIN)
-include $(DEPENDS)
endif
