/* mongo_fdw/mongo_fdw--1.1.sql */

-- Portions Copyright (c) 2004-2023, EnterpriseDB Corporation.
-- Portions Copyright © 2012–2014 Citus Data, Inc.

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION mongo_fdw" to load this file. \quit

CREATE FUNCTION mongo_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION mongo_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER mongo_fdw
  HANDLER mongo_fdw_handler
  VALIDATOR mongo_fdw_validator;

CREATE OR REPLACE FUNCTION mongo_fdw_version()
  RETURNS pg_catalog.int4 STRICT
  AS 'MODULE_PATHNAME' LANGUAGE C;
