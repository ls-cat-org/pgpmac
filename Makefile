# 
# Makefile for pgpmac project
# (C) 2012 by Keith Brister and Northwesern University
# All Rights Reserved
#
pgpmac: pgpmac.c pgpmac.h lspg.o lsredis.o lspmac.o md2cmds.o lsupdate.o lslogging.o lsevents.o lstimer.o lskvs.o Makefile
	gcc -g -pthread -o pgpmac pgpmac.c md2cmds.o lspmac.o lspg.o lsredis.o lsupdate.o lslogging.o lsevents.o lstimer.o lskvs.o -lpq -lncurses -lpthread -lrt -lhiredis

clean:
	rm *.o pgpmac

docs:
	doxygen

lskvs.o: lskvs.c pgpmac.h Makefile
	gcc -g -pthread -c lskvs.c

lstimer.o: lstimer.c pgpmac.h Makefile
	gcc -g -pthread -c lstimer.c

lsevents.o: lsevents.c pgpmac.h Makefile
	gcc -g -pthread -c lsevents.c

lslogging.o: lslogging.c pgpmac.h Makefile
	gcc -g -pthread -c lslogging.c

lsupdate.o: lsupdate.c pgpmac.h Makefile
	gcc -g -pthread -c lsupdate.c

lsredis.o: lsredis.c pgpmac.h Makefile
	gcc -g -pthread -c lsredis.c

lspmac.o: lspmac.c pgpmac.h Makefile
	gcc -g -pthread -c lspmac.c

lspg.o: lspg.c pgpmac.h Makefile
	gcc -g -pthread -c lspg.c

md2cmds.o: md2cmds.c pgpmac.h Makefile
	gcc -g -pthread -c md2cmds.c


kvredis: kvredis.c Makefile
	gcc -g kvredis.c -o kvredis -I /usr/local/include -L /usr/local/lib -lhiredis -lpq
