PGROOT := postgresql
PGINST := $(PGROOT)/installed
PGSRC := $(PGROOT)/src

CC := cc
CFLAGS := -g -O0 -I$(PGINST)/include/server -I$(PGINST)/include/internal -I$(PGROOT)/src/include -I$(PGROOT)/src/backend
LDFLAGS := -L$(PGINST)/lib
PG_LIBS_RAW := $(shell $(PGINST)/bin/pg_config --libs)
PG_LIBS := $(filter-out -lreadline -ledit -ltermcap -lncurses -lcurses,$(PG_LIBS_RAW))
ICU_LIBS := $(shell awk -F'= ' '/^ICU_LIBS[[:space:]]*=/{print $$2}' $(PGROOT)/src/Makefile.global)
LIBS := $(PG_LIBS) $(ICU_LIBS) -L$(PGROOT)/src/interfaces/libpq -lpq $(shell $(PGINST)/bin/pg_config --ldflags)
BACKEND_OBJS := $(shell find $(PGROOT)/src/backend $(PGROOT)/src/timezone -type f -name '*.o')
BACKEND_OBJS := $(filter-out \
	$(PGROOT)/src/backend/main/main.o \
	$(PGROOT)/src/timezone/zic.o \
	$(PGROOT)/src/backend/utils/mb/conversion_procs/% \
	$(PGROOT)/src/backend/snowball/dict_snowball.o \
	$(PGROOT)/src/backend/replication/pgoutput/pgoutput.o \
	$(PGROOT)/src/backend/replication/libpqwalreceiver/libpqwalreceiver.o, \
	$(BACKEND_OBJS))
VPG_OBJS := vpg.o
VPG_ARCHIVE := libvpg.a

.PHONY: all clean

all: $(VPG_ARCHIVE) test_vpg

$(VPG_ARCHIVE): $(VPG_OBJS)
	rm -f $@
	ar rcs $@ $(VPG_OBJS) $(BACKEND_OBJS)

vpg.o: vpg.c
	$(CC) $(CFLAGS) -c vpg.c -o $@

test_vpg: test.c $(VPG_ARCHIVE)
	$(CC) $(CFLAGS) test.c $(VPG_ARCHIVE) $(PGROOT)/src/common/libpgcommon_srv.a $(PGROOT)/src/port/libpgport_srv.a $(LDFLAGS) $(LIBS) -o $@

clean:
	rm -f test_vpg $(VPG_ARCHIVE) $(VPG_OBJS)
