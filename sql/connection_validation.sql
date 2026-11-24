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

-- ============================================================================
-- FDW-753: ObjectId validation (prevent out-of-bounds reads)
-- An ObjectId supplied through a JSON "$oid" field is converted with
-- bson_oid_init_from_string(), which reads 24 characters with no bounds
-- check.  A shorter or non-hexadecimal value must be rejected with a clear
-- error during document construction, before anything is sent to MongoDB.
-- ============================================================================
CREATE FOREIGN TABLE f_mongo_oid_json (_id name, j json)
  SERVER mongo_server OPTIONS (database 'mongo_fdw_regress', collection 'mongo_test');
-- Too-short / non-hex $oid must error (raised before any write to MongoDB).
INSERT INTO f_mongo_oid_json (j) VALUES ('{"$oid": "abc"}');

-- FDW-753: fix URI Injection via Unencoded Credentials
-- These tests verify that special characters in credentials are properly encoded
-- and cannot be used for URI injection attacks. The mongoc_uri_set_username/password
-- APIs handle encoding automatically, preventing injection.

-- TEST 1: Username with @ symbol (email address)
DROP USER MAPPING IF EXISTS FOR public SERVER mongo_server;
CREATE USER MAPPING FOR public SERVER mongo_server
  OPTIONS (username 'user@domain.com', password 'password123');
-- Attempt connection - should not cause URI parsing errors
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 1: Connection attempted with @ in username (auth may fail but no injection)';
  EXCEPTION
    WHEN OTHERS THEN
      -- Authentication failure is expected (invalid credentials), but should not be injection error
      IF SQLERRM LIKE '%failed to parse%' OR SQLERRM LIKE '%injection%' THEN
        RAISE NOTICE 'TEST 1 FAIL: URI injection or parsing error: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 1 PASS: No URI injection (auth failure expected): %', SQLERRM;
      END IF;
  END;
END;
$$;

-- TEST 2: Password with URI special characters
DROP USER MAPPING IF EXISTS FOR public SERVER mongo_server;
CREATE USER MAPPING FOR public SERVER mongo_server
  OPTIONS (username 'admin', password 'p@ss:word&test?value=123');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 2: Connection attempted with URI special chars in password';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%failed to parse%' OR SQLERRM LIKE '%injection%' THEN
        RAISE NOTICE 'TEST 2 FAIL: URI injection or parsing error: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 2 PASS: No URI injection (auth failure expected): %', SQLERRM;
      END IF;
  END;
END;
$$;

-- TEST 3: Password with percent encoding characters
DROP USER MAPPING IF EXISTS FOR public SERVER mongo_server;
CREATE USER MAPPING FOR public SERVER mongo_server
  OPTIONS (username 'user', password 'pass%20word');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 3: Connection attempted with percent encoding in password';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%failed to parse%' OR SQLERRM LIKE '%injection%' THEN
        RAISE NOTICE 'TEST 3 FAIL: URI injection or parsing error: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 3 PASS: No URI injection (auth failure expected): %', SQLERRM;
      END IF;
  END;
END;
$$;

-- TEST 4: Credentials with attempted URI parameter injection
DROP USER MAPPING IF EXISTS FOR public SERVER mongo_server;
CREATE USER MAPPING FOR public SERVER mongo_server
  OPTIONS (username 'user&authSource=evil', password 'pass?ssl=false&malicious=true');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 4: Connection attempted with parameter injection attempt';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%failed to parse%' OR SQLERRM LIKE '%injection%' THEN
        RAISE NOTICE 'TEST 4 FAIL: URI injection or parsing error: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 4 PASS: No URI injection (auth failure expected): %', SQLERRM;
      END IF;
  END;
END;
$$;

-- TEST 5: Special characters commonly used in passwords
DROP USER MAPPING IF EXISTS FOR public SERVER mongo_server;
CREATE USER MAPPING FOR public SERVER mongo_server
  OPTIONS (username 'admin', password '!@#$%^&*()_+-=[]{}|;:",.<>?/~`');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 5: Connection attempted with special chars in password';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%failed to parse%' OR SQLERRM LIKE '%injection%' THEN
        RAISE NOTICE 'TEST 5 FAIL: URI injection or parsing error: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 5 PASS: No URI injection (auth failure expected): %', SQLERRM;
      END IF;
  END;
END;
$$;

-- TEST 6: MongoDB operator injection attempt via password
DROP USER MAPPING IF EXISTS FOR public SERVER mongo_server;
CREATE USER MAPPING FOR public SERVER mongo_server
  OPTIONS (username 'admin', password '{"$ne": null}');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 6: Connection attempted with MongoDB operator in password';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%failed to parse%' OR SQLERRM LIKE '%injection%' THEN
        RAISE NOTICE 'TEST 6 FAIL: URI injection or parsing error: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 6 PASS: No URI injection (auth failure expected): %', SQLERRM;
      END IF;
  END;
END;
$$;

-- TEST 7: Credential validation - valid inputs
-- Control characters are allowed (matching PostgreSQL's permissive behavior)
-- MongoDB driver handles encoding automatically

-- TEST 7a: Valid credentials with printable ASCII characters (should accept)
DROP USER MAPPING IF EXISTS FOR public SERVER mongo_server;
CREATE USER MAPPING FOR public SERVER mongo_server
  OPTIONS (username 'valid_user_123', password 'ValidP@ssw0rd!2024');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 7a PASS: Valid credentials accepted (query succeeded)';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid%provided%%' THEN
        RAISE NOTICE 'TEST 7a FAIL: Valid credentials rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 7a PASS: Valid credentials accepted (connection error expected): %', SQLERRM;
      END IF;
  END;
END;
$$;

-- TEST 8: Database name validation
-- TEST 8a: Database name with URI special characters
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST, SET port :MONGO_PORT);
DROP USER MAPPING IF EXISTS FOR public SERVER mongo_server;
CREATE USER MAPPING FOR public SERVER mongo_server;
DROP FOREIGN TABLE IF EXISTS f_mongo_test_bad_db;
CREATE FOREIGN TABLE f_mongo_test_bad_db (_id name, a int, b varchar)
  SERVER mongo_server OPTIONS (database 'test?injection=true', collection 'mongo_test');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test_bad_db LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with ? in database name';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid database name%' OR SQLERRM LIKE '%URI special characters%' THEN
        RAISE NOTICE 'TEST 8a PASS: Database name with ? rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 8a FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
DROP FOREIGN TABLE IF EXISTS f_mongo_test_bad_db;

-- TEST 8b: Database name with & character
CREATE FOREIGN TABLE f_mongo_test_bad_db (_id name, a int, b varchar)
  SERVER mongo_server OPTIONS (database 'test&param=evil', collection 'mongo_test');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test_bad_db LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with & in database name';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid database name%' OR SQLERRM LIKE '%URI special characters%' THEN
        RAISE NOTICE 'TEST 8b PASS: Database name with & rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 8b FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
DROP FOREIGN TABLE IF EXISTS f_mongo_test_bad_db;

-- TEST 8c: Database name with / character
CREATE FOREIGN TABLE f_mongo_test_bad_db (_id name, a int, b varchar)
  SERVER mongo_server OPTIONS (database 'test/path', collection 'mongo_test');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test_bad_db LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with / in database name';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid database name%' OR SQLERRM LIKE '%URI special characters%' THEN
        RAISE NOTICE 'TEST 8c PASS: Database name with / rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 8c FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
DROP FOREIGN TABLE IF EXISTS f_mongo_test_bad_db;

-- TEST 9: Replica set name validation
-- TEST 9a: Replica set with = character
ALTER SERVER mongo_server OPTIONS (ADD replica_set 'rs0=injection');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with = in replica set name';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid replica set%' OR SQLERRM LIKE '%URI special characters%' THEN
        RAISE NOTICE 'TEST 9a PASS: Replica set with = rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 9a FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (DROP replica_set);

-- TEST 9b: Replica set with & character
ALTER SERVER mongo_server OPTIONS (ADD replica_set 'rs0&param=evil');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with & in replica set name';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid replica set%' OR SQLERRM LIKE '%URI special characters%' THEN
        RAISE NOTICE 'TEST 9b PASS: Replica set with & rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 9b FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (DROP replica_set);

-- TEST 10: Read preference validation
-- TEST 10a: Invalid read preference value
ALTER SERVER mongo_server OPTIONS (ADD read_preference 'invalid_pref');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with invalid read preference';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid read preference%' THEN
        RAISE NOTICE 'TEST 10a PASS: Invalid read preference rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 10a FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (DROP read_preference);

-- TEST 10b: Valid read preference values (should accept)
ALTER SERVER mongo_server OPTIONS (ADD read_preference 'primary');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 10b PASS: Valid read preference "primary" accepted (query succeeded)';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid read preference%' THEN
        RAISE NOTICE 'TEST 10b FAIL: Valid read preference rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 10b PASS: Valid read preference accepted (connection error expected): %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (DROP read_preference);

-- TEST 10c: Test all valid read preference values
DO $$
DECLARE
  prefs text[] := ARRAY['primary', 'primaryPreferred', 'secondary', 'secondaryPreferred', 'nearest'];
  pref text;
BEGIN
  FOREACH pref IN ARRAY prefs
  LOOP
    BEGIN
      EXECUTE format('ALTER SERVER mongo_server OPTIONS (ADD read_preference %L)', pref);
      BEGIN
        PERFORM a, b FROM f_mongo_test LIMIT 1;
        RAISE NOTICE 'TEST 10c PASS: Preference % accepted (query succeeded)', pref;
      EXCEPTION
        WHEN OTHERS THEN
          IF SQLERRM LIKE '%invalid read preference%' THEN
            RAISE NOTICE 'TEST 10c FAIL: Valid preference % rejected', pref;
          ELSE
            RAISE NOTICE 'TEST 10c PASS: Preference % accepted (connection error expected)', pref;
          END IF;
      END;
      EXECUTE 'ALTER SERVER mongo_server OPTIONS (DROP read_preference)';
    EXCEPTION
      WHEN OTHERS THEN
        RAISE NOTICE 'TEST 10c: Error testing preference %: %', pref, SQLERRM;
    END;
  END LOOP;
END;
$$;

-- TEST 11: Server address (hostname) validation
-- TEST 11a: Server address with / character
ALTER SERVER mongo_server OPTIONS (SET address 'localhost/injection');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with / in server address';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid server address%' OR SQLERRM LIKE '%invalid character%' THEN
        RAISE NOTICE 'TEST 11a PASS: Server address with / rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 11a FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);

-- TEST 11b: Server address with ? character
ALTER SERVER mongo_server OPTIONS (SET address 'localhost?param=value');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with ? in server address';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid server address%' OR SQLERRM LIKE '%invalid character%' THEN
        RAISE NOTICE 'TEST 11b PASS: Server address with ? rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 11b FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);

-- TEST 11c: Server address with @ character
ALTER SERVER mongo_server OPTIONS (SET address 'user@localhost');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with @ in server address';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid server address%' OR SQLERRM LIKE '%invalid character%' THEN
        RAISE NOTICE 'TEST 11c PASS: Server address with @ rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 11c FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);

-- TEST 11d: Server address with & character
ALTER SERVER mongo_server OPTIONS (SET address 'localhost&param=evil');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with & in server address';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid server address%' OR SQLERRM LIKE '%invalid character%' THEN
        RAISE NOTICE 'TEST 11d PASS: Server address with & rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 11d FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);

-- TEST 11e: Server address with = character
ALTER SERVER mongo_server OPTIONS (SET address 'localhost=injection');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with = in server address';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid server address%' OR SQLERRM LIKE '%invalid character%' THEN
        RAISE NOTICE 'TEST 11e PASS: Server address with = rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 11e FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);

-- TEST 11f: Server address with # character (fragment)
ALTER SERVER mongo_server OPTIONS (SET address 'localhost#fragment');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with # in server address';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid server address%' OR SQLERRM LIKE '%invalid character%' THEN
        RAISE NOTICE 'TEST 11f PASS: Server address with # rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 11f FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);

-- TEST 11g: Server address with space
ALTER SERVER mongo_server OPTIONS (SET address 'local host');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'UNEXPECTED: Query succeeded with space in server address';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid server address%' OR SQLERRM LIKE '%invalid character%' THEN
        RAISE NOTICE 'TEST 11g PASS: Server address with space rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 11g FAIL: Unexpected error: %', SQLERRM;
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);


-- TEST 11h: Valid server addresses (should accept)
-- Test localhost
ALTER SERVER mongo_server OPTIONS (SET address 'localhost');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 11h PASS: Valid hostname "localhost" accepted';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid server address%' THEN
        RAISE NOTICE 'TEST 11h FAIL: Valid hostname rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 11h PASS: Valid hostname "localhost" accepted';
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);

-- Test IPv4 address
ALTER SERVER mongo_server OPTIONS (SET address '127.0.0.1');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 11h PASS: Valid IPv4 "127.0.0.1" accepted';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid server address%' THEN
        RAISE NOTICE 'TEST 11h FAIL: Valid IPv4 rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 11h PASS: Valid IPv4 "127.0.0.1" accepted';
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);

-- Test hostname with hyphens
ALTER SERVER mongo_server OPTIONS (SET address 'mongo-db-server.example.com');
DO $$
BEGIN
  BEGIN
    PERFORM a, b FROM f_mongo_test LIMIT 1;
    RAISE NOTICE 'TEST 11h PASS: Valid hostname with hyphens accepted';
  EXCEPTION
    WHEN OTHERS THEN
      IF SQLERRM LIKE '%invalid server address%' THEN
        RAISE NOTICE 'TEST 11h FAIL: Valid hostname with hyphens rejected: %', SQLERRM;
      ELSE
        RAISE NOTICE 'TEST 11h PASS: Valid hostname with hyphens accepted';
      END IF;
  END;
END;
$$;
ALTER SERVER mongo_server OPTIONS (SET address :MONGO_HOST);

-- Cleanup
DROP FOREIGN TABLE f_mongo_test;
DROP FOREIGN TABLE f_mongo_oid_json;
DROP USER MAPPING FOR public SERVER mongo_server;
DROP SERVER mongo_server;
DROP EXTENSION mongo_fdw;
