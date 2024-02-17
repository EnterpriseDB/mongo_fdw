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

-- Create foreign tables
CREATE FOREIGN TABLE f_mongo_test (_id name, a int, b varchar)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'mongo_test');
CREATE FOREIGN TABLE f_test_tbl1 (_id name, c1 INTEGER, c2 VARCHAR(10), c3 CHAR(9), c4 INTEGER, c5 pg_catalog.Date, c6 DECIMAL, c7 INTEGER, c8 INTEGER)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl1');
CREATE FOREIGN TABLE f_test_tbl2 (_id name, c1 INTEGER, c2 VARCHAR(14), c3 VARCHAR(13))
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl2');
CREATE FOREIGN TABLE f_test_tbl3 (_id name, name TEXT, marks FLOAT ARRAY, pass BOOLEAN)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl3');

-- Inserts some values in mongo_test collection.
INSERT INTO f_mongo_test VALUES ('0', 1, 'One');
INSERT INTO f_mongo_test VALUES ('0', 2, 'Two');
INSERT INTO f_mongo_test VALUES ('0', 3, 'Three');

SET datestyle TO ISO;

-- Sample data
SELECT c1, c2, c3, c4, c5, c6, c7, c8 FROM f_test_tbl1 ORDER BY c1;

-- WHERE clause pushdown
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6 AS "salary", c8 FROM f_test_tbl1 e
  WHERE c6 IN (1600, 2450)
  ORDER BY c1 ASC NULLS FIRST;
SELECT c1, c2, c6 AS "salary", c8 FROM f_test_tbl1 e
  WHERE c6 IN (1600, 2450)
  ORDER BY c1;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6 FROM f_test_tbl1 e
  WHERE c6 > 3000
  ORDER BY c1 ASC NULLS FIRST;
SELECT c1, c2, c6 FROM f_test_tbl1 e
  WHERE c6 > 3000
  ORDER BY c1 ASC NULLS FIRST;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c6 = 1500
  ORDER BY c1 DESC NULLS LAST;
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c6 = 1500
  ORDER BY c1 DESC NULLS LAST;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c6 BETWEEN 1000 AND 4000
  ORDER BY c1 ASC NULLS FIRST;
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c6 BETWEEN 1000 AND 4000
  ORDER BY c1 ASC NULLS FIRST;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c4, c6, c8 FROM f_test_tbl1 e
  WHERE c4 IS NOT NULL
  ORDER BY c1;
SELECT c1, c2, c4, c6, c8 FROM f_test_tbl1 e
  WHERE c4 IS NOT NULL
  ORDER BY c1;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c5 FROM f_test_tbl1 e
  WHERE c5 <= '1980-12-17'
  ORDER BY c1 ASC NULLS FIRST;
SELECT c1, c2, c5 FROM f_test_tbl1 e
  WHERE c5 <= '1980-12-17'
  ORDER BY c1 ASC NULLS FIRST;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c2 IN ('EMP6', 'EMP12', 'EMP5')
  ORDER BY c1;
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c2 IN ('EMP6', 'EMP12', 'EMP5')
  ORDER BY c1;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c3 LIKE 'SALESMAN'
  ORDER BY c1;
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c3 LIKE 'SALESMAN'
  ORDER BY c1;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c3 LIKE 'MANA%'
  ORDER BY c1;
SELECT c1, c2, c6, c8 FROM f_test_tbl1 e
  WHERE c3 LIKE 'MANA%'
  ORDER BY c1;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT a FROM f_mongo_test
  WHERE a%2 = 1
  ORDER BY a ASC NULLS FIRST;
SELECT a FROM f_mongo_test
  WHERE a%2 = 1
  ORDER BY a ASC NULLS FIRST;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT a, b FROM f_mongo_test
  WHERE a >= 1 AND b LIKE '%O%'
  ORDER BY a;
SELECT a, b FROM f_mongo_test
  WHERE a >= 1 AND b LIKE '%O%'
  ORDER BY a;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2, c5 FROM f_test_tbl1 e
  WHERE c5 <= '1980-12-17' AND c2 IN ('EMP1', 'EMP5', 'EMP10') AND c1 = 100
  ORDER BY c1;
SELECT c1, c2, c5 FROM f_test_tbl1 e
  WHERE c5 <= '1980-12-17' AND c2 IN ('EMP1', 'EMP5', 'EMP10') AND c1 = 100
  ORDER BY c1;

-- The ORDER BY clause shouldn't push-down due to explicit COLLATE.
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2 FROM f_test_tbl1
  WHERE c2 = 'EMP10'
  ORDER BY c2 COLLATE "en_US" DESC NULLS LAST;
SELECT c1, c2 FROM f_test_tbl1
  WHERE c2 = 'EMP10'
  ORDER BY c2 COLLATE "en_US" DESC NULLS LAST;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2 FROM f_test_tbl1
  WHERE c2 < 'EMP10'
  ORDER BY c2 DESC NULLS LAST;
SELECT c1, c2 FROM f_test_tbl1
  WHERE c2 < 'EMP10'
  ORDER BY c2 DESC NULLS LAST;

-- Should push down if two columns of same table are
-- involved in single WHERE clause operator expression.
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c4 FROM f_test_tbl1
  WHERE c1 > c4
  ORDER BY c1 ASC NULLS FIRST;
SELECT c1, c4 FROM f_test_tbl1
  WHERE c1 > c4
  ORDER BY c1 ASC NULLS FIRST;

EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c4, c7, c8 FROM f_test_tbl1
  WHERE c1 < c4 AND c7 < c8
  ORDER BY c1;
SELECT c1, c4, c7, c8 FROM f_test_tbl1
  WHERE c1 < c4 AND c7 < c8
  ORDER BY c1;

-- With ORDER BY pushdown disabled.
SET mongo_fdw.enable_order_by_pushdown TO OFF;
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c4 FROM f_test_tbl1
  WHERE c1 > c4
  ORDER BY c1 ASC NULLS FIRST;
SELECT c1, c4 FROM f_test_tbl1
  WHERE c1 > c4
  ORDER BY c1 ASC NULLS FIRST;
SET mongo_fdw.enable_order_by_pushdown TO ON;

-- Nested operator expression in WHERE clause. Should pushdown.
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2 FROM f_test_tbl1
  WHERE (c1 > 1000) > FALSE
  ORDER BY c1 ASC NULLS FIRST;
SELECT c1, c2 FROM f_test_tbl1
  WHERE (c1 > 1000) > FALSE
  ORDER BY c1 ASC NULLS FIRST;
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c2 FROM f_test_tbl1
  WHERE (c1 > 1000) > 0::BOOLEAN
  ORDER BY c1 ASC NULLS FIRST;
SELECT c1, c2 FROM f_test_tbl1
  WHERE (c1 > 1000) > 0::BOOLEAN
  ORDER BY c1 ASC NULLS FIRST;

-- Shouldn't push down operators where the constant is an array.
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT name, marks FROM f_test_tbl3
  WHERE marks = ARRAY[23::FLOAT, 24::FLOAT]
  ORDER BY name;
SELECT name, marks FROM f_test_tbl3
  WHERE marks = ARRAY[23::FLOAT, 24::FLOAT]
  ORDER BY name;

-- Pushdown in prepared statement.
PREPARE pre_stmt_f_mongo_test(int) AS
  SELECT b FROM f_mongo_test WHERE a = $1 ORDER BY b;
EXPLAIN (VERBOSE, COSTS FALSE)
EXECUTE pre_stmt_f_mongo_test(1);
EXECUTE pre_stmt_f_mongo_test(1);
EXPLAIN (VERBOSE, COSTS FALSE)
EXECUTE pre_stmt_f_mongo_test(2);
EXECUTE pre_stmt_f_mongo_test(2);

-- FDW-297: Only operator expressions should be pushed down in WHERE clause.
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT name, marks FROM f_test_tbl3
  WHERE pass = true
  ORDER BY name DESC NULLS LAST;
SELECT name, marks FROM f_test_tbl3
  WHERE pass = true
  ORDER BY name DESC NULLS LAST;

-- INSERT NULL values and check behaviour.
INSERT INTO f_test_tbl2 VALUES ('0', NULL, NULL, NULL);

-- Should pushdown and shouldn't result row with NULL VALUES.
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1 FROM f_test_tbl2 WHERE c1 < 1;
SELECT c1 FROM f_test_tbl2 WHERE c1 < 1;
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1 FROM f_test_tbl2 WHERE c2 = c3;
SELECT c1 FROM f_test_tbl2 WHERE c2 = c3;

-- Test with IS NULL, shouldn't push down
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1 FROM f_test_tbl2 WHERE c2 IS NULL;
SELECT c1 FROM f_test_tbl2 WHERE c2 IS NULL;

-- FDW-134: Test with number of columns more than 32
CREATE FOREIGN TABLE f_test_large (_id int,
  a01 int, a02 int, a03 int, a04 int, a05 int, a06 int, a07 int, a08 int, a09 int, a10 int,
  a11 int, a12 int, a13 int, a14 int, a15 int, a16 int, a17 int, a18 int, a19 int, a20 int,
  a21 int, a22 int, a23 int, a24 int, a25 int, a26 int, a27 int, a28 int, a29 int, a30 int,
  a31 int, a32 int, a33 int, a34 int, a35 int)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'mongo_test_large');

-- Shouldn't pushdown ORDERBY clause due to exceeded number of path keys limit.
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT _id, a01, a31, a32, a33, a34, a35 FROM f_test_large ORDER BY
  a01 ASC NULLS FIRST, a02 ASC NULLS FIRST, a03 ASC NULLS FIRST, a04 ASC NULLS FIRST, a05 ASC NULLS FIRST,
  a06 ASC NULLS FIRST, a07 ASC NULLS FIRST, a08 ASC NULLS FIRST, a09 ASC NULLS FIRST, a10 ASC NULLS FIRST,
  a11 ASC NULLS FIRST, a12 ASC NULLS FIRST, a13 ASC NULLS FIRST, a14 ASC NULLS FIRST, a15 ASC NULLS FIRST,
  a16 ASC NULLS FIRST, a17 ASC NULLS FIRST, a18 ASC NULLS FIRST, a19 ASC NULLS FIRST, a20 ASC NULLS FIRST,
  a21 ASC NULLS FIRST, a22 ASC NULLS FIRST, a23 ASC NULLS FIRST, a24 ASC NULLS FIRST, a25 ASC NULLS FIRST,
  a26 ASC NULLS FIRST, a27 ASC NULLS FIRST, a28 ASC NULLS FIRST, a29 ASC NULLS FIRST, a30 ASC NULLS FIRST,
  a31 ASC NULLS FIRST, a32 ASC NULLS FIRST, a33 ASC NULLS FIRST, a34 DESC NULLS LAST, a35 ASC NULLS FIRST;
SELECT _id, a01, a31, a32, a33, a34, a35 FROM f_test_large ORDER BY
  a01 ASC NULLS FIRST, a02 ASC NULLS FIRST, a03 ASC NULLS FIRST, a04 ASC NULLS FIRST, a05 ASC NULLS FIRST,
  a06 ASC NULLS FIRST, a07 ASC NULLS FIRST, a08 ASC NULLS FIRST, a09 ASC NULLS FIRST, a10 ASC NULLS FIRST,
  a11 ASC NULLS FIRST, a12 ASC NULLS FIRST, a13 ASC NULLS FIRST, a14 ASC NULLS FIRST, a15 ASC NULLS FIRST,
  a16 ASC NULLS FIRST, a17 ASC NULLS FIRST, a18 ASC NULLS FIRST, a19 ASC NULLS FIRST, a20 ASC NULLS FIRST,
  a21 ASC NULLS FIRST, a22 ASC NULLS FIRST, a23 ASC NULLS FIRST, a24 ASC NULLS FIRST, a25 ASC NULLS FIRST,
  a26 ASC NULLS FIRST, a27 ASC NULLS FIRST, a28 ASC NULLS FIRST, a29 ASC NULLS FIRST, a30 ASC NULLS FIRST,
  a31 ASC NULLS FIRST, a32 ASC NULLS FIRST, a33 ASC NULLS FIRST, a34 DESC NULLS LAST, a35 ASC NULLS FIRST;

-- Should pushdown ORDERBY clause because number of path keys are in limit.
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT _id, a01, a31, a32, a33, a34, a35 FROM f_test_large ORDER BY
  a01 ASC NULLS FIRST, a02 ASC NULLS FIRST, a03 ASC NULLS FIRST, a04 ASC NULLS FIRST, a05 ASC NULLS FIRST,
  a06 ASC NULLS FIRST, a07 ASC NULLS FIRST, a08 ASC NULLS FIRST, a09 ASC NULLS FIRST, a10 ASC NULLS FIRST,
  a11 ASC NULLS FIRST, a12 ASC NULLS FIRST, a13 ASC NULLS FIRST, a14 ASC NULLS FIRST, a15 ASC NULLS FIRST,
  a16 ASC NULLS FIRST, a17 ASC NULLS FIRST, a18 ASC NULLS FIRST, a19 ASC NULLS FIRST, a20 ASC NULLS FIRST,
  a21 ASC NULLS FIRST, a22 ASC NULLS FIRST, a23 ASC NULLS FIRST, a24 ASC NULLS FIRST, a25 ASC NULLS FIRST,
  a26 ASC NULLS FIRST, a27 ASC NULLS FIRST, a28 ASC NULLS FIRST, a29 ASC NULLS FIRST, a30 ASC NULLS FIRST,
  a31 ASC NULLS FIRST, a32 ASC NULLS FIRST;
SELECT _id, a01, a31, a32, a33, a34, a35 FROM f_test_large ORDER BY
  a01 ASC NULLS FIRST, a02 ASC NULLS FIRST, a03 ASC NULLS FIRST, a04 ASC NULLS FIRST, a05 ASC NULLS FIRST,
  a06 ASC NULLS FIRST, a07 ASC NULLS FIRST, a08 ASC NULLS FIRST, a09 ASC NULLS FIRST, a10 ASC NULLS FIRST,
  a11 ASC NULLS FIRST, a12 ASC NULLS FIRST, a13 ASC NULLS FIRST, a14 ASC NULLS FIRST, a15 ASC NULLS FIRST,
  a16 ASC NULLS FIRST, a17 ASC NULLS FIRST, a18 ASC NULLS FIRST, a19 ASC NULLS FIRST, a20 ASC NULLS FIRST,
  a21 ASC NULLS FIRST, a22 ASC NULLS FIRST, a23 ASC NULLS FIRST, a24 ASC NULLS FIRST, a25 ASC NULLS FIRST,
  a26 ASC NULLS FIRST, a27 ASC NULLS FIRST, a28 ASC NULLS FIRST, a29 ASC NULLS FIRST, a30 ASC NULLS FIRST,
  a31 ASC NULLS FIRST, a32 ASC NULLS FIRST;

-- FDW-564: Test ORDER BY with user defined operators. Create the operator
-- family required for the test.
CREATE OPERATOR PUBLIC.<^ (
  LEFTARG = INT4,
  RIGHTARG = INT4,
  PROCEDURE = INT4EQ
);

CREATE OPERATOR PUBLIC.=^ (
  LEFTARG = INT4,
  RIGHTARG = INT4,
  PROCEDURE = INT4LT
);

CREATE OPERATOR PUBLIC.>^ (
  LEFTARG = INT4,
  RIGHTARG = INT4,
  PROCEDURE = INT4GT
);

CREATE OPERATOR FAMILY my_op_family USING btree;

CREATE FUNCTION MY_OP_CMP(A INT, B INT) RETURNS INT AS
  $$ BEGIN RETURN BTINT4CMP(A, B); END $$ LANGUAGE PLPGSQL;

CREATE OPERATOR CLASS my_op_class FOR TYPE INT USING btree FAMILY my_op_family AS
  OPERATOR 1 PUBLIC.<^,
  OPERATOR 3 PUBLIC.=^,
  OPERATOR 5 PUBLIC.>^,
  FUNCTION 1 my_op_cmp(INT, INT);

-- FDW-564: User defined operators are not pushed down.
EXPLAIN (COSTS FALSE, VERBOSE)
SELECT * FROM f_mongo_test ORDER BY a USING OPERATOR(public.<^);
EXPLAIN (COSTS FALSE, VERBOSE)
SELECT MIN(a) FROM f_mongo_test GROUP BY b ORDER BY 1 USING OPERATOR(public.<^);

-- FDW-589: Test enable_order_by_pushdown option at server and table level.
-- Test the option at server level.
-- Check only boolean values are accepted.
ALTER SERVER mongo_server OPTIONS (ADD enable_order_by_pushdown 'abc11');
ALTER SERVER mongo_server OPTIONS (ADD enable_order_by_pushdown 'false');
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c4 FROM f_test_tbl1
  WHERE c1 > c4
  ORDER BY c1 ASC NULLS FIRST;
ALTER SERVER mongo_server OPTIONS (SET enable_order_by_pushdown 'true');
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c4 FROM f_test_tbl1
  WHERE c1 > c4
  ORDER BY c1 ASC NULLS FIRST;
-- Test that setting option at table level does not affect the setting at
-- server level.
ALTER SERVER mongo_server OPTIONS (SET enable_order_by_pushdown 'false');
-- Test the option at table level.
ALTER FOREIGN TABLE f_test_tbl1 OPTIONS (ADD enable_order_by_pushdown 'true');
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c4 FROM f_test_tbl1
  WHERE c1 > c4
  ORDER BY c1 ASC NULLS FIRST;
SELECT c1, c4 FROM f_test_tbl1
  WHERE c1 > c4
  ORDER BY c1 ASC NULLS FIRST;
ALTER FOREIGN TABLE f_test_tbl1 OPTIONS (SET enable_order_by_pushdown 'false');
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT c1, c4 FROM f_test_tbl1
  WHERE c1 > c4
  ORDER BY c1 ASC NULLS FIRST;
SELECT c1, c4 FROM f_test_tbl1
  WHERE c1 > c4
  ORDER BY c1 ASC NULLS FIRST;
ALTER SERVER mongo_server OPTIONS (SET enable_order_by_pushdown 'true');
ALTER FOREIGN TABLE f_test_tbl1 OPTIONS (SET enable_order_by_pushdown 'true');

-- FDW-631: Test pushdown of boolean expression
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT name, pass FROM f_test_tbl3 WHERE pass = false ORDER BY name;
SELECT name, pass FROM f_test_tbl3 WHERE pass = false ORDER BY name;
EXPLAIN (VERBOSE, COSTS FALSE)
SELECT name, pass FROM f_test_tbl3 WHERE pass = true ORDER BY name;
SELECT name, pass FROM f_test_tbl3 WHERE pass = true ORDER BY name;

-- Cleanup
DELETE FROM f_mongo_test WHERE a != 0;
DELETE FROM f_test_tbl2 WHERE c1 IS NULL;
DROP FOREIGN TABLE f_mongo_test;
DROP FOREIGN TABLE f_test_tbl1;
DROP FOREIGN TABLE f_test_tbl2;
DROP FOREIGN TABLE f_test_tbl3;
DROP FOREIGN TABLE f_test_large;
DROP OPERATOR CLASS my_op_class USING btree;
DROP FUNCTION my_op_cmp(a INT, b INT);
DROP OPERATOR FAMILY my_op_family USING btree;
DROP OPERATOR public.>^(INT, INT);
DROP OPERATOR public.=^(INT, INT);
DROP OPERATOR public.<^(INT, INT);
DROP USER MAPPING FOR public SERVER mongo_server;
DROP SERVER mongo_server;
DROP EXTENSION mongo_fdw;
