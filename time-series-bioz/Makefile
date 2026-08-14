TARGET  = time_series_bioz
CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

CPU     = -mcpu=cortex-m3 -mthumb

# CHIPSEL_594X selects the AD5940 (vs AD5941/AD5940-M355) register map inside
# the vendored ad5940lib. ADI_DEBUG turns on BodyImpedance.c's extra status/
# calibration prints (RTIA cal results, start/stop), useful for bring-up.
CFLAGS  = $(CPU) -Os -Wall -Wextra -std=c11 -g \
          -ffunction-sections -fdata-sections \
          -I. -Iad5940lib -DCHIPSEL_594X -DADI_DEBUG
LDFLAGS = $(CPU) -nostartfiles -specs=nosys.specs \
          -Wl,-T,linker.ld -Wl,--gc-sections -Wl,-Map,$(TARGET).map
# -lm must come after the object files: the linker only pulls in a static
# library's members to resolve symbols already needed by what came before
# it on the command line.
LDLIBS  = -lm

SRCS = startup.c main.c ad5940_port.c BodyImpedance.c ad5940lib/ad5940.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET).bin

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# DAPLink shows up as a mass-storage drive; dropping a .bin onto it flashes
# the board. Override e.g. `make flash DAPLINK_MOUNT=/media/you/DAPLINK`.
DAPLINK_MOUNT ?= /media/$(USER)/DAPLINK

flash: $(TARGET).bin
	cp $(TARGET).bin $(DAPLINK_MOUNT)/
	sync

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).bin $(TARGET).map

.PHONY: all flash clean
