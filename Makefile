# 
# Makefile for pgpmac project
# (C) 2012 by Keith Brister and Northwesern University
# All Rights Reserved
#
pgpmac: pgpmac.c pgpmac.h lspg.o lspmac.o md2cmds.o lsupdate.o lslogging.o lsevents.o lstimer.o Makefile
	gcc -g -pthread -o pgpmac pgpmac.c md2cmds.o lspmac.o lspg.o lsupdate.o lslogging.o lsevents.o lstimer.o -lpq -lncurses -lpthread -lrt

clean:
	rm *.o pgpmac

docs:
	doxygen

lstimer.o: lstimer.c pgpmac.h Makefile
	gcc -g -pthread -c lstimer.c

lsevents.o: lsevents.c pgpmac.h Makefile
	gcc -g -pthread -c lsevents.c

lslogging.o: lslogging.c pgpmac.h Makefile
	gcc -g -pthread -c lslogging.c

lsupdate.o: lsupdate.c pgpmac.h Makefile
	gcc -g -pthread -c lsupdate.c

lspmac.o: lspmac.c pgpmac.h Makefile
	gcc -g -pthread -c lspmac.c

lspg.o: lspg.c pgpmac.h Makefile
	gcc -g -pthread -c lspg.c

md2cmds.o: md2cmds.c pgpmac.h Makefile
	gcc -g -pthread -c md2cmds.c