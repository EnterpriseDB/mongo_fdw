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

-- Port outside ushort range. Error.
CREATE SERVER mongo_server1 FOREIGN DATA WRAPPER mongo_fdw
  OPTIONS (address :MONGO_HOST, port '65537');
ALTER SERVER mongo_server OPTIONS (SET port '65537');

-- Validate extension, server and mapping details
CREATE OR REPLACE FUNCTION show_details(host TEXT, port TEXT, uid TEXT, pwd TEXT) RETURNS int AS $$
DECLARE
  ext TEXT;
  srv TEXT;
  sopts TEXT;
  uopts TEXT;
BEGIN
  SELECT e.fdwname, srvname, array_to_string(s.srvoptions, ','), array_to_string(u.umoptions, ',')
    INTO ext, srv, sopts, uopts
    FROM pg_foreign_data_wrapper e LEFT JOIN pg_foreign_server s ON e.oid = s.srvfdw LEFT JOIN pg_user_mapping u ON s.oid = u.umserver
    WHERE e.fdwname = 'mongo_fdw'
    ORDER BY 1, 2, 3, 4;

  raise notice 'Extension            : %', ext;
  raise notice 'Server               : %', srv;

  IF strpos(sopts, host) <> 0 AND strpos(sopts, port) <> 0 THEN
    raise notice 'Server_Options       : matched';
  END IF;

  IF strpos(uopts, uid) <> 0 AND strpos(uopts, pwd) <> 0 THEN
    raise notice 'User_Mapping_Options : matched';
  END IF;

  return 1;
END;
$$ language plpgsql;

SELECT show_details(:MONGO_HOST, :MONGO_PORT, :MONGO_USER_NAME, :MONGO_PASS);

-- Create foreign tables and perform basic SQL operations
CREATE FOREIGN TABLE f_mongo_test (_id name, a int, b varchar)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'mongo_test');
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;
INSERT INTO f_mongo_test VALUES ('0', 2, 'mongo_test insert');
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;
UPDATE f_mongo_test SET b = 'mongo_test update' WHERE a = 2;
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;
DELETE FROM f_mongo_test WHERE a = 2;
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;

-- Test SSL option when MongoDB server running in non-SSL mode.
-- Set non-boolean value, should throw an error.
ALTER SERVER mongo_server OPTIONS (ssl '1');
ALTER SERVER mongo_server OPTIONS (ssl 'x');
-- Check for default value i.e. false
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;
-- Set 'true'.
ALTER SERVER mongo_server OPTIONS (ssl 'true');
-- Results into an error as MongoDB server is running in non-SSL mode.
\set VERBOSITY terse
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;
\set VERBOSITY default
-- Switch back to 'false'.
ALTER SERVER mongo_server OPTIONS (SET ssl 'false');
-- Should now be successful.
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;

-- Alter server to add authentication_database option
ALTER SERVER mongo_server OPTIONS (ADD authentication_database 'NOT_EXIST_DB');
ALTER USER MAPPING FOR public SERVER mongo_server
  OPTIONS (ADD username :MONGO_USER_NAME, password :MONGO_PASS);
-- Below query will fail with authentication error as user cannot be
-- authenticated against given authentication_database.
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;
-- Now changed to valid authentication_database so select query should work.
ALTER SERVER mongo_server
  OPTIONS (SET authentication_database 'mongo_fdw_regress');
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;

ALTER SERVER mongo_server
  OPTIONS (DROP authentication_database);
ALTER USER MAPPING FOR public SERVER mongo_server
  OPTIONS (DROP username, DROP password);

-- FDW-464: Support use_remote_estimate option at server level.
-- Check only boolean values are accepted.
ALTER SERVER mongo_server OPTIONS (ADD use_remote_estimate 'abc11');
-- Check default behaviour. Should be 'false'.
EXPLAIN(COSTS OFF)
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;
-- Enable remote estimation.
ALTER SERVER mongo_server OPTIONS (ADD use_remote_estimate 'true');
EXPLAIN(COSTS OFF)
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;
-- Disable remote estimation.
ALTER SERVER mongo_server OPTIONS (SET use_remote_estimate 'false');
EXPLAIN(COSTS OFF)
SELECT a, b FROM f_mongo_test ORDER BY 1, 2;

-- Cleanup
DROP FOREIGN TABLE f_mongo_test;
DROP USER MAPPING FOR public SERVER mongo_server;
DROP SERVER mongo_server;
DROP EXTENSION mongo_fdw;
