/*-------------------------------------------------------------------------
 *
 * mongo_query.h
 * 		FDW query handling for mongo_fdw
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2024, EnterpriseDB Corporation.
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		mongo_query.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MONGO_QUERY_H
#define MONGO_QUERY_H

#define NUMERICARRAY_OID 1231

/*
 * Context for aggregation pipeline formation.
 */
typedef struct pipeline_cxt
{
	struct HTAB *colInfoHash;	/* columns information hash */
	unsigned int arrayIndex;	/* Index of the various arrays in the
								 * pipeline, starting from zero */
	bool		isBoolExpr;		/* is join expression boolean? */
	bool		isJoinClause;	/* is join clause? This is to add null check
								 * only in case of join clause */
	uint32		opExprCount;	/* count death of the expression */
	ForeignScanState *scanStateNode;	/* To evaluate param expression */
} pipeline_cxt;

/*
 * ColInfoEntry represents a hash table entry that maps a unique column's varno
 * and varattno to the column name and related information.  We construct these
 * hash table entries to speed up the BSON query document formation.
 */
typedef struct ColInfoHashKey
{
	int			varNo;
	int			varAttno;
} ColInfoHashKey;
typedef struct ColInfoEntry
{
	ColInfoHashKey key;			/* Hash key */
	char	   *colName;
	bool		isOuter;
} ColInfoHashEntry;

/*
 * Indexes of FDW-private information stored in fdw_private lists.
 *
 * These items are indexed with the enum mongoFdwScanPrivateIndex, so an item
 * can be fetched with list_nth().  For example, to get the column list:
 *		col_list = strVal(list_nth(fdw_private, mongoFdwPrivateColumnList));
 */
enum mongoFdwScanPrivateIndex
{
	/*
	 * Column list to form column mapping hash i.e. to get only needed columns
	 * from all fetched columns from remote.
	 */
	mongoFdwPrivateColumnList,

	/* Expressions to execute remotely */
	mongoFdwPrivateRemoteExprList,

	/* Relation Type (BASE/JOIN/UPPER/UPPER_JOIN) */
	mongoFdwPrivateRelType,

	/*
	 * List of column name, attribute number, range table index, and whether
	 * this column is of outer relation or not.
	 *
	 * The columns which are part of the join clauses are listed.
	 */
	mongoFdwPrivateJoinClauseColNameList,
	mongoFdwPrivareJoinClauseColNumList,
	mongoFdwPrivateJoinClauseRtiList,
	mongoFdwPrivateJoinClauseIsOuterList,

	/* ORDER BY clause information */
	mongoFdwPrivatePathKeyList,
	mongoFdwPrivateIsAscSortList,

	/* LIMIT/OFFSET clause information */
	mongoFdwPrivateHasLimitClause,
	mongoFdwPrivateLimitCountList,
	mongoFdwPrivateLimitOffsetList,

	/* Upper relation information */

	/* Upper relation grouping operation name list */
	mongoFdwPrivateAggType,

	/* List of column names involved in grouping operation list */
	mongoFdwPrivateAggColList,

	/* GROUP BY clause expression */
	mongoFdwPrivateGroupByColList,

	/* Having expression */
	mongoFdwPrivateHavingExpr,

	/* Is the grouping expression part of HAVING expression or not?  */
	mongoFdwPrivateIsHavingList,

	/*
	 * String describing join i.e. names of relations being joined and types
	 * of join, added when the scan is join
	 */
	mongoFdwPrivateRelations,

	/*
	 * List of column names and whether those are part of inner or outer
	 * relation stored to form Column Mapping Hash.  These are needed column
	 * means those are part of target and restriction columns.
	 */
	mongoFdwPrivateColNameList,
	mongoFdwPrivateColIsInnerList,

	/* Inner and Outer relation names */
	mongoFdwPrivateJoinInnerOuterRelName,

	/* List of join clauses to form a pipeline */
	mongoFdwPrivateJoinClauseList,

	/* Join-type  */
	mongoFdwPrivateJoinType
};

/* Function to be used in mongo_fdw.c */
extern bool append_mongo_value(BSON *queryDocument, const char *keyName,
							   Datum value, bool isnull, Oid id);

/* Functions to be used in deparse.c */
extern char *mongo_operator_name(const char *operatorName);
extern void append_constant_value(BSON *queryDocument, const char *keyName,
								  Const *constant);
extern void mongo_append_expr(Expr *node, BSON *child_doc,
							  pipeline_cxt *context);
extern void append_param_value(BSON *queryDocument, const char *keyName,
							   Param *paramNode,
							   ForeignScanState *scanStateNode);

#endif							/* MONGO_QUERY_H */
