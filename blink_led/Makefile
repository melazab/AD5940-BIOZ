TARGET  = blink_led
CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

CPU     = -mcpu=cortex-m3 -mthumb
CFLAGS  = $(CPU) -Os -ffreestanding -Wall -Wextra -std=c11 -g
LDFLAGS = $(CPU) -nostartfiles -nodefaultlibs -lgcc \
          -Wl,-T,linker.ld -Wl,--gc-sections -Wl,-Map,$(TARGET).map

SRCS = startup.c blink_led.c
OBJS = $(SRCS:.c=.o)

all: $(TARGET).bin

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)
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
