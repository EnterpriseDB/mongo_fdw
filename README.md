# MongoDB Foreign Data Wrapper for PostgreSQL

This PostgreSQL extension implements a Foreign Data Wrapper (FDW) for
[MongoDB][1].

Please note that this version of mongo_fdw works with PostgreSQL and EDB
Postgres Advanced Server 10, 11, 12, 13, 14, and 15.

Installation
------------
About script or manual installation, mongo-c, laegacy and meta driver read [instructions in INSTALL.md](INSTALL.md)
If you run into any issues, please [let us know][2].

Enhancements
-----------
The following enhancements are added to the latest version of mongo_fdw:

### Write-able FDW
The previous version was only read-only, the latest version provides the
write capability. The user can now issue an insert, update, and delete
statements for the foreign tables using the mongo_fdw.

### Connection Pooling
The latest version comes with a connection pooler that utilizes the
same MongoDB database connection for all the queries in the same session.
The previous version would open a new [MongoDB][1] connection for every
query. This is a performance enhancement.

### JOIN push-down
mongo_fdw now also supports join push-down. The joins between two
foreign tables from the same remote MySQL server are pushed to a remote
server, instead of fetching all the rows for both the tables and
performing a join locally, thereby may enhance the performance. Currently,
joins involving only relational and arithmetic operators in join-clauses
are pushed down to avoid any potential join failure. Also, only the
INNER and LEFT/RIGHT OUTER joins are supported, and not the FULL OUTER,
SEMI, and ANTI join. Moreover, only joins between two tables are pushed
down and not when either inner or outer relation is the join itself.

### AGGREGATE push-down
mongo_fdw now also supports aggregate push-down. Push aggregates to the
remote MongoDB server instead of fetching all of the rows and aggregating
them locally. This gives a very good performance boost for the cases
where aggregates can be pushed down. The push-down is currently limited
to aggregate functions min, max, sum, avg, and count, to avoid pushing
down the functions that are not present on the MongoDB server. The
aggregate filters, orders, variadic and distinct are not pushed down.

### ORDER BY push-down
mongo_fdw now also supports order by push-down. If possible, push order
by clause to the remote server so that we get the ordered result set from
the foreign server itself. It might help us to have an efficient merge
join. NULLs behavior is opposite on the MongoDB server. Thus to get an
equivalent result, we can only push-down ORDER BY with either
ASC NULLS FIRST or DESC NULLS LAST. Moreover, as MongoDB sorts only on
fields, only column names in ORDER BY expressions are pushed down.

### LIMIT OFFSET push-down
mongo_fdw now also supports limit offset push-down. Wherever possible,
perform LIMIT and OFFSET operations on the remote server. This reduces
network traffic between local PostgreSQL and remote MongoDB servers.

### New MongoDB C Driver Support
This enhancement is to add a new [MongoDB][1]' C driver. The current
implementation is based on the legacy driver of MongoDB. But
[MongoDB][1] is provided completely new library for driver called
MongoDB's meta driver. Added support for the same. Now compile time
option is available to use legacy and meta driver.

In order to use MongoDB driver 1.17.0+, take the following steps:

  * clone `libmongoc` version 1.17.0+
    (https://github.com/mongodb/mongo-c-driver) and follow the install
    directions given there.  `libbson` is now maintained in a subdirectory
    of the `libmongoc`.
    (https://github.com/mongodb/mongo-c-driver/tree/master/src/libbson).
  * ensure pkg-config / pkgconf is installed on your system.
  * run `make -f Makefile.meta && make -f Makefile.meta install`
  * if you get an error when trying to `CREATE EXTENSION mongo_fdw;`,
    then try running `ldconfig`

Usage
-----
The following parameters can be set on a MongoDB foreign server object:

  * `address`: Address or hostname of the MongoDB server. Defaults to
    `127.0.0.1`
  * `port`: Port number of the MongoDB server. Defaults to `27017`.
  * `use_remote_estimate`: Controls whether mongo_fdw uses exact rows from
    remote collection to obtain cost estimates. Default is `false`.

The following options are only supported with meta driver:

  * `authentication_database`: Database against which user will be
    authenticated against. Only valid with password based authentication.
  * `replica_set`: Replica set the server is member of. If set,
    driver will auto-connect to correct primary in the replica set when
    writing.
  * `read_preference`: primary [default], secondary, primaryPreferred,
    secondaryPreferred, or nearest.
  * `ssl`: false [default], true to enable ssl. See
    http://mongoc.org/libmongoc/current/mongoc_ssl_opt_t.html to
    understand the options.
  * `pem_file`: The .pem file that contains both the TLS/SSL certificate and
    key.
  * `pem_pwd`: The password to decrypt the certificate key file(i.e. pem_file)
  * `ca_file`: The .pem file that contains the root certificate chain from the
    Certificate Authority.
  * `ca_dir`: The absolute path to the `ca_file`.
  * `crl_file`: The .pem file that contains the Certificate Revocation List.
  * `weak_cert_validation`: false [default], This is to enable or disable the
    validation checks for TLS/SSL certificates and allows the use of invalid
	certificates to connect if set to `true`.
  * `enable_join_pushdown`: If `true`, pushes the join between two foreign
	tables from the same foreign server, instead of fetching all the rows
	for both the tables and performing a join locally. This option can also
	be set for an individual table, and if any of the tables involved in the
	join has set it to false then the join will not be pushed down. The
	table-level value of the option takes precedence over the server-level
	option value. Default is `true`.
  * `enable_aggregate_pushdown`: If `true`, push aggregates to the remote
	MongoDB server instead of fetching all of the rows and aggregating them
	locally. This option can also be set for an individual table. The
	table-level value of the option takes precedence over the server-level
	option value. Default is `true`.

The following parameters can be set on a MongoDB foreign table object:

  * `database`: Name of the MongoDB database to query. Defaults to
    `test`.
  * `collection`: Name of the MongoDB collection to query. Defaults to
    the foreign table name used in the relevant `CREATE` command.
  * `enable_join_pushdown`: Similar to the server-level option, but can be
    configured at table level as well. Default is `true`.
  * `enable_aggregate_pushdown`: Similar to the server-level option, but
    can be configured at table level as well. Default is `true`.

The following parameters can be supplied while creating user mapping:

  * `username`: Username to use when connecting to MongoDB.
  * `password`: Password to authenticate to the MongoDB server.

GUC variables:

  * `mongo_fdw.enable_order_by_pushdown`: If `true`, pushes the order by
	operation to the foreign server, instead of fetching rows from the
	foreign server and performing the sort locally. Default is `true`.

As an example, the following commands demonstrate loading the
`mongo_fdw` wrapper, creating a server, and then creating a foreign
table associated with a MongoDB collection. The commands also show
specifying option values in the `OPTIONS` clause. If an option value
isn't provided, the wrapper uses the default value mentioned above.

`mongo_fdw` can collect data distribution statistics will incorporate
them when estimating costs for the query execution plan. To see selected
execution plans for a query, just run `EXPLAIN`.

Examples
--------

Examples with [MongoDB][1]'s equivalent statements.

```sql
-- load extension first time after install
CREATE EXTENSION mongo_fdw;

-- create server object
CREATE SERVER mongo_server
	FOREIGN DATA WRAPPER mongo_fdw
	OPTIONS (address '127.0.0.1', port '27017');

-- create user mapping
CREATE USER MAPPING FOR postgres
	SERVER mongo_server
	OPTIONS (username 'mongo_user', password 'mongo_pass');

-- create foreign table
CREATE FOREIGN TABLE warehouse
	(
		_id name,
		warehouse_id int,
		warehouse_name text,
		warehouse_created timestamptz
	)
	SERVER mongo_server
	OPTIONS (database 'db', collection 'warehouse');

-- Note: first column of the table must be "_id" of type "name".

-- select from table
SELECT * FROM warehouse WHERE warehouse_id = 1;
           _id            | warehouse_id | warehouse_name |     warehouse_created
--------------------------+--------------+----------------+---------------------------
 53720b1904864dc1f5a571a0 |            1 | UPS            | 2014-12-12 12:42:10+05:30
(1 row)

db.warehouse.find
(
	{
		"warehouse_id" : 1
	}
).pretty()
{
	"_id" : ObjectId("53720b1904864dc1f5a571a0"),
	"warehouse_id" : 1,
	"warehouse_name" : "UPS",
	"warehouse_created" : ISODate("2014-12-12T07:12:10Z")
}

-- insert row in table
INSERT INTO warehouse VALUES (0, 2, 'Laptop', '2015-11-11T08:13:10Z');

-- Note: The given value for "_id" column will be ignored and allows MongoDB to
-- insert the unique value for the "_id" column.

db.warehouse.insert
(
	{
		"warehouse_id" : NumberInt(2),
		"warehouse_name" : "Laptop",
		"warehouse_created" : ISODate("2015-11-11T08:13:10Z")
	}
)

-- delete row from table
DELETE FROM warehouse WHERE warehouse_id = 2;

db.warehouse.remove
(
	{
		"warehouse_id" : 2
	}
)

-- update a row of table
UPDATE warehouse SET warehouse_name = 'UPS_NEW' WHERE warehouse_id = 1;

db.warehouse.update
(
	{
		"warehouse_id" : 1
	},
	{
		"warehouse_id" : 1,
		"warehouse_name" : "UPS_NEW",
		"warehouse_created" : ISODate("2014-12-12T07:12:10Z")
	}
)

-- explain a table
EXPLAIN SELECT * FROM warehouse WHERE warehouse_id = 1;
                           QUERY PLAN
-----------------------------------------------------------------
 Foreign Scan on warehouse  (cost=0.00..0.00 rows=1000 width=84)
   Filter: (warehouse_id = 1)
   Foreign Namespace: db.warehouse
(3 rows)

-- collect data distribution statistics
ANALYZE warehouse;

```

Limitations
-----------

  * If the BSON document key contains uppercase letters or occurs within
    a nested document, `mongo_fdw` requires the corresponding column names
    to be declared in double quotes.

  * Note that PostgreSQL limits column names to 63 characters by
    default. If you need column names that are longer, you can increase the
    `NAMEDATALEN` constant in `src/include/pg_config_manual.h`, compile,
    and re-install.


Contributing
------------
Have a fix for a bug or an idea for a great new feature? Great! Check
out the contribution guidelines [here][3].


Support
-------
This project will be modified to maintain compatibility with new
PostgreSQL and EDB Postgres Advanced Server releases.

If you need commercial support, please contact the EnterpriseDB sales
team, or check whether your existing PostgreSQL support provider can
also support mongo_fdw.


License
-------
Portions Copyright (c) 2004-2022, EnterpriseDB Corporation.
Portions Copyright © 2012–2014 Citus Data, Inc.

This program is free software: you can redistribute it and/or modify it
under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

See the [`LICENSE`][4] file for full details.

[1]: http://www.mongodb.com
[2]: https://github.com/enterprisedb/mongo_fdw/issues/new
[3]: CONTRIBUTING.md
[4]: LICENSE
[5]: http://mongoc.org/libmongoc/1.17.3/installing.html#configuring-the-build
[6]: https://github.com/json-c/json-c/tree/json-c-0.15-20200726#build-instructions--
