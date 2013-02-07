VERSION= 0.5
# 
# Makefile for pgpmac project
# (C) 2012 - 2013 by Keith Brister and Northwesern University
# All Rights Reserved
#

#
# RHEL puts the postgresql includes in /usr/include while Ubuntu puts them in /usr/include/postgresql
# This define allows us to use the same Makefile and pgpmac.h on both systems without changes.
# Consider autoconf if we start running into too many things like this.
#
PG_INCLUDE = -I /usr/include/postgresql

pgpmac: pgpmac.c pgpmac.h lspg.o lsredis.o lspmac.o md2cmds.o lslogging.o lsevents.o lstimer.o lstest.o Makefile
	gcc -g -pthread -o pgpmac pgpmac.c  ${PG_INCLUDE} -Wall md2cmds.o lspmac.o lspg.o lsredis.o lslogging.o lsevents.o lstimer.o lstest.o -lpq -lncurses -lpthread -lrt -lhiredis -lm

dist:
	ln -fs . ls-cat-pgpmac-$(VERSION)
	tar czvf ls-cat-pgpmac-$(VERSION).tar.gz ls-cat-pgpmac-$(VERSION)/*.c ls-cat-pgpmac-$(VERSION)/*.h ls-cat-pgpmac-$(VERSION)/pmac_md2.sql ls-cat-pgpmac-$(VERSION)/Makefile ls-cat-pgpmac-$(VERSION)/pmac_md2_ls-cat.pmc ls-cat-pgpmac-$(VERSION)/pgpmac.pdf
	rm -f ls-cat-pgpmac-$(VERSION)

clean:
	rm *.o pgpmac

docs:   *.c *.h Makefile
	/usr/local/bin/doxygen
	(cd docs/latex && make)

lstimer.o: lstimer.c pgpmac.h Makefile
	gcc -g -pthread -c lstimer.c ${PG_INCLUDE} -Wall

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

