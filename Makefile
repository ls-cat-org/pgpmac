# 
# Makefile for pgpmac project
# (C) 2012 by Keith Brister and Northwesern University
# All Rights Reserved
#
pgpmac: pgpmac.c lspg.o lspmac.o Makefile
	gcc -g -o pgpmac pgpmac.c lspmac.o lspg.o -lpq -lncurses

clean:
	rm *.o

lspmac.o: lspmac.c Makefile
	gcc -g -c lspmac.c

lspg.o: lspg.c Makefile
	gcc -g -c lspg.c
