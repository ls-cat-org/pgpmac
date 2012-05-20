



pgpmac: pgpmac.c lspmac.o Makefile
	gcc -g -o pgpmac pgpmac.c lspmac.o -lpq -lncurses

lspmac.o: lspmac.c Makefile
	gcc -g -c lspmac.c