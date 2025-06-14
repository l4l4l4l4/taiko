DFU_ARGS = -a 0 -s 0x8000000

flash: 
	dfu-util $DFU_ARGS -D main.bin
