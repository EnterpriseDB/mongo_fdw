#!/bin/sh

psql <<EOF
CREATE EXTENSION mongo_fdw;
CREATE SERVER mongo_server FOREIGN DATA WRAPPER mongo_fdw OPTIONS (address '127.0.0.1', port '27017');
CREATE USER MAPPING FOR postgres SERVER mongo_server OPTIONS (username 'test', password 'connecttest');
CREATE FOREIGN TABLE pgtesttable (_id NAME, question TEXT, answer TEXT) SERVER mongo_server OPTIONS (database 'pgtest', collection 'pgtesttable');
EOF

psql <<EOF
SELECT * FROM pgtesttable;
EOF
