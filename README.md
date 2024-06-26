# (DEPRECATED) pgpmac - MD2 controller for 3-click centering 
ATTENTION: This library is deprecated, as the Arinax MD3 has its own Redis-based control interface.

pgpmac is the controller which moves the MD2 microdiffractometer to center the beam on a sample, a process known 
as 3-click centering.

It uses a Postgres database as a message queue; clients issue commands in the form of INSERT statements, which 
pgpmac consumes (SELECT+DELETE) and translates into Power PMAC commands to adjust the position of the various 
parts of the MD2.

pgpmac is also used by clients to query the current state of the MD2 via SELECT statements.

### Dependencies
##### RHEL/CentOS/Fedora:
postgresql-devel
ncurses-devel
hiredis-devel
jansson-devel

##### Ubuntu:
libpq-dev
libncurses-dev
libhiredis-dev
libjansson-dev

### Installation
make
sudo make install
python2 mk_pgpmac_redis.py
psql -f pmac_md2.sql
psql -f zoom_e.sql

### Configuration (Environment Variables)

LS_PMAC_HOSTNAME (default: "192.6.94.5") : The hostname of the Power PMAC server where pgpmac submits PMAC commands.

LS_POSTGRES_DATABASE (default: "ls") : The name of the PostgreSQL database pgpmac uses for its message queue and state database.

LS_POSTGRES_HOSTNAME (default: "postgres.ls-cat.net") : The hostname of the PostgreSQL database server.

LS_POSTGRES_USERNAME (default: "lsuser") : The username used to connect to the PostgreSQL database. Only trust and ident authentication are supported, no password shall be provided for connection to the database.
