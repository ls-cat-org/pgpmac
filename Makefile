VERSION= 0.3
# 
# Makefile for pgpmac project
# (C) 2012 by Keith Brister and Northwesern University
# All Rights Reserved
#
pgpmac: pgpmac.c pgpmac.h lspg.o lsredis.o lspmac.o md2cmds.o lslogging.o lsevents.o lstimer.o Makefile
	gcc -g -pthread -o pgpmac pgpmac.c  -Wall md2cmds.o lspmac.o lspg.o lsredis.o lslogging.o lsevents.o lstimer.o -lpq -lncurses -lpthread -lrt -lhiredis -lm

dist:
	ln -fs . ls-cat-pgpmac-$(VERSION)
	tar czvf ls-cat-pgpmac-$(VERSION).tar.gz ls-cat-pgpmac-$(VERSION)/*.c ls-cat-pgpmac-$(VERSION)/*.h ls-cat-pgpmac-$(VERSION)/pmac_md2.sql ls-cat-pgpmac-$(VERSION)/Makefile ls-cat-pgpmac-$(VERSION)/pmac_md2_ls-cat.pmc ls-cat-pgpmac-$(VERSION)/pgpmac.pdf
	rm -f ls-cat-pgpmac-$(VERSION)

clean:
	rm *.o pgpmac

docs:
	/usr/local/bin/doxygen

lstimer.o: lstimer.c pgpmac.h Makefile
	gcc -g -pthread -c lstimer.c -Wall

lsevents.o: lsevents.c pgpmac.h Makefile
	gcc -g -pthread -c lsevents.c -Wall

lslogging.o: lslogging.c pgpmac.h Makefile
	gcc -g -pthread -c lslogging.c -Wall

lsredis.o: lsredis.c pgpmac.h Makefile
	gcc -g -pthread -c lsredis.c -Wall

lspmac.o: lspmac.c pgpmac.h Makefile
	gcc -g -pthread -c lspmac.c -Wall

lspg.o: lspg.c pgpmac.h Makefile
	gcc -g -pthread -c lspg.c -Wall

md2cmds.o: md2cmds.c pgpmac.h Makefile
	gcc -g -pthread -c md2cmds.c -Wall


#
# Not really part of the project.  This is a connector routine to synchronize redis and px.kvs
#
kvredis: kvredis.c Makefile
	gcc -g kvredis.c -o kvredis -I /usr/local/include -L /usr/local/lib -lhiredis -lpq
