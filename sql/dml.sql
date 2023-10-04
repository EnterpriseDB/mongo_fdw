\set MONGO_HOST			`echo \'"$MONGO_HOST"\'`
\set MONGO_PORT			`echo \'"$MONGO_PORT"\'`
\set MONGO_USER_NAME	`echo \'"$MONGO_USER_NAME"\'`
\set MONGO_PASS			`echo \'"$MONGO_PWD"\'`

-- Before running this file user must create database mongo_fdw_regress,
-- mongo_fdw_regress1 and mongo_fdw_regress2 databases on MongoDB with all
-- permission for MONGO_USER_NAME user with MONGO_PASS password and ran
-- mongodb_init.sh file to load collections.

\c contrib_regression
CREATE EXTENSION IF NOT EXISTS mongo_fdw;
CREATE SERVER mongo_server FOREIGN DATA WRAPPER mongo_fdw
  OPTIONS (address :MONGO_HOST, port :MONGO_PORT);
CREATE USER MAPPING FOR public SERVER mongo_server;

-- Create foreign tables
CREATE FOREIGN TABLE f_mongo_test (_id name, a int, b varchar) SERVER mongo_server
  OPTIONS (database 'mongo_fdw_regress', collection 'mongo_test');
CREATE FOREIGN TABLE f_mongo_test1 (_id name, a int, b varchar) SERVER mongo_server
  OPTIONS (database 'mongo_fdw_regress1', collection 'mongo_test1');
CREATE FOREIGN TABLE f_mongo_test2 (_id name, a int, b varchar) SERVER mongo_server
  OPTIONS (database 'mongo_fdw_regress2', collection 'mongo_test2');
-- Creating foreign table without specifying database.
CREATE FOREIGN TABLE f_mongo_test3 (_id name, a int, b varchar) SERVER mongo_server
  OPTIONS (collection 'mongo_test3');
CREATE FOREIGN TABLE f_mongo_test6 (_id name, a int, b text[]) SERVER mongo_server
  OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl6');

-- Verify the INSERT/UPDATE/DELETE operations on a collection (mongo_test)
-- exist in a database (mongo_fdw_regress) in mongoDB.
SELECT a,b FROM f_mongo_test ORDER BY 1, 2;
INSERT INTO f_mongo_test VALUES ('0', 10 , 'INSERT');
SELECT a,b FROM f_mongo_test ORDER BY 1, 2;
UPDATE f_mongo_test SET b = 'UPDATE' WHERE a = 10;
SELECT a,b FROM f_mongo_test ORDER BY 1, 2;
DELETE FROM f_mongo_test WHERE a = 10;
SELECT a,b FROM f_mongo_test ORDER BY 1, 2;

-- Verify the INSERT/UPDATE/DELETE operations on a collection (mongo_test1)
-- not exist in a database (mongo_fdw_regress1) in mongoDB.
SELECT a,b FROM f_mongo_test1 ORDER BY 1, 2;
INSERT INTO f_mongo_test1 VALUES ('0', 10 , 'INSERT');
SELECT a,b FROM f_mongo_test1 ORDER BY 1, 2;
UPDATE f_mongo_test1 SET b = 'UPDATE' WHERE a = 10;
SELECT a,b FROM f_mongo_test1 ORDER BY 1, 2;
DELETE FROM f_mongo_test1 WHERE a = 10;
SELECT a,b FROM f_mongo_test1 ORDER BY 1, 2;

-- Verify the INSERT/UPDATE/DELETE operations on a collection (mongo_test2)
-- not exist in a non exist database (mongo_fdw_regress2) in mongoDB.
SELECT a,b FROM f_mongo_test2 ORDER BY 1, 2;
INSERT INTO f_mongo_test2 VALUES ('0', 10 , 'INSERT');
SELECT a,b FROM f_mongo_test2 ORDER BY 1, 2;
UPDATE f_mongo_test2 SET b = 'UPDATE' WHERE a = 10;
SELECT a,b FROM f_mongo_test2 ORDER BY 1, 2;
DELETE FROM f_mongo_test2 WHERE a = 10;
SELECT a,b FROM f_mongo_test2 ORDER BY 1, 2;

-- Verify the INSERT/UPDATE/DELETE operations on a collection (mongo_test)
-- when foreign table created without database option.
SELECT a,b FROM f_mongo_test3 ORDER BY 1, 2;
INSERT INTO f_mongo_test3 VALUES ('0', 10 , 'INSERT');
SELECT a,b FROM f_mongo_test3 ORDER BY 1, 2;
UPDATE f_mongo_test3 SET b = 'UPDATE' WHERE a = 10;
SELECT a,b FROM f_mongo_test3 ORDER BY 1, 2;
DELETE FROM f_mongo_test3 WHERE a = 10;
SELECT a,b FROM f_mongo_test3 ORDER BY 1, 2;

-- FDW-158: Fix server crash when analyzing a foreign table.
ANALYZE f_mongo_test;
-- Should give correct number of rows now.
SELECT reltuples FROM pg_class WHERE relname = 'f_mongo_test';
-- Check count using select query on table.
SELECT count(*) FROM f_mongo_test;

-- Some more variants of vacuum and analyze
VACUUM f_mongo_test;
VACUUM FULL f_mongo_test;
VACUUM FREEZE f_mongo_test;
ANALYZE f_mongo_test;
ANALYZE f_mongo_test(a);
VACUUM ANALYZE f_mongo_test;

-- FDW-226: Fix COPY FROM and foreign partition routing results in a
-- server crash

-- Should fail as foreign table direct copy is not supported
COPY f_mongo_test TO '/tmp/data.txt' delimiter ',';
COPY f_mongo_test (a) TO '/tmp/data.txt' delimiter ',';
COPY f_mongo_test (b) TO '/tmp/data.txt' delimiter ',';

-- Should pass
COPY (SELECT * FROM f_mongo_test) TO '/tmp/data.txt' delimiter ',';
COPY (SELECT a, b FROM f_mongo_test) TO '/tmp/data.txt' delimiter ',';
COPY (SELECT a FROM f_mongo_test) TO '/tmp/data.txt' delimiter ',';
COPY (SELECT b FROM f_mongo_test) TO '/tmp/data.txt' delimiter ',';

-- Should throw an error as copy to foreign table is not supported
DO
$$
BEGIN
  COPY f_mongo_test FROM '/tmp/data.txt' delimiter ',';
EXCEPTION WHEN others THEN
  IF SQLERRM = 'COPY and foreign partition routing not supported in mongo_fdw' OR
     SQLERRM = 'cannot copy to foreign table "f_mongo_test"' THEN
    RAISE NOTICE 'ERROR:  COPY and foreign partition routing not supported in mongo_fdw';
  ELSE
    RAISE NOTICE '%', SQLERRM;
  END IF;
END;
$$
LANGUAGE plpgsql;

DO
$$
BEGIN
  COPY f_mongo_test(a, b) FROM '/tmp/data.txt' delimiter ',';
EXCEPTION WHEN others THEN
  IF SQLERRM = 'COPY and foreign partition routing not supported in mongo_fdw' OR
     SQLERRM = 'cannot copy to foreign table "f_mongo_test"' THEN
    RAISE NOTICE 'ERROR:  COPY and foreign partition routing not supported in mongo_fdw';
  ELSE
    RAISE NOTICE '%', SQLERRM;
  END IF;
END;
$$
LANGUAGE plpgsql;

DO
$$
BEGIN
  COPY f_mongo_test(a) FROM '/tmp/data.txt' delimiter ',';
EXCEPTION WHEN others THEN
  IF SQLERRM = 'COPY and foreign partition routing not supported in mongo_fdw' OR
     SQLERRM = 'cannot copy to foreign table "f_mongo_test"' THEN
    RAISE NOTICE 'ERROR:  COPY and foreign partition routing not supported in mongo_fdw';
  ELSE
    RAISE NOTICE '%', SQLERRM;
  END IF;
END;
$$
LANGUAGE plpgsql;

DO
$$
BEGIN
  COPY f_mongo_test(b) FROM '/tmp/data.txt' delimiter ',';
EXCEPTION WHEN others THEN
  IF SQLERRM = 'COPY and foreign partition routing not supported in mongo_fdw' OR
     SQLERRM = 'cannot copy to foreign table "f_mongo_test"' THEN
    RAISE NOTICE 'ERROR:  COPY and foreign partition routing not supported in mongo_fdw';
  ELSE
    RAISE NOTICE '%', SQLERRM;
  END IF;
END;
$$
LANGUAGE plpgsql;

--FDW-466: Document update for array elements shouldn't lead to the crash
INSERT INTO f_mongo_test6 VALUES (0, 1, ARRAY ['INSERT', 'DELETE']);
SELECT a, b FROM f_mongo_test6 ORDER BY a;
UPDATE f_mongo_test6 SET b[1] = 'UPDATE' WHERE a = 1;
SELECT a, b FROM f_mongo_test6 ORDER BY a;
DELETE FROM f_mongo_test6 WHERE b[2] = 'DELETE';
SELECT a, b FROM f_mongo_test6 ORDER BY a;

--FDW-481: UPDATE/DELETE shouldn't lead to crash when _id is NULL.
-- If first column type is not NAME then UPDATE/DELETE should result into an error.
CREATE FOREIGN TABLE f_mongo_test7 (_id text, a int, b text) SERVER mongo_server
  OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl7');
SELECT a, b FROM f_mongo_test7 ORDER BY 1;
UPDATE f_mongo_test7 SET b = 'UPDATED' WHERE a = 10;
DELETE FROM f_mongo_test7 WHERE a = 10;
DROP FOREIGN TABLE f_mongo_test7;

-- If first column name is not _id then UPDATE/DELETE should result into an error.
CREATE FOREIGN TABLE f_mongo_test7 (id1 NAME, a int, b text) SERVER mongo_server
  OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl7');
SELECT a, b FROM f_mongo_test7 ORDER BY 1;
UPDATE f_mongo_test7 SET b = 'UPDATED' WHERE a = 10;
DELETE FROM f_mongo_test7 WHERE a = 10;
DROP FOREIGN TABLE f_mongo_test7;

-- UPDATE/DELETE when _id is NULL. Shouldn't crash.
CREATE FOREIGN TABLE f_mongo_test7 (_id NAME, a int, b text) SERVER mongo_server
  OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl7');
SELECT a, b FROM f_mongo_test7 ORDER BY 1;
SELECT * FROM f_mongo_test7 WHERE a = 10 ORDER BY 1;
UPDATE f_mongo_test7 SET b = 'UPDATED' WHERE _id IS NULL;
SELECT a, b FROM f_mongo_test7 ORDER BY 1;
DELETE FROM f_mongo_test7 WHERE a = 20;
SELECT a, b FROM f_mongo_test7 ORDER BY 1;

-- Retain original data of test_tbl7
UPDATE f_mongo_test7 SET b = 'ROW1' WHERE a = 10;
INSERT INTO f_mongo_test7 VALUES(0, 20, 'ROW2');

-- When _id is non-objectId type on MongoDB. Should result into an error.
CREATE FOREIGN TABLE f_mongo_test8 (_id NAME, a int, b text) SERVER mongo_server
  OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl8');
SELECT * FROM f_mongo_test8 ORDER BY 1;
UPDATE f_mongo_test8 SET b = 'UPDATED' WHERE a = 2;
DELETE FROM f_mongo_test8 WHERE a = 2;
SELECT a, b FROM f_mongo_test8 ORDER BY 1;

-- Cleanup
DROP FOREIGN TABLE f_mongo_test;
DROP FOREIGN TABLE f_mongo_test1;
DROP FOREIGN TABLE f_mongo_test2;
DROP FOREIGN TABLE f_mongo_test3;
DROP FOREIGN TABLE f_mongo_test6;
DROP FOREIGN TABLE f_mongo_test7;
DROP FOREIGN TABLE f_mongo_test8;
DROP USER MAPPING FOR public SERVER mongo_server;
DROP SERVER mongo_server;
DROP EXTENSION mongo_fdw;
