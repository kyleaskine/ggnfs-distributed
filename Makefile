# yafu-sieve-server
#
# Standalone build. No dependency on the main yafu binary or its Makefiles —
# this server only needs vendored mongoose/cJSON/sqlite (all under vendor/).
#
# Usage:
#   make            -> build yafu-sieve-server
#   make clean      -> remove objects + binary
#   make update-mongoose / update-cjson / update-sqlite
#                   -> re-fetch a vendored library (manually invoked; not part
#                      of the default build, so a clean clone builds offline)

CC       ?= gcc
CSTD     ?= -std=c11
OPT      ?= -O2 -g
WARN     := -Wall

CFLAGS   := $(OPT) $(CSTD) $(WARN) -fPIC -D_FILE_OFFSET_BITS=64
LDFLAGS  :=
LIBS     := -lpthread -lm -ldl

# Vendored sources are compiled with relaxed warnings; we don't own them.
VENDOR_CFLAGS := $(OPT) -std=gnu99 -fPIC -w \
                 -D_GNU_SOURCE \
                 -DSQLITE_THREADSAFE=1 \
                 -DSQLITE_OMIT_LOAD_EXTENSION=1 \
                 -DSQLITE_DEFAULT_MEMSTATUS=0 \
                 -DSQLITE_USE_URI=1 \
                 -DMG_TLS=MG_TLS_NONE \
                 -DMG_ENABLE_LINES=1 \
                 -DMG_MAX_RECV_SIZE=536870912

INC := -I. -Ivendor

# Server: db + protocol + mongoose + cJSON + sqlite
SERVER_OBJS := server.o db.o protocol.o
SERVER_VENDOR_OBJS := vendor/mongoose.o vendor/cJSON.o vendor/sqlite3.o

# Client: protocol + sieve_executor + mongoose + cJSON  (no sqlite)
CLIENT_OBJS := client.o protocol.o sieve_executor.o
CLIENT_VENDOR_OBJS := vendor/mongoose.o vendor/cJSON.o

ALL_OWN_OBJS    := server.o db.o protocol.o client.o sieve_executor.o
ALL_VENDOR_OBJS := vendor/mongoose.o vendor/cJSON.o vendor/sqlite3.o

SERVER_BIN := yafu-sieve-server
CLIENT_BIN := yafu-sieve-client

.PHONY: all server client clean update-mongoose update-cjson update-sqlite

# dashboard.html is embedded into the server binary so deploys stay
# single-file. Regenerated via xxd whenever the .html changes.
DASHBOARD_HEADER := dashboard_html.h

all: $(SERVER_BIN) $(CLIENT_BIN)

server: $(SERVER_BIN)
client: $(CLIENT_BIN)

$(SERVER_BIN): $(SERVER_OBJS) $(SERVER_VENDOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

$(CLIENT_BIN): $(CLIENT_OBJS) $(CLIENT_VENDOR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INC) -c -o $@ $<

# server.c #includes dashboard_html.h, so make sure it's regenerated first.
server.o: $(DASHBOARD_HEADER)

$(DASHBOARD_HEADER): dashboard.html
	xxd -i -n dashboard_html $< > $@

vendor/%.o: vendor/%.c
	$(CC) $(VENDOR_CFLAGS) $(INC) -c -o $@ $<

clean:
	rm -f $(ALL_OWN_OBJS) $(ALL_VENDOR_OBJS) $(SERVER_BIN) $(CLIENT_BIN) $(DASHBOARD_HEADER)

# ---------- vendor refresh helpers (manually invoked) ----------

MONGOOSE_TAG ?= 7.21
CJSON_TAG    ?= v1.7.19
SQLITE_TAG   ?= 3530100
SQLITE_YEAR  ?= 2026

update-mongoose:
	curl -fsSL -o vendor/mongoose.h \
	  https://raw.githubusercontent.com/cesanta/mongoose/$(MONGOOSE_TAG)/mongoose.h
	curl -fsSL -o vendor/mongoose.c \
	  https://raw.githubusercontent.com/cesanta/mongoose/$(MONGOOSE_TAG)/mongoose.c
	@echo "mongoose updated to $(MONGOOSE_TAG) — remember to bump vendor/VENDOR.md"

update-cjson:
	curl -fsSL -o vendor/cJSON.h \
	  https://raw.githubusercontent.com/DaveGamble/cJSON/$(CJSON_TAG)/cJSON.h
	curl -fsSL -o vendor/cJSON.c \
	  https://raw.githubusercontent.com/DaveGamble/cJSON/$(CJSON_TAG)/cJSON.c
	@echo "cJSON updated to $(CJSON_TAG) — remember to bump vendor/VENDOR.md"

update-sqlite:
	@TMP=$$(mktemp -d) && \
	 curl -fsSL -o $$TMP/sqlite.zip \
	   https://sqlite.org/$(SQLITE_YEAR)/sqlite-amalgamation-$(SQLITE_TAG).zip && \
	 python3 -c "import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" \
	   $$TMP/sqlite.zip $$TMP && \
	 mv $$TMP/sqlite-amalgamation-$(SQLITE_TAG)/sqlite3.c \
	    $$TMP/sqlite-amalgamation-$(SQLITE_TAG)/sqlite3.h \
	    $$TMP/sqlite-amalgamation-$(SQLITE_TAG)/sqlite3ext.h vendor/ && \
	 rm -rf $$TMP && \
	 echo "sqlite updated to $(SQLITE_TAG) — remember to bump vendor/VENDOR.md"
