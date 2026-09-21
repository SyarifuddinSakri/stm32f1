PROJECT = prog
BUILD_DIR = bin

SHARED_DIR = ../my-common-code ../sylib/debug/ ../sylib/esp8266 ../sylib/flash
CFILES = main.c debug.c esp8266.c eeprom.c
CFILES += api.c
CXXFILES += function.cxx
AFILES += api-asm.S

# TODO - you will need to edit these two lines!
DEVICE=stm32f103c8t6
OOCD_FILE = board/stm32f1discovery.cfg

# You shouldn't have to edit anything below here.
VPATH += $(SHARED_DIR)
INCLUDES += $(patsubst %,-I%, . $(SHARED_DIR))
OPENCM3_DIR=../libopencm3

include ../libopencm3/mk/genlink-config.mk
#using LDSCRIPT after the above include to override Linker file usage
LDSCRIPT=stm32f103c8t6.ld
include ../rules.mk
#Stop creating new Linker file 
GENERATED_BINS := $(filter-out $(LDSCRIPT),$(GENERATED_BINS))
