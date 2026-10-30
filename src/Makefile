PS5_HOST ?= ps5
PS5_PORT ?= 9021

ifdef PS5_PAYLOAD_SDK
    include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
else
    $(error PS5_PAYLOAD_SDK is undefined)
endif

PAYLOAD_VERSION ?= v1.0.0
ELF := PS5-HLTB-$(PAYLOAD_VERSION).elf

SOURCES := main.c games.c hltb.c server.c sqlite/sqlite3.c
HEADERS := config.h games.h hltb.h server.h sqlite/sqlite3.h
ASSETS  := web/index.html resources/ps4_logo.webp resources/ps5_logo.png resources/app_icon_sm.png

CPPFLAGS := -DPAYLOAD_VERSION=\"$(PAYLOAD_VERSION)\" \
            -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION \
            -DSQLITE_OMIT_SHARED_CACHE -DSQLITE_TEMP_STORE=2
CFLAGS := -Wall -Werror -Os
LDLIBS := -lkernel_sys -lSceSysmodule -lSceUserService -lSceSystemService -lSceNet -lSceSsl -lSceHttp -lSceHttp2

.PHONY: all clean test debug

all: $(ELF)

$(ELF): $(SOURCES) $(HEADERS) $(ASSETS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SOURCES) $(LDLIBS)

clean:
	rm -f PS5-HLTB-*.elf

test: $(ELF)
	$(PS5_DEPLOY) -h $(PS5_HOST) -p $(PS5_PORT) $^

debug: $(ELF)
	gdb-multiarch \
	-ex "set architecture i386:x86-64" \
	-ex "target extended-remote $(PS5_HOST):2159" \
	-ex "file $(ELF)" \
	-ex "remote put $(ELF) /data/$(ELF)" \
	-ex "set remote exec-file /data/$(ELF)" \
	-ex "start"
