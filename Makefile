######################################
# DPCAP47 - DP station-keeping capability (after DNV-ST-0111 level 1) for the SwissMicros DM42n (DMCP5 .pg5)
# Based on the SwissMicros DMCP5_SDK Makefile (BSD-3), as HP47, ZX47, Chess 47, NAV47, TERM47 and GB47
######################################
# make              build/DPCAP47.pg5 (loads on DMCP5 3.57 and later: interface 3.17)
# make test         the engine's checks on the PC (test/dptest.c)
# make table F=x.DPV   the level 1 analysis of a vessel file, as a table, on the PC
# make lpcheck     the allocation against an exact LP, every direction, case and Beaufort number (scipy)
# make pc [F=x.DPV]  the PC program (GTK 4): DNV level 1, level 2, ABS SKP side by side
# make pccheck     the PC program's methods checked (level 1 the same as the calculator's)
# make sim          the PC simulator (build/sim/sim); make shots: its screens -> build/sim/run/*.png

TARGET    = DPCAP47
BUILD_DIR = build
PYTHON   ?= python3
PYTHON_GTK ?= python3
IFC      ?= 17

CC      = arm-none-eabi-gcc
AS      = arm-none-eabi-gcc -x assembler-with-cpp
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

C_SOURCES = dmcp/sys/pgm_syscalls.c src/main.c src/dpcap.c
ASM_SOURCES = dmcp/startup_pgm.s
C_INCLUDES = -Idmcp -Isrc

C_DEFS   = -D__weak="__attribute__((weak))" -D__packed="__attribute__((__packed__))" -DPLATFORM_IFC_VER=$(IFC)
CPUFLAGS = -mthumb -march=armv7e-m -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS   = -std=gnu11 $(CPUFLAGS) $(C_DEFS) $(C_INCLUDES) -Wall -fdata-sections -ffunction-sections -O2 -g \
           -fsingle-precision-constant
CFLAGS  += -MD -MP -MF $(BUILD_DIR)/.dep/$(@F).d
LDSCRIPT = stm32_program.ld
LDFLAGS  = $(CPUFLAGS) -T$(LDSCRIPT) -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections \
           -Wl,--wrap=_malloc_r -g
LIBS     = -lm

all: $(BUILD_DIR)/$(TARGET).pg5

OBJECTS  = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
vpath %.c $(sort $(dir $(C_SOURCES)))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)/.dep
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)/.dep
	$(AS) -c $(CFLAGS) $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) $(LIBS) -o $@

# QSPI CRC is compiled into the program header: if it changes, rebuild once.
$(BUILD_DIR)/$(TARGET).pg5: $(BUILD_DIR)/$(TARGET).elf tools/pg5tool.py
	$(OBJCOPY) --remove-section .qspi -O binary $< $(BUILD_DIR)/$(TARGET)_flash.bin
	$(OBJCOPY) --only-section   .qspi -O binary $< $(BUILD_DIR)/$(TARGET)_qspi.bin
	@if ! $(PYTHON) tools/pg5tool.py qspi $(BUILD_DIR)/$(TARGET)_qspi.bin src/qspi_crc.h; then \
	   rm -f $(BUILD_DIR)/pgm_syscalls.o $<; $(MAKE) --no-print-directory $@; exit $$?; fi
	$(PYTHON) tools/pg5tool.py pg5 $(BUILD_DIR)/$(TARGET)_flash.bin $@
	$(SIZE) $<

$(BUILD_DIR)/.dep:
	mkdir -p $@

# ------------------------------------------------------------------ PC tests
T = $(BUILD_DIR)/test
HOSTCC = gcc -O2 -Wall -Wextra -Isrc
test: $(T)/dptest
	$(T)/dptest

table: $(T)/dptest
	$(T)/dptest $(F)

# the allocation against an exact LP (needs scipy): make lpcheck F=vessels/PROPS-NOGEN.DPV
lpcheck: $(T)/dptest
	$(PYTHON) tools/lpcheck.py $(T)/dptest $(or $(F),vessels/EXAMPLE.DPV)

$(T)/dptest: test/dptest.c src/dpcap.c src/dpcap.h
	mkdir -p $(T)
	$(HOSTCC) -o $@ test/dptest.c src/dpcap.c -lm

# ------------------------------------------------------------------ the PC program (tools/dpcap47_gtk.py)
lib: $(BUILD_DIR)/libdpcap.so
$(BUILD_DIR)/libdpcap.so: src/dpcap.c src/dpcap.h
	mkdir -p $(BUILD_DIR)
	gcc -O2 -Wall -Wextra -shared -fPIC -Isrc -o $@ src/dpcap.c -lm

pc: lib
	$(PYTHON_GTK) tools/dpcap47_gtk.py $(F)

pccheck: lib
	$(PYTHON_GTK) tools/dpcap/check.py

# ------------------------------------------------------------------ the PC simulator
S = $(BUILD_DIR)/sim
sim:
	mkdir -p $(S)
	gcc -O2 -g -w '-D__packed=__attribute__((__packed__))' '-D__weak=__attribute__((weak))' -DDPCAP47_SIM \
	    -Itest/sim -Isrc -o $(S)/sim test/sim/sim.c src/main.c src/dpcap.c -lm

shots: sim
	sh test/shots.sh $(S) $(PYTHON)

clean:
	-rm -fR $(BUILD_DIR)

-include $(wildcard $(BUILD_DIR)/.dep/*)

.PHONY: all test table lpcheck lib pc pccheck sim shots clean
