# Art Swap helper (artswapd) - the payload, and the same code for a Mac or Linux host.
#
#   make                 build/helper/artswapd.elf  (PS5_PAYLOAD_SDK: the payload SDK)
#   make host            build/helper-host/artswapd (libcurl; the console's paths under $ARTSWAP_ROOT)
#
# Run through helper/tools/build.sh, which finds the SDK (in the VM).
#
# Copyright (C) 2026 Gary Riches
# SPDX-License-Identifier: GPL-3.0-or-later

ROOT := $(abspath $(CURDIR)/..)
OUT := $(ROOT)/build/helper
HOST_OUT := $(ROOT)/build/helper-host

SOURCES := src/artswapd.c src/art.c src/home.c src/name.c src/phone.c src/scan.c src/sgdb.c src/smp.c src/image.c src/bc7enc.c src/config.c \
	src/util.c
THIRD := src/stb_impl.c third_party/cJSON.c
SQLITE := third_party/sqlite3.c
SQLITE_FLAGS := -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
	-DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_TEMP_STORE=3 -DSQLITE_OMIT_DEPRECATED=1 -DSQLITE_OMIT_SHARED_CACHE=1 \
	-DSQLITE_DQS=0
WARN := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
HEADERS := $(wildcard src/*.h)

.PHONY: all host clean
all: $(OUT)/artswapd.elf
host: $(HOST_OUT)/artswapd

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
endif

$(OUT) $(HOST_OUT):
	mkdir -p $@

# ---- the payload ----
$(OUT)/%.o: src/%.c $(HEADERS) | $(OUT)
	$(CC) -std=gnu11 -O2 -g $(WARN) -I$(OUT) -c -o $@ $<
$(OUT)/cJSON.o: third_party/cJSON.c | $(OUT)
	$(CC) -std=gnu11 -O2 -w -c -o $@ $<
$(OUT)/sqlite3.o: $(SQLITE) | $(OUT)
	$(CC) -std=gnu11 -O2 -w $(SQLITE_FLAGS) -c -o $@ $<
$(OUT)/stb_impl.o: src/stb_impl.c | $(OUT)
	$(CC) -std=gnu11 -O2 -w -c -o $@ $<

PS5_OBJECTS := $(patsubst src/%.c,$(OUT)/%.o,$(SOURCES) src/http_ps5.c src/stb_impl.c) $(OUT)/cJSON.o $(OUT)/sqlite3.o
$(OUT)/artswapd.elf: $(PS5_OBJECTS)
	$(CC) -o $@ $^ -lpthread

# ---- the network test payload (helper/tests/nettest.c) ----
NET_OBJECTS := $(OUT)/util.o $(OUT)/http_ps5.o $(OUT)/config.o
$(OUT)/nettest.o: tests/nettest.c $(HEADERS) | $(OUT)
	$(CC) -std=gnu11 -O2 -g $(WARN) -c -o $@ $<
$(OUT)/nettest.elf: $(OUT)/nettest.o $(NET_OBJECTS)
	$(CC) -o $@ $^ -lpthread
nettest: $(OUT)/nettest.elf
$(OUT)/tlstest.o: tests/tlstest.c $(HEADERS) | $(OUT)
	$(CC) -std=gnu11 -O2 -g $(WARN) -c -o $@ $<
$(OUT)/tlstest.elf: $(OUT)/tlstest.o $(OUT)/util.o
	$(CC) -o $@ $^ -lpthread
tlstest: $(OUT)/tlstest.elf
# the ShadowMountPlus refresh test payload (helper/tests/smptest.c)
SMPTEST_ID ?= PPSA99305
SMPTEST_FLAGS := -DSMPTEST_ID='"$(SMPTEST_ID)"' $(if $(SMPTEST_REFRESH),-DSMPTEST_REFRESH) \
	$(if $(SMPTEST_SNAPSHOT),-DSMPTEST_SNAPSHOT='"$(SMPTEST_SNAPSHOT)"') $(if $(SMPTEST_ROOT),-DSMPTEST_ROOT)
$(OUT)/smptest.elf: tests/smptest.c $(OUT)/smp.o $(OUT)/util.o $(OUT)/cJSON.o $(HEADERS) FORCE | $(OUT)
	$(CC) -std=gnu11 -O2 -g $(WARN) $(SMPTEST_FLAGS) -c -o $(OUT)/smptest.o tests/smptest.c
	$(CC) -o $@ $(OUT)/smptest.o $(OUT)/smp.o $(OUT)/util.o $(OUT)/cJSON.o -lpthread
smptest: $(OUT)/smptest.elf
.PHONY: nettest tlstest smptest FORCE

# ---- the host build, for tests ----
HOST_CC ?= cc
HOST_FLAGS := -std=gnu11 -O2 -g -DARTSWAP_HOST -I$(OUT)
$(HOST_OUT)/artswapd: $(SOURCES) src/http_host.c $(HEADERS) | $(HOST_OUT)
	$(HOST_CC) $(HOST_FLAGS) -w -c -o $(HOST_OUT)/sqlite3.o $(SQLITE_FLAGS) $(SQLITE)
	$(HOST_CC) $(HOST_FLAGS) -w -c -o $(HOST_OUT)/cJSON.o third_party/cJSON.c
	$(HOST_CC) $(HOST_FLAGS) -w -c -o $(HOST_OUT)/stb_impl.o src/stb_impl.c
	$(HOST_CC) $(HOST_FLAGS) $(WARN) -o $@ $(SOURCES) src/http_host.c $(HOST_OUT)/sqlite3.o $(HOST_OUT)/cJSON.o \
		$(HOST_OUT)/stb_impl.o -lcurl -lpthread -lm

clean:
	rm -rf $(OUT) $(HOST_OUT)
