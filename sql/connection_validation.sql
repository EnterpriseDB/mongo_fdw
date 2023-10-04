\set VERBOSITY terse
\set MONGO_HOST			`echo \'"$MONGO_HOST"\'`
\set MONGO_PORT			`echo \'"$MONGO_PORT"\'`
\set MONGO_USER_NAME	`echo \'"$MONGO_USER_NAME"\'`
\set MONGO_PASS			`echo \'"$MONGO_PWD"\'`

-- Before running this file user must create database mongo_fdw_regress on
-- MongoDB with all permission for MONGO_USER_NAME user with MONGO_PASS
-- password and ran mongodb_init.sh file to load collections.

\c contrib_regression
CREATE EXTENSION IF NOT EXISTS mongo_fdw;
CREATE SERVER mongo_server FOREIGN DATA WRAPPER mongo_fdw
  OPTIONS (address :MONGO_HOST, port :MONGO_PORT);
CREATE USER MAPPING FOR public SERVER mongo_server;

-- Create foreign tables and validate
CREATE FOREIGN TABLE f_mongo_test (_id name, a int, b varchar)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'mongo_test');
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;

--
-- fdw-108: After a change to a pg_foreign_server or pg_user_mapping catalog
-- entry, connection should be invalidated.
--

-- Alter one of the SERVER option
-- Set wrong address for mongo_server
ALTER SERVER mongo_server OPTIONS (SET address '127.0.0.10');
ALTER SERVER mongo_server OPTIONS (SET port '9999');
-- Should fail with an error
INSERT INTO f_mongo_test VALUES ('0', 2, 'RECORD INSERTED');
UPDATE f_mongo_test SET b = 'RECORD UPDATED' WHERE a = 2;
DELETE FROM f_mongo_test WHERE a = 2;
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;
-- Set correct address for mongo_server
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);
ALTER SERVER mongo_server OPTIONS (SET port :MONGO_PORT);
-- Should able to insert the data
INSERT INTO f_mongo_test VALUES ('0', 2, 'RECORD INSERTED');
DELETE FROM f_mongo_test WHERE a = 2;

-- Drop user mapping and create with invalid username and password for public
-- user mapping
DROP USER MAPPING FOR public SERVER mongo_server;
CREATE USER MAPPING FOR public SERVER mongo_server
  OPTIONS (username 'wrong', password 'wrong');
-- Should fail with an error
INSERT INTO f_mongo_test VALUES ('0', 3, 'RECORD INSERTED');
UPDATE f_mongo_test SET b = 'RECORD UPDATED' WHERE a = 3;
DELETE FROM f_mongo_test WHERE a = 3;
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;
-- Drop user mapping and create without username and password for public
-- user mapping
DROP USER MAPPING FOR public SERVER mongo_server;
CREATE USER MAPPING FOR public SERVER mongo_server;
-- Should able to insert the data
INSERT INTO f_mongo_test VALUES ('0', 3, 'RECORD INSERTED');
DELETE FROM f_mongo_test WHERE a = 3;

-- Cleanup
DROP FOREIGN TABLE f_mongo_test;
DROP USER MAPPING FOR public SERVER mongo_server;
DROP SERVER mongo_server;
DROP EXTENSION mongo_fdw;
