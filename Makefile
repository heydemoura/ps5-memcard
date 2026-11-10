PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
VERSION ?= 0.4.0

PS5_CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_INCDIR := $(PS5_PAYLOAD_SDK)/target/include
PS5_LIBDIR := $(PS5_PAYLOAD_SDK)/target/lib

CFLAGS := -O2 -Wall -D_BSD_SOURCE -std=gnu11 -Isrc -I$(PS5_INCDIR)
SQLITE_FLAGS := -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_WAL -w
LDFLAGS := -L$(PS5_LIBDIR)
LIBS := -lkernel_sys -lkernel -lSceSystemService -lSceUserService -lSceFsInternalForVsh -lSceRegMgr -lSceNet -lSceAppInstUtil -lSceNotification
CMP_SRCS := $(wildcard src/CMP/*.c)
HOST_CC ?= cc
HOST_CFLAGS := -O1 -g -Wall -Wextra -Wno-format-truncation -Isrc
SIM_PORT ?= 8083

all: ps5-memcard.elf

.DELETE_ON_ERROR:

# The page and three.js are embedded with .incbin (see main.c), so no xxd is needed.
# The 3D icon code is kept in its own file and inlined into the page here.
build/ui.html: src/ui.html src/card3d.js Makefile
	@mkdir -p build
	sed -e 's/__VERSION__/v$(VERSION)/g' -e '/__CARD3D__/{r src/card3d.js' -e 'd' -e '}' $< > $@

# three.js is served by the payload itself (the page must work without internet),
# gzipped to keep the ELF small.
build/three.module.min.js.gz: vendor/three.module.min.js
	@mkdir -p build
	gzip -9 -n -c $< > $@

src/sqlite3.o: src/sqlite3.c
	$(PS5_CC) -O2 -D_BSD_SOURCE -std=gnu11 -I$(PS5_INCDIR) $(SQLITE_FLAGS) -c -o $@ $<

src/miniz_tinfl.o: src/miniz_tinfl.c src/miniz_tinfl.h src/miniz_common.h
	$(PS5_CC) -O2 -D_BSD_SOURCE -std=gnu11 -Isrc -I$(PS5_INCDIR) -w -c -o $@ $<

ps5-memcard.elf: src/main.c src/memcard.c src/memcard.h src/homescreen.c src/homescreen.h assets/param.json assets/icon0.png build/ui.html build/three.module.min.js.gz src/sqlite3.o src/miniz_tinfl.o $(CMP_SRCS) $(wildcard src/CMP/*.h) $(wildcard src/CMP/internal/*.h)
	$(PS5_CC) $(CFLAGS) -DGARLIC_VERSION='"$(VERSION)"' -DUI_HTML_PATH='"build/ui.html"' $(LDFLAGS) \
		-o $@ src/main.c src/memcard.c src/homescreen.c $(CMP_SRCS) src/sqlite3.o src/miniz_tinfl.o $(LIBS)

# ── Host side: no console needed ──────────────────────────────────
build/memcard_test: tests/memcard_test.c src/memcard.c src/memcard.h
	@mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -fsanitize=address,undefined -o $@ tests/memcard_test.c src/memcard.c -lpthread

build/sim: tests/sim.c src/memcard.c src/memcard.h
	@mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -o $@ tests/sim.c src/memcard.c -lpthread

test: build/memcard_test
	build/memcard_test

# The real page and the real memcard core against a fake console in /tmp.
sim: build/sim build/ui.html build/three.module.min.js.gz
	build/sim --port $(SIM_PORT) --ui build/ui.html

# Re-render assets/icon0.png from the 3D card model (needs npm install and Chromium).
icon:
	node tools/make-icon.cjs

clean:
	rm -rf build ps5-memcard.elf src/sqlite3.o src/miniz_tinfl.o

.PHONY: all clean test sim icon
