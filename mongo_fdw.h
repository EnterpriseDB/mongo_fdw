/*-------------------------------------------------------------------------
 *
 * mongo_fdw.h
 * 		Foreign-data wrapper for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2023, EnterpriseDB Corporation.
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		mongo_fdw.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MONGO_FDW_H
#define MONGO_FDW_H

#include "config.h"
#include "mongo_wrapper.h"

#ifdef META_DRIVER
#include "mongoc.h"
#else
#include "mongo.h"
#endif
#include "access/reloptions.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#if PG_VERSION_NUM < 120000
#include "nodes/relation.h"
#endif
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datetime.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

#ifdef META_DRIVER
#define BSON bson_t
#define BSON_TYPE 							bson_type_t
#define BSON_ITERATOR 						bson_iter_t
#define MONGO_CONN 							mongoc_client_t
#define MONGO_CURSOR 						mongoc_cursor_t
#define BSON_TYPE_DOCUMENT 					BSON_TYPE_DOCUMENT
#define BSON_TYPE_NULL 						BSON_TYPE_NULL
#define BSON_TYPE_ARRAY 					BSON_TYPE_ARRAY
#define BSON_TYPE_INT32 					BSON_TYPE_INT32
#define BSON_TYPE_INT64						BSON_TYPE_INT64
#define BSON_TYPE_DOUBLE 					BSON_TYPE_DOUBLE
#define BSON_TYPE_BINDATA 					BSON_TYPE_BINARY
#define BSON_TYPE_BOOL 						BSON_TYPE_BOOL
#define BSON_TYPE_UTF8 						BSON_TYPE_UTF8
#define BSON_TYPE_OID 						BSON_TYPE_OID
#define BSON_TYPE_DATE_TIME 				BSON_TYPE_DATE_TIME
#define BSON_TYPE_SYMBOL 					BSON_TYPE_SYMBOL
#define BSON_TYPE_UNDEFINED 				BSON_TYPE_UNDEFINED
#define BSON_TYPE_REGEX 					BSON_TYPE_REGEX
#define BSON_TYPE_CODE 						BSON_TYPE_CODE
#define BSON_TYPE_CODEWSCOPE 				BSON_TYPE_CODEWSCOPE
#define BSON_TYPE_TIMESTAMP 				BSON_TYPE_TIMESTAMP

#define PREF_READ_PRIMARY_NAME 				"readPrimary"
#define PREF_READ_SECONDARY_NAME 			"readSecondary"
#define PREF_READ_PRIMARY_PREFERRED_NAME 	"readPrimaryPreferred"
#define PREF_READ_SECONDARY_PREFERRED_NAME  "readSecondaryPreferred"
#define PREF_READ_NEAREST_NAME 				"readNearest"

#define BSON_ITER_BOOL 						bson_iter_bool
#define BSON_ITER_DOUBLE 					bson_iter_double
#define BSON_ITER_INT32 					bson_iter_int32
#define BSON_ITER_INT64 					bson_iter_int64
#define BSON_ITER_OID 						bson_iter_oid
#define BSON_ITER_UTF8 						bson_iter_utf8
#define BSON_ITER_REGEX 					bson_iter_regex
#define BSON_ITER_DATE_TIME 				bson_iter_date_time
#define BSON_ITER_CODE 						bson_iter_code
#define BSON_ITER_VALUE 					bson_iter_value
#define BSON_ITER_KEY 						bson_iter_key
#define BSON_ITER_NEXT 						bson_iter_next
#define BSON_ITER_TYPE 						bson_iter_type
#define BSON_ITER_BINARY 					bson_iter_binary
#else
#define BSON 								bson
#define BSON_TYPE 							bson_type
#define BSON_ITERATOR 						bson_iterator
#define MONGO_CONN 							mongo
#define MONGO_CURSOR 						mongo_cursor
#define BSON_TYPE_DOCUMENT 					BSON_OBJECT
#define BSON_TYPE_NULL 						BSON_NULL
#define BSON_TYPE_ARRAY						BSON_ARRAY
#define BSON_TYPE_INT32 					BSON_INT
#define BSON_TYPE_INT64 					BSON_LONG
#define BSON_TYPE_DOUBLE 					BSON_DOUBLE
#define BSON_TYPE_BINDATA 					BSON_BINDATA
#define BSON_TYPE_BOOL 						BSON_BOOL
#define BSON_TYPE_UTF8 						BSON_STRING
#define BSON_TYPE_OID 						BSON_OID
#define BSON_TYPE_DATE_TIME 				BSON_DATE
#define BSON_TYPE_SYMBOL 					BSON_SYMBOL
#define BSON_TYPE_UNDEFINED 				BSON_UNDEFINED
#define BSON_TYPE_REGEX 					BSON_REGEX
#define BSON_TYPE_CODE 						BSON_CODE
#define BSON_TYPE_CODEWSCOPE 				BSON_CODEWSCOPE
#define BSON_TYPE_TIMESTAMP 				BSON_TIMESTAMP

#define BSON_ITER_BOOL 						bson_iterator_bool
#define BSON_ITER_DOUBLE 					bson_iterator_double
#define BSON_ITER_INT32 					bson_iterator_int
#define BSON_ITER_INT64 					bson_iterator_long
#define BSON_ITER_OID 						bson_iterator_oid
#define BSON_ITER_UTF8 						bson_iterator_string
#define BSON_ITER_REGEX 					bson_iterator_regex
#define BSON_ITER_DATE_TIME 				bson_iterator_date
#define BSON_ITER_CODE 						bson_iterator_code
#define BSON_ITER_VALUE 					bson_iterator_value
#define BSON_ITER_KEY 						bson_iterator_key
#define BSON_ITER_NEXT 						bson_iterator_next
#define BSON_ITER_TYPE 						bson_iterator_type
#define BSON_ITER_BINARY 					bson_iterator_bin_data
#endif

/* Defines for valid option names */
#define OPTION_NAME_ADDRESS					"address"
#define OPTION_NAME_PORT 					"port"
#define OPTION_NAME_DATABASE 				"database"
#define OPTION_NAME_COLLECTION 				"collection"
#define OPTION_NAME_USERNAME 				"username"
#define OPTION_NAME_PASSWORD 				"password"
#define OPTION_NAME_USE_REMOTE_ESTIMATE	    "use_remote_estimate"
#ifdef META_DRIVER
#define OPTION_NAME_READ_PREFERENCE 		"read_preference"
#define OPTION_NAME_AUTHENTICATION_DATABASE "authentication_database"
#define OPTION_NAME_REPLICA_SET 			"replica_set"
#define OPTION_NAME_SSL 					"ssl"
#define OPTION_NAME_PEM_FILE 				"pem_file"
#define OPTION_NAME_PEM_PWD 				"pem_pwd"
#define OPTION_NAME_CA_FILE 				"ca_file"
#define OPTION_NAME_CA_DIR 					"ca_dir"
#define OPTION_NAME_CRL_FILE 				"crl_file"
#define OPTION_NAME_WEAK_CERT 				"weak_cert_validation"
#define OPTION_NAME_ENABLE_JOIN_PUSHDOWN	"enable_join_pushdown"
#define OPTION_NAME_ENABLE_AGGREGATE_PUSHDOWN "enable_aggregate_pushdown"
#define OPTION_NAME_ENABLE_ORDER_BY_PUSHDOWN "enable_order_by_pushdown"
#endif

/* Default values for option parameters */
#define DEFAULT_IP_ADDRESS 					"127.0.0.1"
#define DEFAULT_PORT_NUMBER 				27017
#define DEFAULT_DATABASE_NAME 				"test"

/* Defines for sending queries and converting types */
#define EQUALITY_OPERATOR_NAME 				"="
#define INITIAL_ARRAY_CAPACITY 				8
#define MONGO_TUPLE_COST_MULTIPLIER 		5
#define MONGO_CONNECTION_COST_MULTIPLIER 	5
#define POSTGRES_TO_UNIX_EPOCH_DAYS 		(POSTGRES_EPOCH_JDATE - UNIX_EPOCH_JDATE)
#define POSTGRES_TO_UNIX_EPOCH_USECS 		(POSTGRES_TO_UNIX_EPOCH_DAYS * USECS_PER_DAY)

/* Macro for list API backporting. */
#if PG_VERSION_NUM < 130000
#define mongo_list_concat(l1, l2) list_concat(l1, list_copy(l2))
#else
#define mongo_list_concat(l1, l2) list_concat((l1), (l2))
#endif

/* Macro for hard-coded aggregation result key */
#define AGG_RESULT_KEY		 				"v_agg"

/*
 * We build a hash table that stores the column details.  However, a table can
 * have maximum MaxHeapAttributeNumber columns.  And since we allow join only
 * on two tables, we set the max hash table size to twice that limit.
 */
#define MaxHashTableSize					(MaxHeapAttributeNumber * 2)

/*
 * MongoValidOption keeps an option name and a context.  When an option is
 * passed into mongo_fdw objects (server and foreign table), we compare this
 * option's name and context against those of valid options.
 */
typedef struct MongoValidOption
{
	const char *optionName;
	Oid			optionContextId;
} MongoValidOption;

/* Array of options that are valid for mongo_fdw */
#ifdef META_DRIVER
static const uint32 ValidOptionCount = 23;
#else
static const uint32 ValidOptionCount = 7;
#endif
static const MongoValidOption ValidOptionArray[] =
{
	/* Foreign server options */
	{OPTION_NAME_ADDRESS, ForeignServerRelationId},
	{OPTION_NAME_PORT, ForeignServerRelationId},
	{OPTION_NAME_USE_REMOTE_ESTIMATE, ForeignServerRelationId},

#ifdef META_DRIVER
	{OPTION_NAME_READ_PREFERENCE, ForeignServerRelationId},
	{OPTION_NAME_AUTHENTICATION_DATABASE, ForeignServerRelationId},
	{OPTION_NAME_REPLICA_SET, ForeignServerRelationId},
	{OPTION_NAME_SSL, ForeignServerRelationId},
	{OPTION_NAME_PEM_FILE, ForeignServerRelationId},
	{OPTION_NAME_PEM_PWD, ForeignServerRelationId},
	{OPTION_NAME_CA_FILE, ForeignServerRelationId},
	{OPTION_NAME_CA_DIR, ForeignServerRelationId},
	{OPTION_NAME_CRL_FILE, ForeignServerRelationId},
	{OPTION_NAME_WEAK_CERT, ForeignServerRelationId},
	{OPTION_NAME_ENABLE_JOIN_PUSHDOWN, ForeignServerRelationId},
	{OPTION_NAME_ENABLE_AGGREGATE_PUSHDOWN, ForeignServerRelationId},
	{OPTION_NAME_ENABLE_ORDER_BY_PUSHDOWN, ForeignServerRelationId},
#endif

	/* Foreign table options */
	{OPTION_NAME_DATABASE, ForeignTableRelationId},
	{OPTION_NAME_COLLECTION, ForeignTableRelationId},
#ifdef META_DRIVER
	{OPTION_NAME_ENABLE_JOIN_PUSHDOWN, ForeignTableRelationId},
	{OPTION_NAME_ENABLE_AGGREGATE_PUSHDOWN, ForeignTableRelationId},
	{OPTION_NAME_ENABLE_ORDER_BY_PUSHDOWN, ForeignTableRelationId},
#endif

	/* User mapping options */
	{OPTION_NAME_USERNAME, UserMappingRelationId},
	{OPTION_NAME_PASSWORD, UserMappingRelationId}
};

/*
 * MongoFdwOptions holds the option values to be used when connecting to Mongo.
 * To resolve these values, we first check foreign table's options, and if not
 * present, we then fall back to the default values specified above.
 */
typedef struct MongoFdwOptions
{
	char	   *svr_address;
	uint16		svr_port;
	char	   *svr_database;
	char	   *collectionName;
	char	   *svr_username;
	char	   *svr_password;
	bool		use_remote_estimate;	/* use remote estimate for rows */
#ifdef META_DRIVER
	char	   *readPreference;
	char	   *authenticationDatabase;
	char	   *replicaSet;
	bool		ssl;
	char	   *pem_file;
	char	   *pem_pwd;
	char	   *ca_file;
	char	   *ca_dir;
	char	   *crl_file;
	bool		weak_cert_validation;
	bool		enable_join_pushdown;
	bool		enable_aggregate_pushdown;
	bool		enable_order_by_pushdown;
#endif
} MongoFdwOptions;

/*
 * MongoFdwExecState keeps foreign data wrapper specific execution state that
 * we create and hold onto when executing the query.
 *
 * Execution state of a foreign insert/update/delete operation.
 */
typedef struct MongoFdwModifyState
{
	Relation	rel;			/* relcache entry for the foreign table */
	List	   *target_attrs;	/* list of target attribute numbers */

	/* Info about parameters for prepared statement */
	int			p_nums;			/* number of parameters to transmit */
	FmgrInfo   *p_flinfo;		/* output conversion functions for them */

	struct HTAB *columnMappingHash;

	MONGO_CONN *mongoConnection;	/* MongoDB connection */
	MONGO_CURSOR *mongoCursor;	/* MongoDB cursor */
	BSON	   *queryDocument;	/* Bson Document */

	MongoFdwOptions *options;
	AttrNumber	rowidAttno;		/* attnum of resjunk rowid column */

	/* Join/Upper relation information */
	uint32		relType;		/* relation type.  Base, Join, Upper, or Upper
								 * on join */
	char	   *outerRelName;	/* Outer relation name */
} MongoFdwModifyState;

/*
 * ColumnMapping represents a hash table entry that maps a column name to
 * column-related information.  We construct these hash table entries to speed
 * up the conversion from BSON documents to PostgreSQL tuples, and each hash
 * entry maps the column name to the column's tuple index and its type-related
 * information.
 */
typedef struct ColumnMapping
{
	char		columnName[NAMEDATALEN];
	uint32		columnIndex;
	Oid			columnTypeId;
	int32		columnTypeMod;
	Oid			columnArrayTypeId;
	/* Column serial number in target list (set only for join rel) */
	uint32		columnSerialNo;
} ColumnMapping;

/*
 * FDW-specific planner information kept in RelOptInfo.fdw_private for a
 * mongo_fdw foreign table.  For a baserel, this struct is created by
 * MongoGetForeignRelSize.
 */
typedef struct MongoFdwRelationInfo
{
	/*
	 * True means that the relation can be pushed down. Always true for simple
	 * foreign scan.
	 */
	bool		pushdown_safe;

	/* baserestrictinfo clauses, broken down into safe and unsafe subsets. */
	List	   *local_conds;
	List	   *remote_conds;

	/* Name of the base rel (not set for join rels!) */
	char	   *base_relname;

	/*
	 * Name of the relation while EXPLAINing ForeignScan.  It is used for join
	 * relations but is set for all relations.  For join relation, the name
	 * indicates which foreign tables are being joined and the join type used.
	 */
	StringInfo	relation_name;

	/* True means that the query_pathkeys is safe to push down */
	bool		qp_is_pushdown_safe;

	/* Join information */
	RelOptInfo *outerrel;
	RelOptInfo *innerrel;
	JoinType	jointype;
	List	   *joinclauses;
	char	   *inner_relname;
	char	   *outer_relname;

	MongoFdwOptions *options;	/* Options applicable for this relation */

	/* Grouping information */
	List	   *grouped_tlist;
	List	   *groupbyColList;

	/* Upper relation information */
	UpperRelationKind stage;

	/*
	 * True if the underlying scan relation involved in aggregation is
	 * pushable.
	 */
	bool		is_agg_scanrel_pushable;

	/* Inherit required flags from MongoFdwOptions */
	bool		is_order_by_pushable;
} MongoFdwRelationInfo;

/*
 * MongoRelQualInfo contains column name, varno, varattno, and its relation
 * name of columns involved in the join quals which is passed to the execution
 * state through fdw_private.  For upper relation, it also includes aggregate
 * type, aggregate column name, and whether the aggregate is in target or in
 * having clause details.
 *
 * Unlike postgres_fdw, remote query formation is done in the execution state.
 * The information, mainly the varno i.e. range table index, we get at the
 * execution time is different than the planning state. That may result in
 * fetching incorrect data.  So, to avoid this, we are gathering information
 * required to form a MongoDB query in the planning state and passing it to the
 * executor.
 *
 * For join relation:
 * Assume, we have the following two tables with RTI 1 and 2 respectively:
 * 			T1(a int, b int)
 * 			T2(x int, y int)
 *
 * and if the join clause is like below with T1 as inner relation and T2 outer
 * relation:
 * 			(T1.a = T2.x AND T1.b > T2.y)
 *
 * then as columns a, b, x, and y are involved in the join clause, we need to
 * form the following 4 lists as part of MongoRelQualInfo:
 *
 * 1. colNameList: List of column names
 * 			a->x->b->y
 * 2. colNumList: List of column attribute number
 * 			1->1->2->2
 * 3. rtiList: Range table index of the column
 * 			1->2->1->2
 * 4. isOuterList: Is it a column of an outer relation?
 * 			1->0->1->0
 *
 * If we want information related to column 'a', then look for information
 * available at the zeroth index of all four lists.
 *
 * To avoid duplicate entry of columns, we use a hash table having a unique
 * hash key as a set of varno and varattno.
 *
 * For upper relation:
 * Assume, we have to calculate the sum of column 'a' and the average of column
 * 'b' of the above table 'T1' where the minimum of a is greater than 1.  This
 * can be done by the following SQL query:
 *
 * 			SELECT SUM(a), AVG(b) FROM T1 HAVING MIN(a) > 1;
 *
 * Here, there are two aggregation types SUM and MIN, and two aggregation
 * columns i.e. 'a' and 'b'.  To differentiate between two aggregation
 * operations, we need to save information about whether the aggregation
 * operation is part of a target list or having clause.  So, we need to form
 * the following three lists as a part of MongoRelQualInfo:
 *
 * 1. aggTypeList: List of aggregation operations
 * 			SUM->AVG->MIN
 * 2. aggColList: List of aggregated columns
 * 			a->b->a
 * 3. isHavingList: Is aggregation operation part of HAVING clause or not?
 * 			0->0->1
 */
typedef struct MongoRelQualInfo
{
	PlannerInfo *root;			/* global planner state */
	RelOptInfo *foreignRel;		/* the foreign relation we are planning for */
	Relids		outerRelids;	/* set of base relids of outer relation */
	List	   *colNameList;
	List	   *colNumList;
	List	   *rtiList;
	List	   *isOuterList;
	struct HTAB *exprColHash;
	/* For upper-relation */
	bool		is_agg_column;	/* is column aggregated or not? */
	bool		is_having;		/* is it part of HAVING clause or not? */
	List	   *aggTypeList;
	List	   *aggColList;
	List	   *isHavingList;
} MongoRelQualInfo;

typedef struct ColumnHashKey
{
	int			varno;
	int			varattno;
} ColumnHashKey;

/*
 * Indexes for relation type. The RelOptKind could be used but there is no
 * kind called UPPER_JOIN_REL. The UPPER_JOIN_REL is nothing but UPPER_REL but
 * for our use case, we are differentiating these two types.
 */
typedef enum MongoFdwRelType
{
	BASE_REL,
	JOIN_REL,
	UPPER_REL,
	UPPER_JOIN_REL
} MongoFdwRelType;

/* options.c */
extern MongoFdwOptions *mongo_get_options(Oid foreignTableId);
extern void mongo_free_options(MongoFdwOptions *options);
extern StringInfo mongo_option_names_string(Oid currentContextId);

/* connection.c */
MONGO_CONN *mongo_get_connection(ForeignServer *server,
								 UserMapping *user,
								 MongoFdwOptions *opt);

extern void mongo_cleanup_connection(void);
extern void mongo_release_connection(MONGO_CONN *conn);

/* Function declarations related to creating the mongo query */
extern BSON *mongo_query_document(ForeignScanState *scanStateNode);
extern List *mongo_get_column_list(PlannerInfo *root, RelOptInfo *foreignrel,
								   List *scan_var_list, List **colNameList,
								   List **colIsInnerList);
extern bool mongo_is_foreign_expr(PlannerInfo *root, RelOptInfo *baserel,
								  Expr *expression, bool is_having_cond);
extern bool mongo_is_foreign_param(PlannerInfo *root, RelOptInfo *baserel,
								   Expr *expr);

/* Function declarations for foreign data wrapper */
extern Datum mongo_fdw_handler(PG_FUNCTION_ARGS);
extern Datum mongo_fdw_validator(PG_FUNCTION_ARGS);

/* deparse.c headers */
extern void mongo_check_qual(Expr *node, MongoRelQualInfo *qual_info);
extern const char *mongo_get_jointype_name(JoinType jointype);
extern EquivalenceMember *mongo_find_em_for_rel(PlannerInfo *root,
												EquivalenceClass *ec,
												RelOptInfo *rel);
extern bool mongo_is_builtin(Oid oid);
extern bool mongo_is_default_sort_operator(EquivalenceMember *em,
										   PathKey *pathkey);
extern bool mongo_is_foreign_pathkey(PlannerInfo *root, RelOptInfo *baserel,
									 PathKey *pathkey);

#endif							/* MONGO_FDW_H */
