AVRDUDE_PART=m328p
GCC_PART=atmega328p
AVRDUDE =avrdude -c usbasp-clone -b 19200 -P usb -p $(AVRDUDE_PART)
AVRDUDE_UART =avrdude -c arduino -b 115200 -P ft232r -p $(AVRDUDE_PART)
#atmega 328p over ArduinoISP

COMPILE = avr-gcc -Wall -Os -Iusbdrv -I. -mmcu=$(GCC_PART) -DF_CPU=16000000UL -DDEBUG_LEVEL=0
# NEVER compile the final product with debugging! Any debug output will
# distort timing so that the specs can't be met.

OBJECTS = usbdrv/usbdrv.o usbdrv/usbdrvasm.o usbdrv/oddebug.o main.o
#OBJECTS = main.o

# symbolic targets:
all:	main.hex

.c.o:
	$(COMPILE) -c $< -o $@

.S.o:
	$(COMPILE) -x assembler-with-cpp -c $< -o $@
# "-x assembler-with-cpp" should not be necessary since this is the default
# file type for the .S (with capital S) extension. However, upper case
# characters are not always preserved on Windows. To ensure WinAVR
# compatibility define the file type manually.

.c.s:
	$(COMPILE) -S $< -o $@

flash:	all
	$(AVRDUDE) -U flash:w:main.hex:i

flash_uart:	all
	$(AVRDUDE_UART) -U flash:w:main.hex:i


# Fuse high byte:
# 0xdd = 1 1 0 1   1 1 0 1
#        ^ ^ ^ ^   ^ \-+-/ 
#        | | | |   |   +------ BODLEVEL 2..0 (brownout trigger level -> 2.7V)
#        | | | |   +---------- EESAVE (preserve EEPROM on Chip Erase -> not preserved)
#        | | | +-------------- WDTON (watchdog timer always on -> disable)
#        | | +---------------- SPIEN (enable serial programming -> enabled)
#        | +------------------ DWEN (debug wire enable)
#        +-------------------- RSTDISBL (disable external reset -> enabled)
#
# Fuse low byte:
# 0xe1 = 1 1 1 0   0 0 0 1
#        ^ ^ \+/   \--+--/
#        | |  |       +------- CKSEL 3..0 (clock selection -> HF PLL)
#        | |  +--------------- SUT 1..0 (BOD enabled, fast rising power)
#        | +------------------ CKOUT (clock output on CKOUT pin -> disabled)
#        +-------------------- CKDIV8 (divide clock by 8 -> don't divide)
flash_bootloader:
	#$(AVRDUDE) -v -e -U efuse:w:0xfd:m -U hfuse:w:0xd5:m -U lfuse:w:0xe2:m # internal 8MHz
	$(AVRDUDE) -v -e -U efuse:w:0xfd:m -U hfuse:w:0xd5:m -U lfuse:w:0xff:m #external 8MHz>
	$(AVRDUDE) -v -e -U flash:w:optiboot_atmega328.hex -U lock:w:0x0F:m 

fuse:
	$(AVRDUDE) -U hfuse:w:0xdd:m -U lfuse:w:0xe1:m

readcal:
	$(AVRDUDE) -U calibration:r:/dev/stdout:i | head -1


clean:
	rm -f main.hex main.lst main.obj main.cof main.list main.map main.eep.hex main.bin *.o usbdrv/*.o main.s usbdrv/oddebug.s usbdrv/usbdrv.s

# file targets:
main.bin:	$(OBJECTS)
	$(COMPILE) -o main.bin $(OBJECTS)

main.hex:	main.bin
	rm -f main.hex main.eep.hex
	avr-objcopy -j .text -j .data -O ihex main.bin main.hex
	./checksize main.bin 32000 2000
# do the checksize script as our last action to allow successful compilation
# on Windows with WinAVR where the Unix commands will fail.

disasm:	main.bin
	avr-objdump -d main.bin

cpp:
	$(COMPILE) -E main.c
