#---------------------------------------------------------------------------------
# Basic libnx .nro Makefile (switchmio)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output .nro file
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# ROMFS is a directory whose contents get bundled into the .nro (e.g. font.ttf)
#
# APP_TITLE, APP_AUTHOR, APP_VERSION define metadata shown on the Homebrew Menu
# APP_ICON points to a 256x256 JPG used as the icon
#---------------------------------------------------------------------------------
TARGET      :=  switchmio
BUILD       :=  build
SOURCES     :=  source
DATA        :=  data
INCLUDES    :=  include
ROMFS       :=  romfs

APP_TITLE   :=  switchmio
APP_AUTHOR  :=  You
APP_VERSION :=  1.0
APP_ICON    :=  icon/icon.jpg

#---------------------------------------------------------------------------------
ARCH    :=  -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec

# Cross pkg-config, used to pull correct cflags/libs for SDL2 + SDL2_ttf + SDL2_image
PKGCONF :=  $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
PC_LIBS :=  sdl2 SDL2_ttf SDL2_image

PC_LIBS :=  sdl2 SDL2_ttf SDL2_image SDL2_mixer
LIBS    :=  `$(PKGCONF) --libs $(PC_LIBS)` -lmodplug -lmpg123 -lvorbisidec -lopusfile -lopus -lFLAC -logg -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ljpeg -lpng -lwebp -lz -lnx -lm

CFLAGS  :=  -g -Wall -O2 -ffunction-sections \
            $(ARCH) $(DEFINES) `$(PKGCONF) --cflags $(PC_LIBS)`

CFLAGS  +=  $(INCLUDE) -D__SWITCH__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++20

ASFLAGS :=  -g $(ARCH)
LDFLAGS  =  -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    :=  -lnx
#---------------------------------------------------------------------------------
LIBDIRS :=  $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
# Use the cross-compiler as the linker driver, not the plain system 'ld'
#---------------------------------------------------------------------------------
export LD := $(CC)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                     $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    :=  $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export OFILES_BIN   :=  $(addsuffix .o,$(BINFILES))
export OFILES_SRC   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES        =  $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   :=  $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE      :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                         $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                         -I$(CURDIR)/$(BUILD)

export LIBPATHS     :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export NROFLAGS     :=  --icon=$(CURDIR)/$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp
ifneq ($(ROMFS),)
export NROFLAGS     +=  --romfsdir=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all

#---------------------------------------------------------------------------------
all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all : $(OUTPUT).nro

$(OUTPUT).nro : $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf : $(OFILES)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
