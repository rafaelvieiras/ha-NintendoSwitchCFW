#---------------------------------------------------------------------------------
.SUFFIXES:

TARGET		:=	homeassistant_orchestrator
BUILD		:=	build
SOURCES		:=	.
INCLUDES	:=	include

#---------------------------------------------------------------------------------
DEVKITPRO	?=	/opt/devkitpro
PREFIX		:=	$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
ARCH		:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=el0 -fPIE

CC		:=	$(PREFIX)gcc
CXX		:=	$(PREFIX)g++
LD		:=	$(PREFIX)gcc
OBJCOPY	:=	$(PREFIX)objcopy
STRIP	:=	$(PREFIX)strip
NM		:=	$(PREFIX)nm

#---------------------------------------------------------------------------------
DEFINES	:=	-D__SWITCH__
CFLAGS	:=	-g -Wall -Wextra -O2 -ffunction-sections -fPIE $(ARCH) $(DEFINES) -I$(CURDIR)/../switch_sysmodule/include -isystem $(DEVKITPRO)/libnx/include -isystem $(DEVKITPRO)/portlibs/switch/include
CXXFLAGS:=	$(CFLAGS) -std=c++17 -fno-rtti -fno-exceptions
LDFLAGS	:=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -pie -Wl,-Map,$(notdir $*.map) -L$(DEVKITPRO)/libnx/lib -L$(DEVKITPRO)/portlibs/switch/lib

# No curl/mbedtls/z here on purpose - the orchestrator never talks to the network
# beyond loopback health checks and its own tiny control port, unlike core.
LIBS	:= -lnx

#---------------------------------------------------------------------------------
all: $(TARGET).nso

$(TARGET).nso: $(TARGET).elf
	$(DEVKITPRO)/tools/bin/elf2nso $< $@
	@echo built ... $@


$(TARGET).elf: main.cpp ../switch_sysmodule/ConfigManager.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).nso $(TARGET).nsp $(TARGET).map
