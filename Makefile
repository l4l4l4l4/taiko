# How to enter DFU
# 1. Hold down boot0.
# 2. Press and release rst.
# 3. Release boot0.

DFU_ARGS = -a 0 -s 0x8000000
BIN_PATH = ./target/thumbv7em-none-eabi/release/taiko

flash: 
	dfu-util $(DFU_ARGS) -D $(BIN_PATH) 
