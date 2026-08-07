#---------------------------------------------------------------------------------
.SUFFIXES:

TARGET		:=	homeassistant
BUILD		:=	build
SOURCES		:=	.
INCLUDES	:=	include

#---------------------------------------------------------------------------------
DEVKITPRO	?=	/opt/devkitpro
PREFIX		:=	$(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-

CC		:=	$(PREFIX)gcc
CXX		:=	$(PREFIX)g++
LD		:=	$(PREFIX)gcc
OBJCOPY	:=	$(PREFIX)objcopy
STRIP	:=	$(PREFIX)strip
NM		:=	$(PREFIX)nm

#---------------------------------------------------------------------------------
ARCH	:=	-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=el0 -fPIE
DEFINES	:=	-D__SWITCH__
CFLAGS	:=	-g -Wall -Wextra -O2 -ffunction-sections $(ARCH) $(DEFINES) -I$(CURDIR)/include -isystem $(DEVKITPRO)/libnx/include -isystem $(DEVKITPRO)/portlibs/switch/include -I$(CURDIR)/../switch_sysmodule/include
CXXFLAGS:=	$(CFLAGS) -fno-rtti -fno-exceptions
LDFLAGS	:=	-specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) -L$(DEVKITPRO)/libnx/lib -L$(DEVKITPRO)/portlibs/switch/lib

LIBS	:= -lcurl -lmbedtls -lmbedcrypto -lmbedx509 -lz -lnx

# Same source as the .nro build above (main() branches into launcher mode at startup
# when it finds LAUNCH_REQUEST_PATH - see run_launcher_mode() in main.cpp), but packaged
# as an NSO/exefs.nsp instead of an .nro: this build is meant to be dropped at
# /atmosphere/contents/ALBUM_OVERRIDE_PROGRAM_ID/ as a full content override for the
# Album applet (see SysmoduleConstants.h), not run from the Homebrew Menu. Uses the same
# sysmodule-style specs/-pie/elf2nso as switch_sysmodule/switch_sysmodule_orchestrator,
# not the hbloader-style .nro toolchain above.
LAUNCHER_TARGET := homeassistant_launcher
LAUNCHER_CXXFLAGS := $(CXXFLAGS) -DLAUNCHER_BUILD
LAUNCHER_LDFLAGS := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -pie -Wl,-Map,$(LAUNCHER_TARGET).map -L$(DEVKITPRO)/libnx/lib -L$(DEVKITPRO)/portlibs/switch/lib

#---------------------------------------------------------------------------------
all: $(TARGET).nro

$(TARGET).nro: $(TARGET).elf
	$(DEVKITPRO)/tools/bin/nacptool --create "HomeAssistant Switch Integration" "FaserF" "$(VERSION)" $(TARGET).nacp
	$(DEVKITPRO)/tools/bin/elf2nro $< $@ --icon=icon.jpg --nacp=$(TARGET).nacp
	@echo built ... $@

$(TARGET).elf: main.cpp ../switch_sysmodule/ConfigManager.cpp
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@

launcher: $(LAUNCHER_TARGET).nso launcher_main.npdm

$(LAUNCHER_TARGET).nso: $(LAUNCHER_TARGET).elf
	$(DEVKITPRO)/tools/bin/elf2nso $< $@
	@echo built ... $@

$(LAUNCHER_TARGET).elf: main.cpp ../switch_sysmodule/ConfigManager.cpp
	$(CXX) $(LAUNCHER_CXXFLAGS) $(LAUNCHER_LDFLAGS) $^ $(LIBS) -o $@

launcher_main.npdm: launcher_main.json
	$(DEVKITPRO)/tools/bin/npdmtool $< $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).nro $(TARGET).map $(LAUNCHER_TARGET).elf $(LAUNCHER_TARGET).nso launcher_main.npdm homeassistant_launcher.map
