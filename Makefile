PGROOT := postgresql
PGINST := $(PGROOT)/installed
PGSRC  := $(PGROOT)/src

CC := cc

# Single set of flags — single process, single ABI, no frontend/backend split.
CFLAGS := -g -O0 \
	-I$(PGINST)/include/server \
	-I$(PGINST)/include/internal \
	-I$(PGROOT)/src/include \
	-I$(PGROOT)/src/backend \
	-I$(PGINST)/include \
	-I$(PGROOT)/src/interfaces/libpq \
	-I$(PGROOT)/src/timezone \
	-I/opt/homebrew/opt/icu4c@78/include

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

VPG_OBJS    := vpg.o vpg_initdb.o vpg_findtimezone.o vpg_fe_shim.o
VPG_ARCHIVE := libvpg.a

.PHONY: all clean

all: $(VPG_ARCHIVE)

FEUTILS_NEEDED := \
	$(PGROOT)/src/fe_utils/option_utils.o

$(VPG_ARCHIVE): $(VPG_OBJS)
	rm -f $@
	ar rcs $@ $(VPG_OBJS) $(BACKEND_OBJS) $(FEUTILS_NEEDED)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(VPG_ARCHIVE) $(VPG_OBJS)
