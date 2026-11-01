#---------------------------------------------------------------------------------
# HomeNX Discord: a Tesla/Ultrahand overlay (.ovl), built with devkitPro's
# switch_rules like the Tesla template. `make` here gives homenx-discord.ovl.
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE	:= HomeNX Discord
APP_AUTHOR	:= HomeNX
APP_VERSION	:= 1.0.0

TARGET		:= homenx-discord
BUILD		:= build
SOURCES		:= source
INCLUDES	:= include ../menu/src/third_party ../common/include ../../lib/nxui/include/nxui/third_party/stb
NO_ICON		:= 1

ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS	:=	-g -Wall -Os -ffunction-sections -fdata-sections $(ARCH) $(DEFINES)
CFLAGS	+=	$(INCLUDE) -D__SWITCH__
CXXFLAGS	:= $(CFLAGS) -std=gnu++20
ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -Wl,--gc-sections

CURL_LIBS := $(shell $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config --libs --static libcurl 2>/dev/null)
ifeq ($(filter -lcurl,$(CURL_LIBS)),)
# No libcurl.pc here (the .pc files carry the prefix curl was built with, and
# some devkitPro installs ship the libraries without it): link the known static
# set for devkitPro's curl — mbedtls and zlib, in dependency order.
CURL_LIBS := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz
endif
LIBS	:=	$(CURL_LIBS) -lz -lnx
LIBDIRS	:= $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))

export LD	:=	$(CXX)
export OFILES	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o)
export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)
export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)
export NROFLAGS	+=	--nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@rm -fr $(BUILD) $(TARGET).ovl $(TARGET).nacp $(TARGET).elf

else

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).ovl

$(OUTPUT).ovl	:	$(OUTPUT).elf $(OUTPUT).nacp
	@elf2nro $< $@ $(NROFLAGS)
	@echo "built ... $(notdir $(OUTPUT).ovl)"

$(OUTPUT).elf	:	$(OFILES)

-include $(DEPENDS)

endif
