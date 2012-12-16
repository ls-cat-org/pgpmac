VERSION= 0.2
# 
# Makefile for pgpmac project
# (C) 2012 by Keith Brister and Northwesern University
# All Rights Reserved
#
pgpmac: pgpmac.c pgpmac.h lspg.o lsredis.o lspmac.o md2cmds.o lslogging.o lsevents.o lstimer.o Makefile
	gcc -g -pthread -o pgpmac pgpmac.c md2cmds.o lspmac.o lspg.o lsredis.o lslogging.o lsevents.o lstimer.o -lpq -lncurses -lpthread -lrt -lhiredis

dist:
	ln -fs . ls-cat-pgpmac-$(VERSION)
	tar czvf ls-cat-pgpmac-$(VERSION).tar.gz ls-cat-pgpmac-$(VERSION)/*.c ls-cat-pgpmac-$(VERSION)/*.h ls-cat-pgpmac-$(VERSION)/pmac_md2.sql ls-cat-pgpmac-$(VERSION)/Makefile ls-cat-pgpmac-$(VERSION)/pmac_md2_ls-cat.pmc 
	rm -f ls-cat-pgpmac-$(VERSION)

clean:
	rm *.o pgpmac

docs:
	/usr/local/bin/doxygen

lstimer.o: lstimer.c pgpmac.h Makefile
	gcc -g -pthread -c lstimer.c

lsevents.o: lsevents.c pgpmac.h Makefile
	gcc -g -pthread -c lsevents.c

lslogging.o: lslogging.c pgpmac.h Makefile
	gcc -g -pthread -c lslogging.c

lsredis.o: lsredis.c pgpmac.h Makefile
	gcc -g -pthread -c lsredis.c

lspmac.o: lspmac.c pgpmac.h Makefile
	gcc -g -pthread -c lspmac.c

lspg.o: lspg.c pgpmac.h Makefile
	gcc -g -pthread -c lspg.c

md2cmds.o: md2cmds.c pgpmac.h Makefile
	gcc -g -pthread -c md2cmds.c


#
# Not really part of the project.  This is a connector routine to synchronize redis and px.kvs
#
kvredis: kvredis.c Makefile
	gcc -g kvredis.c -o kvredis -I /usr/local/include -L /usr/local/lib -lhiredis -lpq
