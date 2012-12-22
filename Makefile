VERSION= 0.3
# 
# Makefile for pgpmac project
# (C) 2012 by Keith Brister and Northwesern University
# All Rights Reserved
#
pgpmac: pgpmac.c pgpmac.h lspg.o lsredis.o lspmac.o md2cmds.o lslogging.o lsevents.o lstimer.o ini.o redis_ini.o pgpmac_rpc_svc.o pgpmac_rpc_xdr.o pgpmac_rpc_proc.o Makefile
	gcc -g -pthread -o pgpmac pgpmac.c md2cmds.o lspmac.o lspg.o lsredis.o lslogging.o lsevents.o lstimer.o ini.o redis_ini.o pgpmac_rpc_svc.o pgpmac_rpc_xdr.o pgpmac_rpc_proc.o libhiredis.a -lpq -lncurses -lpthread -lrt -lm

dist:
	ln -fs . ls-cat-pgpmac-$(VERSION)
	tar czvf ls-cat-pgpmac-$(VERSION).tar.gz ls-cat-pgpmac-$(VERSION)/*.c ls-cat-pgpmac-$(VERSION)/*.h ls-cat-pgpmac-$(VERSION)/pmac_md2.sql ls-cat-pgpmac-$(VERSION)/Makefile ls-cat-pgpmac-$(VERSION)/pmac_md2_ls-cat.pmc ls-cat-pgpmac-$(VERSION)/pgpmac.pdf
	rm -f ls-cat-pgpmac-$(VERSION)

clean:
	rm md2cmds.o lspmac.o lspg.o lsupdate.o lslogging.o lsredis.o lsevents.o lstimer.o lskvs.o ini.o redis_ini.o pgpmac_rpc_svc.o pgpmac_rpc_xdr.o pgpmac_rpc_proc.o pgpmac

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

ini.o: ini.c ini.h Makefile
	gcc -g -c ini.c

redis_ini.o: redis_ini.c ini.h Makefile
	gcc -g -c redis_ini.c

pgpmac_rpc_svc.o: pgpmac_rpc_svc.c pgpmac_rpc.h Makefile
	gcc -g -c pgpmac_rpc_svc.c

pgpmac_rpc_xdr.o: pgpmac_rpc_xdr.c pgpmac_rpc.h Makefile
	gcc -g -c pgpmac_rpc_xdr.c

pgpmac_rpc_call.o: pgpmac_rpc_call.c pgpmac_rpc.h Makefile
	gcc -g -c pgpmac_rpc_call.c

pgpmac_rpc_proc.o: pgpmac_rpc_proc.c pgpmac_rpc.h Makefile
	gcc -g -c pgpmac_rpc_proc.c

#
# Not really part of the project.  This is a connector routine to synchronize redis and px.kvs
#
kvredis: kvredis.c Makefile
	gcc -g kvredis.c -o kvredis -I /usr/local/include -L /usr/local/lib -lhiredis -lpq
