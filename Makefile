PGROOT := postgresql
PGINST := $(PGROOT)/installed
PGSRC  := $(PGROOT)/src

CC := cc

CFLAGS := -g -O0 \
	-I$(PGINST)/include/server \
	-I$(PGINST)/include/internal \
	-I$(PGROOT)/src/include \
	-I$(PGROOT)/src/backend \
	-I$(PGINST)/include \
	-I$(PGROOT)/src/interfaces/libpq \
	-I$(PGROOT)/src/timezone \
	-I/opt/homebrew/opt/icu4c@78/include \
	-I.

LDFLAGS := -L$(PGINST)/lib
PG_LIBS_RAW := $(shell $(PGINST)/bin/pg_config --libs)
PG_LIBS     := $(filter-out -lreadline -ledit -ltermcap -lncurses -lcurses,$(PG_LIBS_RAW))
ICU_LIBS    := $(shell awk -F'= ' '/^ICU_LIBS[[:space:]]*=/{print $$2}' $(PGROOT)/src/Makefile.global)
LIBS := $(PG_LIBS) $(ICU_LIBS) \
	-L$(PGROOT)/src/interfaces/libpq -lpq \
	$(shell $(PGINST)/bin/pg_config --ldflags)

BACKEND_OBJS := $(shell find $(PGROOT)/src/backend $(PGROOT)/src/timezone -type f -name '*.o')
BACKEND_OBJS := $(filter-out \
	$(PGROOT)/src/backend/main/main.o \
	$(PGROOT)/src/timezone/zic.o \
	$(PGROOT)/src/backend/utils/mb/conversion_procs/% \
	$(PGROOT)/src/backend/snowball/dict_snowball.o \
	$(PGROOT)/src/backend/replication/pgoutput/pgoutput.o \
	$(PGROOT)/src/backend/replication/libpqwalreceiver/libpqwalreceiver.o, \
	$(BACKEND_OBJS))

FEUTILS_NEEDED := \
	$(PGROOT)/src/fe_utils/option_utils.o

# ---- initdb sub-library ------------------------------------------------
INITDB_DIR  := initdb
INITDB_SRCS := $(wildcard $(INITDB_DIR)/*.c)
INITDB_OBJS := $(INITDB_SRCS:.c=.o)
INITDB_LIB  := $(INITDB_DIR)/libvpg_initdb.a

# ---- top-level core object ---------------------------------------------
CORE_OBJS   := vpg.o

# ---- combined archive --------------------------------------------------
VPG_ARCHIVE := libvpg.a

.PHONY: all clean

all: $(VPG_ARCHIVE)

# Build initdb objects (clang resolves "vpg_bootstrap.h" relative to
# the source file's own directory, so no extra -I needed)
$(INITDB_DIR)/%.o: $(INITDB_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

$(INITDB_LIB): $(INITDB_OBJS)
	rm -f $@
	ar rcs $@ $(INITDB_OBJS)

# Top-level vpg.o
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Pack everything into one archive the V linker sees
$(VPG_ARCHIVE): $(CORE_OBJS) $(INITDB_LIB)
	rm -f $@
	ar rcs $@ $(CORE_OBJS) $(INITDB_OBJS) $(BACKEND_OBJS) $(FEUTILS_NEEDED)

clean:
	rm -f $(VPG_ARCHIVE) $(CORE_OBJS) $(INITDB_OBJS) $(INITDB_LIB)
