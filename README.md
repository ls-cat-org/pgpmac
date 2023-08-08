# pmac

TODO: Please add more documentation here on what this does, and how to install it.

### Environment variables

LS_POSTGRES_DATABASE (default: "ls") : The name of the PostgreSQL database pgpmac uses for its message queue and state database.

LS_POSTGRES_HOSTNAME (default: "10.1.0.3") : The hostname of the PostgreSQL database server.

LS_POSTGRES_USERNAME (default: "lsuser") : The username used to connect to the PostgreSQL database. Only trust and ident authentication are supported, no password shall be provided for connection to the database.
