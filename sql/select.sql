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

-- Check version
SELECT mongo_fdw_version();

-- Create foreign tables
CREATE FOREIGN TABLE f_mongo_test (_id name, a int, b text)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'mongo_test');
CREATE FOREIGN TABLE f_test_tbl1 (_id NAME, c1 INTEGER, c2 VARCHAR(10), c3 CHAR(9),c4 INTEGER, c5 pg_catalog.Date, c6 DECIMAL, c7 INTEGER, c8 INTEGER)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl1');
CREATE FOREIGN TABLE f_test_tbl2 (_id NAME, c1 INTEGER, c2 VARCHAR(14), c3 VARCHAR(13))
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl2');
CREATE FOREIGN TABLE countries (_id NAME, name VARCHAR, population INTEGER, capital VARCHAR, hdi FLOAT)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'countries');
CREATE FOREIGN TABLE country_elections (_id NAME, "lastElections.type" VARCHAR, "lastElections.date" pg_catalog.TIMESTAMP)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'countries');
CREATE FOREIGN TABLE main_exports (_id NAME, "mainExports" TEXT[] )
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'countries');
CREATE FOREIGN TABLE test_json ( __doc json)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'warehouse');
CREATE FOREIGN TABLE test_jsonb ( __doc jsonb)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'warehouse');
CREATE FOREIGN TABLE test_text ( __doc text)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'warehouse');
CREATE FOREIGN TABLE test_varchar ( __doc varchar)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'warehouse');
CREATE FOREIGN TABLE f_test5 (_id NAME, c1 NUMERIC)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test5');
CREATE FOREIGN TABLE f_test_tbl4 (_id NAME, a NUMERIC(12, 2))
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl4');
CREATE FOREIGN TABLE f_test_tbl5 (_id NAME, a BOOLEAN)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl4');
CREATE FOREIGN TABLE f_test_tbl6 (_id NAME, a INTEGER)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl5');
CREATE FOREIGN TABLE f_test_tbl7 (_id NAME, a INTEGER)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test_tbl4');

SET datestyle TO ISO;

-- Retrieve data from foreign table using SELECT statement.
SELECT c1, c2, c3, c4, c5, c6, c7, c8 FROM f_test_tbl1
  ORDER BY c1 DESC, c8;
SELECT DISTINCT c8 FROM f_test_tbl1 ORDER BY 1;
SELECT c2 AS "Employee Name" FROM f_test_tbl1 ORDER BY c2 COLLATE "C";
SELECT c8, c6, c7 FROM f_test_tbl1 ORDER BY 1, 2, 3;
SELECT c1, c2, c3, c4, c5, c6, c7, c8 FROM f_test_tbl1
  WHERE c1 = 100 ORDER BY 1;
SELECT c1, c2, c3, c4, c5, c6, c7, c8 FROM f_test_tbl1
  WHERE c1 = 100 OR c1 = 700 ORDER BY 1;
SELECT c1, c2, c3 FROM f_test_tbl1 WHERE c3 like 'SALESMAN' ORDER BY 1;
SELECT c1, c2, c3 FROM f_test_tbl1 WHERE c1 IN (100, 700) ORDER BY 1;
SELECT c1, c2, c3 FROM f_test_tbl1 WHERE c1 NOT IN (100, 700) ORDER BY 1 LIMIT 5;
SELECT c1, c2, c8 FROM f_test_tbl1 WHERE c8 BETWEEN 10 AND 20 ORDER BY 1;
SELECT c1, c2, c6 FROM f_test_tbl1 ORDER BY 1 OFFSET 5;

-- Retrieve data from foreign table using group by clause.
SELECT c8 "Department", COUNT(c1) "Total Employees" FROM f_test_tbl1
  GROUP BY c8 ORDER BY c8;
SELECT c8, SUM(c6) FROM f_test_tbl1
  GROUP BY c8 HAVING c8 IN (10, 30) ORDER BY c8;
SELECT c8, SUM(c6) FROM f_test_tbl1
  GROUP BY c8 HAVING SUM(c6) > 9400 ORDER BY c8;

-- Retrieve data from foreign table using sub-queries.
SELECT c1, c2, c6 FROM f_test_tbl1
  WHERE c8 <> ALL (SELECT c1 FROM f_test_tbl2 WHERE c1 IN (10, 30, 40))
  ORDER BY c1;
SELECT c1, c2, c3 FROM f_test_tbl2
  WHERE EXISTS (SELECT 1 FROM f_test_tbl1 WHERE f_test_tbl2.c1 = f_test_tbl1.c8)
  ORDER BY 1, 2;
SELECT c1, c2, c3, c4, c5, c6, c7, c8 FROM f_test_tbl1
  WHERE c8 NOT IN (SELECT c1 FROM f_test_tbl2) ORDER BY c1;

-- Retrieve data from foreign table using UNION operator.
SELECT c1, c2 FROM f_test_tbl2 UNION
SELECT c1, c2 FROM f_test_tbl1 ORDER BY c1;

SELECT c1, c2 FROM f_test_tbl2 UNION ALL
SELECT c1, c2 FROM f_test_tbl1 ORDER BY c1;

-- Retrieve data from foreign table using INTERSECT operator.
SELECT c1, c2 FROM f_test_tbl1 WHERE c1 >= 800 INTERSECT
SELECT c1, c2 FROM f_test_tbl1 WHERE c1 >= 400 ORDER BY c1;

SELECT c1, c2 FROM f_test_tbl1 WHERE c1 >= 800 INTERSECT ALL
SELECT c1, c2 FROM f_test_tbl1 WHERE c1 >= 400 ORDER BY c1;

-- Retrieve data from foreign table using EXCEPT operator.
SELECT c1, c2 FROM f_test_tbl1 EXCEPT
SELECT c1, c2 FROM f_test_tbl1 WHERE c1 > 900 ORDER BY c1;

SELECT c1, c2 FROM f_test_tbl1 EXCEPT ALL
SELECT c1, c2 FROM f_test_tbl1 WHERE c1 > 900 ORDER BY c1;

-- Retrieve data from foreign table using CTE (with clause).
WITH
  with_qry AS (SELECT c1, c2, c3 FROM f_test_tbl2)
SELECT e.c2, e.c6, w.c1, w.c2 FROM f_test_tbl1 e, with_qry w
  WHERE e.c8 = w.c1 ORDER BY e.c8, e.c2 COLLATE "C";

WITH
  test_tbl2_costs AS (SELECT d.c2, SUM(c6) test_tbl2_total FROM f_test_tbl1 e, f_test_tbl2 d
    WHERE e.c8 = d.c1 GROUP BY 1),
  avg_cost AS (SELECT SUM(test_tbl2_total)/COUNT(*) avg FROM test_tbl2_costs)
SELECT * FROM test_tbl2_costs
  WHERE test_tbl2_total > (SELECT avg FROM avg_cost) ORDER BY c2 COLLATE "C";

-- Retrieve data from foreign table using window clause.
SELECT c8, c1, c6, AVG(c6) OVER (PARTITION BY c8) FROM f_test_tbl1
  ORDER BY c8, c1;
SELECT c8, c1, c6, COUNT(c6) OVER (PARTITION BY c8) FROM f_test_tbl1
  WHERE c8 IN (10, 30, 40, 50, 60, 70) ORDER BY c8, c1;
SELECT c8, c1, c6, SUM(c6) OVER (PARTITION BY c8) FROM f_test_tbl1
  ORDER BY c8, c1;

-- Views
CREATE VIEW smpl_vw AS
  SELECT c1, c2, c3, c4, c5, c6, c7, c8 FROM f_test_tbl1 ORDER BY c1;
SELECT * FROM smpl_vw ORDER BY 1;

CREATE VIEW comp_vw (s1, s2, s3, s6, s7, s8, d2) AS
  SELECT s.c1, s.c2, s.c3, s.c6, s.c7, s.c8, d.c2
    FROM f_test_tbl2 d, f_test_tbl1 s WHERE d.c1 = s.c8 AND d.c1 = 10
    ORDER BY s.c1;
SELECT * FROM comp_vw ORDER BY 1;

CREATE TEMPORARY VIEW temp_vw AS
  SELECT c1, c2, c3 FROM f_test_tbl2;
SELECT * FROM temp_vw ORDER BY 1, 2;

CREATE VIEW mul_tbl_view AS
  SELECT d.c1 dc1, d.c2 dc2, e.c1 ec1, e.c2 ec2, e.c6 ec6
    FROM f_test_tbl2 d INNER JOIN f_test_tbl1 e ON d.c1 = e.c8 ORDER BY d.c1;
SELECT * FROM mul_tbl_view ORDER BY 1, 2, 3;

-- Foreign-Foreign table joins

-- CROSS JOIN.
SELECT f_test_tbl2.c2, f_test_tbl1.c2
  FROM f_test_tbl2 CROSS JOIN f_test_tbl1 ORDER BY 1, 2;
-- INNER JOIN.
SELECT d.c1, d.c2, e.c1, e.c2, e.c6, e.c8
  FROM f_test_tbl2 d, f_test_tbl1 e WHERE d.c1 = e.c8 ORDER BY 1, 3;
SELECT d.c1, d.c2, e.c1, e.c2, e.c6, e.c8
  FROM f_test_tbl2 d INNER JOIN f_test_tbl1 e ON d.c1 = e.c8 ORDER BY 1, 3;
-- OUTER JOINS.
SELECT d.c1, d.c2, e.c1, e.c2, e.c6, e.c8
  FROM f_test_tbl2 d LEFT OUTER JOIN f_test_tbl1 e ON d.c1 = e.c8 ORDER BY 1, 3;
SELECT d.c1, d.c2, e.c1, e.c2, e.c6, e.c8
  FROM f_test_tbl2 d RIGHT OUTER JOIN f_test_tbl1 e ON d.c1 = e.c8 ORDER BY 1, 3;
SELECT d.c1, d.c2, e.c1, e.c2, e.c6, e.c8
  FROM f_test_tbl2 d FULL OUTER JOIN f_test_tbl1 e ON d.c1 = e.c8 ORDER BY 1, 3;

-- Local-Foreign table joins.
CREATE TABLE l_test_tbl1 AS
  SELECT c1, c2, c3, c4, c5, c6, c7, c8 FROM f_test_tbl1;
CREATE TABLE l_test_tbl2 AS
  SELECT c1, c2, c3 FROM f_test_tbl2;

-- CROSS JOIN.
SELECT f_test_tbl2.c2, l_test_tbl1.c2 FROM f_test_tbl2 CROSS JOIN l_test_tbl1 ORDER BY 1, 2;
-- INNER JOIN.
SELECT d.c1, d.c2, e.c1, e.c2, e.c6, e.c8
  FROM l_test_tbl2 d, f_test_tbl1 e WHERE d.c1 = e.c8 ORDER BY 1, 3;
SELECT d.c1, d.c2, e.c1, e.c2, e.c6, e.c8
  FROM f_test_tbl2 d INNER JOIN l_test_tbl1 e ON d.c1 = e.c8 ORDER BY 1, 3;
-- OUTER JOINS.
SELECT d.c1, d.c2, e.c1, e.c2, e.c6, e.c8
  FROM f_test_tbl2 d LEFT OUTER JOIN l_test_tbl1 e ON d.c1 = e.c8 ORDER BY 1, 3;
SELECT d.c1, d.c2, e.c1, e.c2, e.c6, e.c8
  FROM f_test_tbl2 d RIGHT OUTER JOIN l_test_tbl1 e ON d.c1 = e.c8 ORDER BY 1, 3;
SELECT d.c1, d.c2, e.c1, e.c2, e.c6, e.c8
  FROM f_test_tbl2 d FULL OUTER JOIN l_test_tbl1 e ON d.c1 = e.c8 ORDER BY 1, 3;

-- Retrieve complex data containing Sub-fields, dates, Arrays
SELECT * FROM countries ORDER BY _id;
SELECT * FROM country_elections ORDER BY _id;
SELECT * FROM main_exports ORDER BY _id;

-- Retrieve complex data containing Json objects (__doc tests)
SELECT json_data.key AS key1, json_data.value AS value1
  FROM test_json, json_each_text(test_json.__doc) AS json_data
  WHERE key NOT IN ('_id') ORDER BY json_data.key COLLATE "C";
SELECT json_data.key AS key1, json_data.value AS value1
  FROM test_jsonb, jsonb_each_text(test_jsonb.__doc) AS json_data
  WHERE key NOT IN ('_id') ORDER BY json_data.key COLLATE "C";
SELECT json_data.key AS key1, json_data.value AS value1
  FROM test_text, json_each_text(test_text.__doc::json) AS json_data
  WHERE key NOT IN ('_id') ORDER BY json_data.key COLLATE "C";
SELECT json_data.key AS key1, json_data.value AS value1
  FROM test_varchar, json_each_text(test_varchar.__doc::json) AS json_data
  WHERE key NOT IN ('_id') ORDER BY json_data.key COLLATE "C";

-- Inserts some values in mongo_test collection.
INSERT INTO f_mongo_test VALUES ('0', 1, 'One');
INSERT INTO f_mongo_test VALUES ('0', 2, 'Two');
INSERT INTO f_mongo_test VALUES ('0', 3, 'Three');
INSERT INTO f_mongo_test VALUES ('0', 4, 'Four');
INSERT INTO f_mongo_test VALUES ('0', 5, 'Five');
INSERT INTO f_mongo_test VALUES ('0', 6, 'Six');
INSERT INTO f_mongo_test VALUES ('0', 7, 'Seven');
INSERT INTO f_mongo_test VALUES ('0', 8, 'Eight');
INSERT INTO f_mongo_test VALUES ('0', 9, 'Nine');
INSERT INTO f_mongo_test VALUES ('0', 10, 'Ten');

-- Retrieve Data From foreign tables in functions.
CREATE OR REPLACE FUNCTION test_param_where() RETURNS void AS $$
DECLARE
  n varchar;
BEGIN
  FOR x IN 1..9 LOOP
    SELECT b INTO n FROM f_mongo_test WHERE a = x;
    RAISE NOTICE 'Found number %', n;
  END LOOP;
  return;
END
$$ LANGUAGE plpgsql;

SELECT test_param_where();

-- FDW-103: Parameter expression should work correctly with WHERE clause.
SELECT a, b FROM f_mongo_test WHERE a = (SELECT 2) ORDER BY a;
SELECT a, b FROM f_mongo_test WHERE b = (SELECT 'Seven'::text) ORDER BY a;
-- Create local table and load data into it.
CREATE TABLE l_mongo_test AS SELECT a, b FROM f_mongo_test;
-- Check correlated query.
SELECT a, b FROM l_mongo_test lt
  WHERE lt.b = (SELECT b FROM f_mongo_test ft WHERE lt.b = ft.b)
  ORDER BY a;
SELECT a, b FROM l_mongo_test lt
  WHERE lt.a = (SELECT a FROM f_mongo_test ft WHERE lt.a = ft.a)
  ORDER BY a;
SELECT c1, c8 FROM f_test_tbl1 ft1
  WHERE ft1.c8 = (SELECT c1 FROM f_test_tbl2 ft2 WHERE ft1.c8 = ft2.c1)
  ORDER BY c1 LIMIT 2;

-- FDW-197: Casting target list should give correct result.
SELECT a::float FROM f_mongo_test ORDER BY a LIMIT 2;
SELECT a::boolean FROM f_mongo_test ORDER BY a LIMIT 2;
SELECT a, b::varchar FROM f_mongo_test ORDER BY a LIMIT 3;
SELECT a::float, b::varchar FROM f_mongo_test ORDER BY a LIMIT 2;
SELECT a::real, b::char(20) FROM f_mongo_test ORDER BY a LIMIT 2;
SELECT c1, c2::text FROM f_test_tbl1 ORDER BY c1 LIMIT 2;
SELECT a, LENGTH(b) FROM f_mongo_test ORDER BY 1 LIMIT 2;
SELECT t1.c6::float, t1.c6::int, t1.c5::timestamptz, t1.c3::text, t2.c1::numeric, t2.c3
  FROM f_test_tbl1 t1, f_test_tbl2 t2 WHERE t1.c8 = t2.c1
  ORDER BY t2.c1, t1.c6 LIMIT 5;
SELECT SUM(a::float), SUM(a % 2), a % 2 AS "a % 2"FROM f_mongo_test
  GROUP BY a % 2 ORDER BY 2;
SELECT (c6::float + (c1 * length(c3::text))) AS "c1 + c6", c1, c6
  FROM f_test_tbl1 ORDER BY c1 LIMIT 5;

-- FDW-249; LEFT JOIN LATERAL should not crash
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.a, t1.b, t3.a, t1_a FROM f_mongo_test t1 LEFT JOIN LATERAL (
  SELECT t2.a, t1.a AS t1_a FROM f_mongo_test t2) t3 ON t1.a = t3.a ORDER BY 1 ASC NULLS FIRST;
SELECT t1.a, t1.b, t3.a, t1_a FROM f_mongo_test t1 LEFT JOIN LATERAL (
  SELECT t2.a, t1.a AS t1_a FROM f_mongo_test t2) t3 ON t1.a = t3.a ORDER BY 1 ASC NULLS FIRST;
SELECT t1.c1, t3.c1, t3.t1_c8 FROM f_test_tbl1 t1 INNER JOIN LATERAL (
  SELECT t2.c1, t1.c8 AS t1_c8 FROM f_test_tbl2 t2) t3 ON t3.c1 = t3.t1_c8
  ORDER BY 1, 2, 3;
SELECT t1.c1, t3.c1, t3.t1_c8 FROM l_test_tbl1 t1 LEFT JOIN LATERAL (
  SELECT t2.c1, t1.c8 AS t1_c8 FROM f_test_tbl2 t2) t3 ON t3.c1 = t3.t1_c8
  ORDER BY 1, 2, 3;
SELECT c1, c2, (SELECT r FROM (SELECT c1 AS c1) x, LATERAL (SELECT c1 AS r) y)
  FROM f_test_tbl1 ORDER BY 1, 2, 3;
-- LATERAL JOIN with RIGHT should throw error
SELECT t1.c1, t3.c1, t3.t1_c8 FROM f_test_tbl1 t1 RIGHT JOIN LATERAL (
  SELECT t2.c1, t1.c8 AS t1_c8 FROM f_test_tbl2 t2) t3 ON t3.c1 = t3.t1_c8
  ORDER BY 1, 2, 3;

-- FDW-262: Should throw an error when we select system attribute.
SELECT xmin FROM f_test_tbl1;
SELECT ctid, xmax, tableoid FROM f_test_tbl1;
SELECT xmax, c1 FROM f_test_tbl1;
SELECT count(tableoid) FROM f_test_tbl1;

-- FDW-391: Support whole-row reference.
EXPLAIN (VERBOSE, COSTS OFF)
SELECT t1.c2, t1 FROM f_test_tbl1 t1
  WHERE c1 = 100 ORDER BY 1;

-- Force hash-join for consistent result.
SET enable_mergejoin TO off;
SET enable_nestloop TO off;
EXPLAIN (VERBOSE, COSTS OFF)
SELECT d, d.c2, e.c1, e
  FROM f_test_tbl2 d LEFT OUTER JOIN f_test_tbl1 e ON d.c1 = e.c8 ORDER BY 1, 3;
RESET enable_mergejoin;
RESET enable_nestloop;

-- FDW-427: The numeric value should display correctly as per precision and
-- scale defined.
SELECT c1 FROM f_test5 ORDER BY 1;
-- Number with the required precision.
DROP FOREIGN TABLE f_test5;
CREATE FOREIGN TABLE f_test5 (_id NAME, c1 NUMERIC(8, 6))
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test5');
SELECT c1 FROM f_test5 ORDER BY 1;
-- Number with less scale. Should round-off the scale.
DROP FOREIGN TABLE f_test5;
CREATE FOREIGN TABLE f_test5 (_id NAME, c1 NUMERIC(6, 2))
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test5');
SELECT c1 FROM f_test5 ORDER BY 1;
-- Number only with precision.
DROP FOREIGN TABLE f_test5;
CREATE FOREIGN TABLE f_test5 (_id NAME, c1 NUMERIC(2))
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test5');
SELECT c1 FROM f_test5 ORDER BY 1;
-- Number with improper precision and scale,
-- resulting in error "numeric field overflow".
DROP FOREIGN TABLE f_test5;
CREATE FOREIGN TABLE f_test5 (_id NAME, c1 NUMERIC(3, 2))
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'test5');
SELECT c1 FROM f_test5 ORDER BY 1;

-- FDW-418: Resolve data compatibility.
SELECT a FROM f_test_tbl4 ORDER BY 1;
SELECT a FROM f_test_tbl5 ORDER BY 1;
SELECT a FROM f_test_tbl6 ORDER BY 1;
SELECT a FROM f_test_tbl7 ORDER BY 1;


-- FDW-529: Fix server crash caused due to missed handling of Param node for
-- comparison expressions while preparing query filter.

CREATE OR REPLACE FUNCTION fdw529_test_param_where() RETURNS int AS $$
DECLARE
  val1 INT := 5;
  val2 INT := 10;
  cnt INT;
BEGIN
  SELECT count(*) INTO cnt FROM f_mongo_test WHERE a > val1 AND a < val2;
  RETURN cnt;
END
$$ LANGUAGE plpgsql;

SELECT fdw529_test_param_where();
SELECT fdw529_test_param_where();
SELECT fdw529_test_param_where();
SELECT fdw529_test_param_where();
SELECT fdw529_test_param_where();
-- This should not crash
SELECT fdw529_test_param_where();

-- Cleanup
DELETE FROM f_mongo_test WHERE a != 0;
DROP TABLE l_test_tbl1;
DROP TABLE l_test_tbl2;
DROP TABLE l_mongo_test;
DROP VIEW smpl_vw;
DROP VIEW comp_vw;
DROP VIEW temp_vw;
DROP VIEW mul_tbl_view;
DROP FUNCTION test_param_where();
DROP FUNCTION fdw529_test_param_where();
DROP FOREIGN TABLE f_mongo_test;
DROP FOREIGN TABLE f_test_tbl1;
DROP FOREIGN TABLE f_test_tbl2;
DROP FOREIGN TABLE countries;
DROP FOREIGN TABLE country_elections;
DROP FOREIGN TABLE main_exports;
DROP FOREIGN TABLE test_json;
DROP FOREIGN TABLE test_jsonb;
DROP FOREIGN TABLE test_text;
DROP FOREIGN TABLE test_varchar;
DROP FOREIGN TABLE f_test5;
DROP FOREIGN TABLE f_test_tbl4;
DROP FOREIGN TABLE f_test_tbl5;
DROP FOREIGN TABLE f_test_tbl6;
DROP FOREIGN TABLE f_test_tbl7;
DROP USER MAPPING FOR public SERVER mongo_server;
DROP SERVER mongo_server;
DROP EXTENSION mongo_fdw;
