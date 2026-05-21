.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/wut/share/wut_rules

#-------------------------------------------------------------------------------
# App metadata
#-------------------------------------------------------------------------------
TARGET        :=  discord-wiiu
BUILD         :=  build
SOURCES       :=  src \
                  src/discord \
                  src/ui \
                  vendor/cJSON
INCLUDES      :=  src vendor
DATA          :=

APP_NAME       := Discord Wii U
APP_SHORTNAME  := Discord
APP_AUTHOR     := WiiU Homebrew
APP_ICON       := $(TOPDIR)/meta/icon.png

#-------------------------------------------------------------------------------
# Build flags
#-------------------------------------------------------------------------------
CFLAGS    :=  -g -Wall -Wextra -O2 $(MACHDEP) \
              -Wno-unused-parameter

CXXFLAGS  :=  $(CFLAGS) -std=c++17 -fexceptions -fno-rtti

ASFLAGS   :=  $(ARCH)

LDFLAGS    =  $(ARCH) $(RPXSPECS) -Wl,-Map,$(notdir $*.map)

LIBS      :=  -lSDL2_ttf \
              -lharfbuzz \
              -lfreetype \
              -lpng16 \
              -lbz2 \
              -lSDL2 \
              -lcurl \
              -lbrotlidec \
              -lbrotlicommon \
              -lmbedtls \
              -lmbedcrypto \
              -lmbedx509 \
              -lz \
              -lwutd \
              -lwut

LIBDIRS   :=  $(WUT_ROOT) \
              $(PORTLIBS)

#-------------------------------------------------------------------------------
# Two-phase build: top-level sets up exports; recursive make builds in BUILD/
#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)
export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(TOPDIR)/$(dir)/*.c)))
CPPFILES  :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(TOPDIR)/$(dir)/*.cpp)))
SFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(TOPDIR)/$(dir)/*.s)))

export OFILES   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(TOPDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export VPATH    :=  $(foreach dir,$(SOURCES),$(TOPDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(TOPDIR)/$(dir))

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -rf $(BUILD) $(TARGET).elf $(TARGET).rpx $(TARGET).wuhb

else

#-------------------------------------------------------------------------------
# Build directory context: wire include paths into CPPFLAGS for base_rules;
# use CXX as linker so C++ runtime and -specs flags are handled correctly.
#-------------------------------------------------------------------------------
CPPFLAGS := $(INCLUDE)
LD       := $(CXX)
DEPENDS   := $(OFILES:.o=.d)

$(OUTPUT).wuhb : $(OUTPUT).rpx
$(OUTPUT).rpx  : $(OUTPUT).elf
$(OUTPUT).elf  : $(OFILES)

-include $(DEPENDS)

endif
