ifeq ($(strip $(DEVKITPPC)),)
$(error "DEVKITPPC not set — run inside devkitpro/devkitppc Docker image")
endif

include $(DEVKITPPC)/wii_rules

TARGET    := boot
BUILD     := build
SOURCES   := source

CFLAGS    := -g -O2 -Wall $(MACHDEP) $(INCLUDE) -I$(DEVKITPRO)/libogc/include
CXXFLAGS  := $(CFLAGS)
LDFLAGS   := -g $(MACHDEP) -Wl,-Map,$(notdir $@).map -L$(DEVKITPRO)/libogc/lib/wii

LIBS      := -lwiiuse -lbte -lwiikeyboard -logc -lm -lfat

include $(DEVKITPPC)/base_rules

DEPSDIR   := $(CURDIR)/$(BUILD)
OFILES    := $(SOURCES)/main.o

all: $(BUILD) $(TARGET).dol

$(BUILD):
	mkdir -p $@

$(TARGET).elf: $(OFILES)

$(TARGET).dol: $(TARGET).elf
	@elf2dol $< $@

clean:
	rm -rf $(BUILD) *.elf *.dol *.map

-include $(DEPSDIR)/*.d
