VERSION= 1.2
# 
# Makefile for pgpmac project
# (C) 2012 - 2018 by Keith Brister and Northwesern University
# All Rights Reserved
#

#
# RHEL puts the postgresql includes in /usr/include while Ubuntu puts them in /usr/include/postgresql
# This define allows us to use the same Makefile and pgpmac.h on both systems without changes.
# Consider autoconf if we start running into too many things like this.
#
PG_INCLUDE = -I /usr/include/postgresql

pgpmac: pgpmac.c pgpmac.h lspg.o lsredis.o lspmac.o md2cmds.o lslogging.o lsevents.o lstimer.o lstest.o lsdetectorstate.o lsraster.o Makefile
	gcc -g -pthread -o pgpmac pgpmac.c  ${PG_INCLUDE} -Wall md2cmds.o lspmac.o lspg.o lsredis.o lslogging.o lsevents.o lstimer.o lsdetectorstate.o lstest.o lsraster.o -lpq -lncurses -lpthread -lrt -lhiredis -lm -ljansson

dist:
	ln -fs . ls-cat-pgpmac-$(VERSION)
	tar czvf ls-cat-pgpmac-$(VERSION).tar.gz ls-cat-pgpmac-$(VERSION)/*.c ls-cat-pgpmac-$(VERSION)/*.h ls-cat-pgpmac-$(VERSION)/pmac_md2.sql ls-cat-pgpmac-$(VERSION)/Makefile ls-cat-pgpmac-$(VERSION)/21-ID-*/*.pmc
	rm -f ls-cat-pgpmac-$(VERSION)

install: pgpmac maybe_install_pgpmac_rsyslogd.sh pgpmac.logrotate
	install -p pgpmac /usr/local/bin
	install --mode 0644 pgpmac.logrotate /etc/logrotate.d/pgpmac
	./maybe_install_pgpmac_rsyslogd.sh

.SILENT: clean
clean:
	$(RM) *.o pgpmac 2>/dev/null
	$(RM) ls-cat-pgpmac-*.gz 2>/dev/null

.SILENT: distclean
distclean:
	-@rm *.o pgpmac 2>/dev/null
	-@rm ls-cat-pgpmac-*.gz

docs:   *.c *.h Makefile
	/usr/bin/doxygen
	(cd docs/latex && make)

lsraster.o: lsraster.c pgpmac.h Makefile
	gcc -g -pthread -c lsraster.c ${PG_INCLUDE} -Wall

lstimer.o: lstimer.c pgpmac.h Makefile
	gcc -g -pthread -c lstimer.c ${PG_INCLUDE} -Wall

lsdetectorstate.o: lsdetectorstate.c pgpmac.h Makefile
	gcc -g -pthread -c lsdetectorstate.c ${PG_INCLUDE} -Wall

lsevents.o: lsevents.c pgpmac.h Makefile
	gcc -g -pthread -c lsevents.c ${PG_INCLUDE} -Wall

lslogging.o: lslogging.c pgpmac.h Makefile
	gcc -g -pthread -c lslogging.c ${PG_INCLUDE} -Wall

lsredis.o: lsredis.c pgpmac.h Makefile
	gcc -g -pthread -c lsredis.c ${PG_INCLUDE} -Wall

lspmac.o: lspmac.c pgpmac.h Makefile
	gcc -g -pthread -c lspmac.c ${PG_INCLUDE} -Wall

lspg.o: lspg.c pgpmac.h Makefile
	gcc -g -pthread -c lspg.c ${PG_INCLUDE} -Wall

md2cmds.o: md2cmds.c pgpmac.h Makefile
	gcc -g -pthread -c md2cmds.c ${PG_INCLUDE}  -Wall

lstest.o: lstest.c pgpmac.h Makefile
	gcc -g -pthread -c lstest.c ${PG_INCLUDE} -Wall

