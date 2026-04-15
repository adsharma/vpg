PGROOT := postgresql
PGINST := $(PGROOT)/installed
PGSRC  := $(PGROOT)/src

CC := cc

# Backend (server) compiler flags
CFLAGS := -g -O0 \
	-I$(PGINST)/include/server \
	-I$(PGINST)/include/internal \
	-I$(PGROOT)/src/include \
	-I$(PGROOT)/src/backend

# Frontend (initdb) compiler flags — uses postgres_fe.h + fe_utils
INITDB_CFLAGS := -g -O0 \
	-DFRONTEND \
	-DUSE_PRIVATE_ENCODING_FUNCS \
	-I$(PGINST)/include \
	-I$(PGINST)/include/internal \
	-I$(PGROOT)/src/include \
	-I$(PGROOT)/src/interfaces/libpq \
	-I$(PGROOT)/src/timezone \
	-I/opt/homebrew/opt/icu4c@78/include \
	$(shell $(PGINST)/bin/pg_config --cflags)

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

VPG_OBJS    := vpg.o vpg_initdb.o vpg_findtimezone.o vpg_localtime.o
VPG_ARCHIVE := libvpg.a

# Frontend libs needed by initdb code
FE_LIBS := $(PGROOT)/src/fe_utils/libpgfeutils.a \
           $(PGROOT)/src/common/libpgcommon.a \
           $(PGROOT)/src/port/libpgport.a

.PHONY: all clean

all: $(VPG_ARCHIVE)

$(VPG_ARCHIVE): $(VPG_OBJS)
	rm -f $@
	ar rcs $@ $(VPG_OBJS) $(BACKEND_OBJS)

vpg.o: vpg.c vpg.h
	$(CC) $(CFLAGS) -c vpg.c -o $@

vpg_initdb.o: vpg_initdb.c
	$(CC) $(INITDB_CFLAGS) -c vpg_initdb.c -o $@

vpg_findtimezone.o: vpg_findtimezone.c
	$(CC) $(INITDB_CFLAGS) -c vpg_findtimezone.c -o $@

vpg_localtime.o: vpg_localtime.c
	$(CC) $(INITDB_CFLAGS) -c vpg_localtime.c -o $@

clean:
	rm -f $(VPG_ARCHIVE) $(VPG_OBJS)
