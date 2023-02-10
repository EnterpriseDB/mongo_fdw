MongoDB Foreign Data Wrapper for PostgreSQL
============================================

This is a foreign data wrapper (FDW) to connect [PostgreSQL](https://www.postgresql.org/)
to [MongoDB](https://www.mongodb.com/). This version of `mongo_fdw` works with PostgreSQL and EDB
Postgres Advanced Server 10, 11, 12, 13, 14, and 15.

Contents
--------

1. [Features](#features)
2. [Supported platforms](#supported-platforms)
3. [Installation](#installation)
4. [Usage](#usage)
5. [Functions](#functions)
6. [Identifier case handling](#identifier-case-handling)
7. [Generated columns](#generated-columns)
8. [Character set handling](#character-set-handling)
9. [Examples](#examples)
10. [Limitations](#limitations)
11. [TAP tests](#tap-tests)
12. [Development roadmap](#development-roadmap)
13. [Useful links](#useful-links)

Features
--------

The following enhancements are added to the latest version of `mongo_fdw`:

#### Write-able FDW
The previous version was only read-only, the latest version provides the
write capability. The user can now issue an insert, update, and delete
statements for the foreign tables using the `mongo_fdw`.

#### Connection Pooling
The latest version comes with a connection pooler that utilizes the
same MongoDB database connection for all the queries in the same session.
The previous version would open a new [MongoDB][1] connection for every
query. This is a performance enhancement.

#### JOIN push-down
`mongo_fdw` now also supports join push-down. The joins between two
foreign tables from the same remote MySQL server are pushed to a remote
server, instead of fetching all the rows for both the tables and
performing a join locally, thereby may enhance the performance. Currently,
joins involving only relational and arithmetic operators in join-clauses
are pushed down to avoid any potential join failure. Also, only the
INNER and LEFT/RIGHT OUTER joins are supported, and not the FULL OUTER,
SEMI, and ANTI join. Moreover, only joins between two tables are pushed
down and not when either inner or outer relation is the join itself.

#### AGGREGATE push-down
`mongo_fdw` now also supports aggregate push-down. Push aggregates to the
remote MongoDB server instead of fetching all of the rows and aggregating
them locally. This gives a very good performance boost for the cases
where aggregates can be pushed down. The push-down is currently limited
to aggregate functions min, max, sum, avg, and count, to avoid pushing
down the functions that are not present on the MongoDB server. The
aggregate filters, orders, variadic and distinct are not pushed down.

#### ORDER BY push-down
`mongo_fdw` now also supports order by push-down. If possible, push order
by clause to the remote server so that we get the ordered result set from
the foreign server itself. It might help us to have an efficient merge
join. NULLs behavior is opposite on the MongoDB server. Thus to get an
equivalent result, we can only push-down ORDER BY with either
ASC NULLS FIRST or DESC NULLS LAST. Moreover, as MongoDB sorts only on
fields, only column names in ORDER BY expressions are pushed down.

#### LIMIT OFFSET push-down
`mongo_fdw` now also supports limit offset push-down. Wherever possible,
perform LIMIT and OFFSET operations on the remote server. This reduces
network traffic between local PostgreSQL and remote MongoDB servers.

#### GUC variables:

  * `mongo_fdw.enable_order_by_pushdown`: If `true`, pushes the order by
	operation to the foreign server, instead of fetching rows from the
	foreign server and performing the sort locally. Default is `true`.


Supported platforms
-------------------

`mongo_fdw` was developed on Linux, and should run on any
reasonably POSIX-compliant system.

Installation
------------

About script or manual installation, `mongo-c`, legacy and `meta` driver please read the following [instructions in INSTALL.md](INSTALL.md).

If you run into any issues, please [let us know][2].

Usage
-----

## CREATE SERVER options

`mongo_fdw` accepts the following options via the `CREATE SERVER` command:

- **address** as *string*, optional

  Address or hostname of the MongoDB server. Defaults to `127.0.0.1`.

- **port** as *integer*, optioanl

  Port number of the MongoDB server. Defaults to `27017`.
  
- **use_remote_estimate** as *boolean*, optioanl

  Controls whether `mongo_fdw` uses exact rows from
    remote collection to obtain cost estimates. Default is `false`.

The following options are _only supported with meta driver_:

- **authentication_database** as *string*, optional

  Database against which user will be
    authenticated against. Only valid with password based authentication.

- **replica_set** as *string*, optional

  Replica set the server is member of. If set,
    driver will auto-connect to correct primary in the replica set when
    writing.

- **read_preference** as *string*, optional

  `primary` (default), `secondary`, `primaryPreferred`,
    `secondaryPreferred`, or `nearest`.

- **ssl** as *boolean*, optional

  `false` is default, `true` to enable ssl. See
    http://mongoc.org/libmongoc/current/mongoc_ssl_opt_t.html to
    understand the options.

- **pem_file** as *string*, optional

  The .pem file that contains both the TLS/SSL certificate and
    key.

- **pem_pwd** as *string*, optional

  The password to decrypt the certificate key file(i.e. pem_file)

- **ca_file** as *string*, optional

  The .pem file that contains the root certificate chain from the
    Certificate Authority.

- **ca_dir** as *string*, optional

  The absolute path to the `ca_file`.

- **crl_file** as *string*, optional

  The .pem file that contains the Certificate Revocation List.

- **weak_cert_validation** as *boolean*, optional

  `false` is default, This is to enable or disable the
    validation checks for TLS/SSL certificates and allows the use of invalid
	certificates to connect if set to `true`.

- **enable_join_pushdown** as *boolean*, optional

  If `true`, pushes the join between two foreign
	tables from the same foreign server, instead of fetching all the rows
	for both the tables and performing a join locally. This option can also
	be set for an individual table, and if any of the tables involved in the
	join has set it to false then the join will not be pushed down. The
	table-level value of the option takes precedence over the server-level
	option value. Default is `true`.

- **enable_aggregate_pushdown** as *boolean*, optional

  If `true`, push aggregates to the remote
	MongoDB server instead of fetching all of the rows and aggregating them
	locally. This option can also be set for an individual table. The
	table-level value of the option takes precedence over the server-level
	option value. Default is `true`.

## CREATE USER MAPPING options

`mongo_fdw` accepts the following options via the `CREATE USER MAPPING`
command:

- **username**

  Username to use when connecting to MongoDB.

- **password**

  Password to authenticate to the MongoDB server.
  
## CREATE FOREIGN TABLE options

`mongo_fdw` accepts the following table-level options via the
`CREATE FOREIGN TABLE` command:

- **database** as *string*, optional.

  Name of the MongoDB database to query. Defaults to
    `test`.

- **collection** as *string*, optional.

  Name of the MongoDB collection to query. Defaults to
    the foreign table name used in the relevant `CREATE` command.

- **enable_join_pushdown** as *boolean*, optional

  Similar to the server-level option, but can be
    configured at table level as well. Default is `true`.

- **enable_aggregate_pushdown** as *boolean*, optional

  Similar to the server-level option, but
    can be configured at table level as well. Default is `true`.
    
No column-level options are available.

## IMPORT FOREIGN SCHEMA options

`mongo_fdw` don't supports [IMPORT FOREIGN SCHEMA](https://www.postgresql.org/docs/current/sql-importforeignschema.html)
because MongoDB is schemaless.

## TRUNCATE support

`mongo_fdw` don't implements the foreign data wrapper `TRUNCATE` API, available
from PostgreSQL 14, because MongoDB is schemaless.

Functions
---------

As well as the standard `mongo_fdw_handler()` and `mongo_fdw_validator()`
functions, `mongo_fdw` provides the following user-callable utility functions:

**Yet not described!**.

Identifier case handling
------------------------

PostgreSQL folds identifiers to lower case by default, MongoDB use JSON notation of identifiers.

All transformation rules and problems **yet not described**.

Generated columns
-----------------

`mongo_fdw` does't provides support for PostgreSQL's generated
columns (PostgreSQL 12+).

**Behaviour with generated columns yet not tested and not described**.

Note that while `mongo_fdw` will insert or update the generated column value
in MongoDB, there is nothing to stop the value being modified within MongoDB,
and hence no guarantee that in subsequent `SELECT` operations the column will
still contain the expected generated value. This limitation also applies to
`postgres_fdw`.

For more details on generated columns see:

- [Generated Columns](https://www.postgresql.org/docs/current/ddl-generated-columns.html)
- [CREATE FOREIGN TABLE](https://www.postgresql.org/docs/current/sql-createforeigntable.html)

Character set handling
----------------------

`BSON` in MongoDB can only be encoded in `UTF-8`. Also `UTF-8` is recommended and
de-facto most popular PostgreSQL server encoding.

Encodings mapping between PostgreSQL and MongoDB **yet not described**.

Examples
--------

As an example, the following commands demonstrate loading the
`mongo_fdw` wrapper, creating a server, and then creating a foreign
table associated with a MongoDB collection. The commands also show
specifying option values in the `OPTIONS` clause. If an option value
isn't provided, the wrapper uses the default value mentioned above.

`mongo_fdw` can collect data distribution statistics will incorporate
them when estimating costs for the query execution plan. To see selected
execution plans for a query, just run `EXPLAIN`.

Install the extension (superuser only):

    CREATE EXTENSION mongo_fdw;

Create a foreign server with appropriate configuration (superuser only):

    CREATE SERVER "MongoDB server"
      FOREIGN DATA WRAPPER mongo_fdw
      OPTIONS (
        address '127.0.0.1',
        port '27017'
     );

Create an appropriate user mapping:

    CREATE USER MAPPING FOR CURRENT_USER SERVER "MongoDB server"
		OPTIONS(
			username 'mongo_user',
			password 'mongo_pass'
			);
	

Grant usage on MongoDB server to non privileged user by your choice if necessary (recommended):
	
	GRANT USAGE ON FOREIGN SERVER "MongoDB server" TO your_user;


Create a foreign table referencing the MongoDB collection:

    -- Note: first column of the table must be "_id" of type "name".
    CREATE FOREIGN TABLE warehouse
	(
		_id name,
		warehouse_id int,
		warehouse_name text,
		warehouse_created timestamptz
	)
	SERVER "MongoDB server"
	OPTIONS (
		database 'db',
		collection 'warehouse'
		);


Other examples with [MongoDB][1]'s equivalent statements.

```sql

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

  - If the BSON document key contains uppercase letters or occurs within
    a nested document, ``mongo_fdw`` requires the corresponding column names
    to be declared in double quotes.

  - Note that PostgreSQL limits column names to 63 characters by
    default. If you need column names that are longer, you can increase the
    `NAMEDATALEN` constant in `src/include/pg_config_manual.h`, compile,
    and re-install.

Contributing
------------
Have a fix for a bug or an idea for a great new feature? Great! Check
out the contribution guidelines [here][3].

Useful links
------------

### Source code

Reference FDW realisation, `postgres_fdw`
 - https://git.postgresql.org/gitweb/?p=postgresql.git;a=tree;f=contrib/postgres_fdw;hb=HEAD

### General FDW Documentation

 - https://www.postgresql.org/docs/current/ddl-foreign-data.html
 - https://www.postgresql.org/docs/current/sql-createforeigndatawrapper.html
 - https://www.postgresql.org/docs/current/sql-createforeigntable.html
 - https://www.postgresql.org/docs/current/sql-importforeignschema.html
 - https://www.postgresql.org/docs/current/fdwhandler.html
 - https://www.postgresql.org/docs/current/postgres-fdw.html

### Other FDWs

 - https://wiki.postgresql.org/wiki/Fdw
 - https://pgxn.org/tag/fdw/

Support
-------
This project will be modified to maintain compatibility with new
PostgreSQL and EDB Postgres Advanced Server releases.

If you need commercial support, please contact the EnterpriseDB sales
team, or check whether your existing PostgreSQL support provider can
also support `mongo_fdw`.


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
