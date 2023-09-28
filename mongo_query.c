/*-------------------------------------------------------------------------
 *
 * mongo_query.c
 * 		FDW query handling for mongo_fdw
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2023, EnterpriseDB Corporation.
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		mongo_query.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "mongo_wrapper.h"

#include <bson.h>
#include <json.h>

#if PG_VERSION_NUM < 120000
#include "access/sysattr.h"
#endif
#include "access/htup_details.h"
#if PG_VERSION_NUM >= 120000
#include "access/table.h"
#endif
#include "catalog/heap.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_operator.h"
#if PG_VERSION_NUM >= 130000
#include "common/hashfn.h"
#endif
#ifdef META_DRIVER
#include "mongoc.h"
#else
#include "mongo.h"
#endif
#include "mongo_query.h"
#if PG_VERSION_NUM < 120000
#include "nodes/relation.h"
#include "optimizer/var.h"
#endif
#if PG_VERSION_NUM >= 120000
#include "optimizer/optimizer.h"
#endif
#include "parser/parsetree.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * Global context for foreign_expr_walker's search of an expression tree.
 */
typedef struct foreign_glob_cxt
{
	PlannerInfo *root;			/* global planner state */
	RelOptInfo *foreignrel;		/* the foreign relation we are planning for */
#ifndef META_DRIVER
	unsigned short varcount;	/* Var count */
	unsigned short opexprcount;
#endif
	Relids		relids;			/* relids of base relations in the underlying
								 * scan */
	bool		is_having_cond; /* "true" for HAVING clause condition */
} foreign_glob_cxt;

/*
 * Local (per-tree-level) context for foreign_expr_walker's search.
 * This is concerned with identifying collations used in the expression.
 */
typedef enum
{
	FDW_COLLATE_NONE,			/* expression is of a noncollatable type */
	FDW_COLLATE_SAFE,			/* collation derives from a foreign Var */
	FDW_COLLATE_UNSAFE			/* collation derives from something else */
} FDWCollateState;

typedef struct foreign_loc_cxt
{
	Oid			collation;		/* OID of current collation, if any */
	FDWCollateState state;		/* state of current collation choice */
} foreign_loc_cxt;

/* Local functions forward declarations */
#ifndef META_DRIVER
static Expr *find_argument_of_type(List *argumentList, NodeTag argumentType);
static List *equality_operator_list(List *operatorList);
static List *unique_column_list(List *operatorList);
static List *column_operator_list(Var *column, List *operatorList);
#endif
static bool foreign_expr_walker(Node *node,
								foreign_glob_cxt *glob_cxt,
								foreign_loc_cxt *outer_cxt);
static List *prepare_var_list_for_baserel(Oid relid, Index varno,
										  Bitmapset *attrs_used);
#ifdef META_DRIVER
static HTAB *column_info_hash(List *colname_list, List *colnum_list,
							  List *rti_list, List *isouter_list);
static void mongo_prepare_pipeline(List *clause, BSON *inner_pipeline,
								   pipeline_cxt *context);
static void mongo_append_clauses_to_pipeline(List *clause, BSON *child_doc,
											 pipeline_cxt *context);
#endif

#if PG_VERSION_NUM >= 160000
static List *mongo_append_unique_var(List *varlist, Var *var);
#endif

#ifndef META_DRIVER
/*
 * find_argument_of_type
 *		Walks over the given argument list, looks for an argument with the
 *		given type, and returns the argument if it is found.
 */
static Expr *
find_argument_of_type(List *argumentList, NodeTag argumentType)
{
	Expr	   *foundArgument = NULL;
	ListCell   *argumentCell;

	foreach(argumentCell, argumentList)
	{
		Expr	   *argument = (Expr *) lfirst(argumentCell);

		/* For RelabelType type, examine the inner node */
		if (IsA(argument, RelabelType))
			argument = ((RelabelType *) argument)->arg;

		if (nodeTag(argument) == argumentType)
		{
			foundArgument = argument;
			break;
		}
	}

	return foundArgument;
}
#endif

/*
 * mongo_query_document
 *		Takes in the applicable operator expressions for relation, the join
 *		clauses for join relation, and grouping targets for upper relation and
 *		converts these expressions, join clauses, and grouping targets into
 *		equivalent queries in MongoDB.
 *
 * For join clauses, transforms simple comparison expressions along with a
 * comparison between two vars and nested operator expressions as well.
 *
 * Example: Consider the following two foreign tables:
 *    t1(_id NAME, age INT, name VARCHAR)
 *    t2(_id NAME, old INT, alias VARCHAR)
 *
 * SQL query:
 *    SELECT * FROM t1 LEFT JOIN t2 ON (t1.age = t2.old)
 *    WHERE (t1.age % 2) = 1
 *    ORDER BY t1.age ASC NULLS FIRST;

 * Equivalent MongoDB query:
 *
 *    db.t1.aggregate([
 *      {
 *        "$lookup":
 *        {
 *          "from": "t2",
 *          "let": { "v_age": "$age" },
 *          "pipeline": [
 *            {
 *              "$match":
 *              {
 *                "$expr":
 *                {
 *                  "$and": [
 *                    { "$eq": [ "$$v_age", "$old" ] }
 *                    { "$ne": [ "$$v_age", null ] },
 *                    { "$ne": [ "$old", null ] },
 *                  ]
 *                }
 *              }
 *            }
 *          ],
 *          "as": "Join_Result"
 *        }
 *      },
 *      { "$match" :
 *        {
 *          "$expr" :
 *          { "$and" : [
 *              { "$eq" : [ { "$mod" : [ "$age", 2] }, 1]},
 *              { "$ne" : [ "$age", null ] }
 *            ]
 *          }
 *        }
 *      }
 *      {
 *        "$unwind":
 *        {
 *          "path": "$Join_Result",
 *          "preserveNullAndEmptyArrays": true
 *        }
 *      },
 *		{ "$sort": { "age" : 1 } }
 *    ])
 *
 * Any MongoDB query would have the following three main arrays:
 *  1. Root pipeline array (first square bracket):
 *  	This has three elements called $lookup, $unwind, and $match stages.
 *  2. Inner pipeline array (starting with "pipeline" keyword above):
 *  	It has one element that is $match.
 *  3. "$and" expression inside inner pipeline:
 *  	These elements depend on the join clauses available.
 *
 * The outer $match stage (2nd element of root pipeline array) represents
 * remote_exprs, and $match inside $lookup stage represents the join clauses.
 *
 * For grouping target, add $group stage on the base relation or join relation.
 * The HAVING clause is nothing but a post $match stage.
 *
 * Example: Consider above table t1:
 *
 * SQL query:
 *    SELECT name, SUM(age) FROM t1 GROUP BY name HAVING MIN(name) = 'xyz'
 *      ORDER BY name DESC NULLS LAST;
 *
 * Equivalent MongoDB query:
 *
 *     db.t1.aggregate([
 *       {
 *         "$group":
 *         {
 *           "_id": {"name": "$name"},
 *           "v_agg0": {"$sum": "$age"},
 *           "v_having": {"$min": "$name"}
 *         }
 *       },
 *       {
 *         "$match": {"v_having": "xyz"}
 *       }
 *		 { "$sort": { "name" : -1 } }
 *     ])
 *
 * For ORDER BY, add $sort stage on the base relation or join or grouping
 * relation as shown in the above examples of join and grouping relations.
 */
BSON *
mongo_query_document(ForeignScanState *scanStateNode)
{
	ForeignScan *fsplan = (ForeignScan *) scanStateNode->ss.ps.plan;
	BSON	   *queryDocument = bsonCreate();
	BSON	   *filter = bsonCreate();
	List	   *PrivateList = fsplan->fdw_private;
	List	   *opExpressionList = list_nth(PrivateList,
											mongoFdwPrivateRemoteExprList);
#ifdef META_DRIVER
	MongoFdwModifyState *fmstate = (MongoFdwModifyState *) scanStateNode->fdw_state;
	BSON		root_pipeline;
	BSON		match_stage;
	int			root_index = 0;
	List	   *joinclauses;
	List	   *colnum_list;
	List	   *colname_list = NIL;
	List	   *isouter_list = NIL;
	List	   *rti_list;
	List	   *pathkey_list;
	List	   *is_ascsort_list;
	char	   *inner_relname;
	char	   *outer_relname;
	HTAB	   *columnInfoHash;
	int			jointype;
	int			natts;
	bool		has_limit;

	/* Retrieve data passed by planning phase */
	colname_list = list_nth(PrivateList, mongoFdwPrivateJoinClauseColNameList);
	colnum_list = list_nth(PrivateList, mongoFdwPrivareJoinClauseColNumList);
	rti_list = list_nth(PrivateList, mongoFdwPrivateJoinClauseRtiList);
	isouter_list = list_nth(PrivateList, mongoFdwPrivateJoinClauseIsOuterList);

	/* Length should be same for all lists of column information */
	natts = list_length(colname_list);
	Assert(natts == list_length(colnum_list) && natts == list_length(rti_list)
		   && natts == list_length(isouter_list));

	/* Store information in the hash-table */
	columnInfoHash = column_info_hash(colname_list, colnum_list, rti_list,
									  isouter_list);

	/* Retrieve information related to ORDER BY clause */
	pathkey_list = list_nth(PrivateList, mongoFdwPrivatePathKeyList);
	is_ascsort_list = list_nth(PrivateList, mongoFdwPrivateIsAscSortList);

	/* Retrieve information related to LIMIT/OFFSET clause */
	has_limit = intVal(list_nth(PrivateList, mongoFdwPrivateHasLimitClause));

	if (fmstate->relType == JOIN_REL || fmstate->relType == UPPER_JOIN_REL)
	{
		List	   *innerouter_relname;

		joinclauses = list_nth(PrivateList, mongoFdwPrivateJoinClauseList);
		if (joinclauses)
			jointype = intVal(list_nth(PrivateList, mongoFdwPrivateJoinType));

		innerouter_relname = list_nth(PrivateList,
									  mongoFdwPrivateJoinInnerOuterRelName);
		inner_relname = strVal(list_nth(innerouter_relname, 0));
		outer_relname = strVal(list_nth(innerouter_relname, 1));
	}

	/* Prepare array of stages */
	bsonAppendStartArray(queryDocument, "pipeline", &root_pipeline);
#endif

	/*
	 * Add filter into query pipeline if available.  These are remote_exprs
	 * i.e. clauses available in WHERE and those are push-able to the remote
	 * side.
	 */
	if (opExpressionList)
	{
#ifdef META_DRIVER
		pipeline_cxt context;

		context.colInfoHash = columnInfoHash;
		context.isBoolExpr = false;
		context.isJoinClause = false;
		context.scanStateNode = scanStateNode;

		bsonAppendStartArray(filter, "pipeline", &match_stage);

		/* Form equivalent WHERE clauses in MongoDB */
		mongo_prepare_pipeline(opExpressionList, &match_stage, &context);

		bsonAppendFinishArray(filter, &match_stage);
#else
		Oid			relationId;
		List	   *equalityOperatorList;
		List	   *comparisonOperatorList;
		List	   *columnList;
		ListCell   *equalityOperatorCell;
		ListCell   *columnCell;

		if (fsplan->scan.scanrelid > 0)
			relationId = RelationGetRelid(scanStateNode->ss.ss_currentRelation);
		else
			relationId = 0;

		/*
		 * We distinguish between equality expressions and others since we
		 * need to insert the latter (<, >, <=, >=, <>) as separate
		 * sub-documents into the BSON query object.
		 */
		equalityOperatorList = equality_operator_list(opExpressionList);
		comparisonOperatorList = list_difference(opExpressionList,
												 equalityOperatorList);

		/* Append equality expressions to the query */
		foreach(equalityOperatorCell, equalityOperatorList)
		{
			OpExpr	   *equalityOperator;
			Oid			columnId = InvalidOid;
			char	   *columnName;
			Const	   *constant;
			Param	   *paramNode;
			List	   *argumentList;
			Var		   *column;

			equalityOperator = (OpExpr *) lfirst(equalityOperatorCell);
			argumentList = equalityOperator->args;
			column = (Var *) find_argument_of_type(argumentList, T_Var);
			constant = (Const *) find_argument_of_type(argumentList, T_Const);
			paramNode = (Param *) find_argument_of_type(argumentList, T_Param);

			columnId = column->varattno;
			columnName = get_attname(relationId, columnId, false);

			if (constant != NULL)
				append_constant_value(filter, columnName, constant);
			else
				append_param_value(filter, columnName, paramNode,
								   scanStateNode);
		}

		/*
		 * For comparison expressions, we need to group them by their columns
		 * and then append all expressions that correspond to a column as one
		 * sub-document.  Otherwise, even when we have two expressions to
		 * define the upper and lower bound of a range, Mongo uses only one of
		 * these expressions during an index search.
		 */
		columnList = unique_column_list(comparisonOperatorList);

		/* Append comparison expressions, grouped by columns, to the query */
		foreach(columnCell, columnList)
		{
			Var		   *column = (Var *) lfirst(columnCell);
			Oid			columnId = InvalidOid;
			char	   *columnName;
			List	   *columnOperatorList;
			ListCell   *columnOperatorCell;
			BSON		childDocument;

			if (relationId != 0)
			{
				columnId = column->varattno;
				columnName = get_attname(relationId, columnId, false);
			}

			/* Find all expressions that correspond to the column */
			columnOperatorList = column_operator_list(column,
													  comparisonOperatorList);

			/* For comparison expressions, start a sub-document */
			bsonAppendStartObject(filter, columnName, &childDocument);

			foreach(columnOperatorCell, columnOperatorList)
			{
				OpExpr	   *columnOperator;
				char	   *operatorName;
				char	   *mongoOperatorName;
				List	   *argumentList;
				Const	   *constant;
				Param	   *paramNode;

				columnOperator = (OpExpr *) lfirst(columnOperatorCell);
				argumentList = columnOperator->args;
				constant = (Const *) find_argument_of_type(argumentList,
														   T_Const);
				paramNode = (Param *) find_argument_of_type(argumentList,
															T_Param);
				operatorName = get_opname(columnOperator->opno);
				mongoOperatorName = mongo_operator_name(operatorName);

				if (constant != NULL)
					append_constant_value(filter, mongoOperatorName, constant);
				else
					append_param_value(filter, mongoOperatorName, paramNode,
									   scanStateNode);
			}
			bsonAppendFinishObject(filter, &childDocument);
		}
#endif
	}
	if (!bsonFinish(filter))
	{
#ifdef META_DRIVER
		ereport(ERROR,
				(errmsg("could not create document for query"),
				 errhint("BSON flags: %d", queryDocument->flags)));
#else
		ereport(ERROR,
				(errmsg("could not create document for query"),
				 errhint("BSON error: %d", queryDocument->err)));
#endif
	}

#ifdef META_DRIVER
	if (fmstate->relType == JOIN_REL || fmstate->relType == UPPER_JOIN_REL)
	{
		BSON		inner_pipeline;
		BSON		lookup_object;
		BSON		lookup;
		BSON		let_exprs;
		BSON		unwind_stage;
		BSON		unwind;
		BSON	   *inner_pipeline_doc = bsonCreate();
		ListCell   *cell1;
		ListCell   *cell2;

		/* $lookup stage. This is to perform JOIN */
		bsonAppendStartObject(&root_pipeline, psprintf("%d", root_index++),
							  &lookup_object);
		bsonAppendStartObject(&lookup_object, "$lookup", &lookup);
		bsonAppendUTF8(&lookup, "from", inner_relname);

		/*
		 * Start "let" operator: Specifies variables to use in the pipeline
		 * stages.  To access columns of outer relation, those need to be
		 * defined in terms of a variable using "let".
		 */
		bsonAppendStartObject(&lookup, "let", &let_exprs);
		forboth(cell1, colname_list, cell2, isouter_list)
		{
			char	   *colname = strVal(lfirst(cell1));
			bool		is_outer = lfirst_int(cell2);

			/*
			 * Ignore column name with "*" because this is not the name of any
			 * particular column and is not allowed in the let operator. While
			 * deparsing the COUNT(*) aggregation operation, this column name
			 * is added to lists to maintain the length of column information.
			 */
			if (is_outer && strcmp(colname, "*") != 0)
			{
				/*
				 * Add prefix "v_" to column name to form variable name.  Need
				 * to prefix with any lowercase letter because variable names
				 * must begin with only a lowercase ASCII letter or a
				 * non-ASCII character.
				 */
				char	   *varname = psprintf("v_%s", colname);
				char	   *field = psprintf("$%s", colname);

				bsonAppendUTF8(&let_exprs, varname, field);
			}
		}
		bsonAppendFinishObject(&lookup, &let_exprs);	/* End "let" */

		/* Form inner pipeline required in $lookup stage to execute $match */
		bsonAppendStartArray(inner_pipeline_doc, "pipeline", &inner_pipeline);
		if (joinclauses)
		{
			pipeline_cxt context;

			context.colInfoHash = columnInfoHash;
			context.isBoolExpr = false;
			context.isJoinClause = true;
			context.scanStateNode = scanStateNode;

			/* Form equivalent join qual clauses in MongoDB */
			mongo_prepare_pipeline(joinclauses, &inner_pipeline, &context);
			bsonAppendFinishArray(inner_pipeline_doc, &inner_pipeline);
		}

		/* Append inner pipeline to $lookup stage */
		bson_append_array(&lookup, "pipeline", (int) strlen("pipeline"),
						  &inner_pipeline);

		bsonAppendUTF8(&lookup, "as", "Join_Result");
		bsonAppendFinishObject(&lookup_object, &lookup);
		bsonAppendFinishObject(&root_pipeline, &lookup_object);

		/* $match stage. This is to add a filter */
		if (opExpressionList)
			bsonAppendBson(&root_pipeline, "$match", &match_stage);

		/*
		 * $unwind stage. This deconstructs an array field from the input
		 * documents to output a document for each element.
		 */
		bsonAppendStartObject(&root_pipeline, psprintf("%d", root_index++),
							  &unwind_stage);
		bsonAppendStartObject(&unwind_stage, "$unwind", &unwind);
		bsonAppendUTF8(&unwind, "path", "$Join_Result");
		if (jointype == JOIN_INNER)
			bsonAppendBool(&unwind, "preserveNullAndEmptyArrays", false);
		else
			bsonAppendBool(&unwind, "preserveNullAndEmptyArrays", true);
		bsonAppendFinishObject(&unwind_stage, &unwind);
		bsonAppendFinishObject(&root_pipeline, &unwind_stage);

		fmstate->outerRelName = outer_relname;
	}
	else if (opExpressionList)
		bsonAppendBson(&root_pipeline, "$match", &match_stage);

	/* Add $group stage for upper relation */
	if (fmstate->relType == UPPER_JOIN_REL || fmstate->relType == UPPER_REL)
	{
		List	   *func_list;
		List	   *agg_col_list;
		List	   *groupby_col_list;
		List	   *having_expr;
		BSON		groupby_expr;
		BSON		group_stage;
		BSON		group_expr;
		BSON		group;
		ListCell   *cell1;
		ListCell   *cell2;
		ListCell   *cell3;
		List	   *is_having_list;
		Index		aggIndex = 0;

		func_list = list_nth(PrivateList, mongoFdwPrivateAggType);
		agg_col_list = list_nth(PrivateList, mongoFdwPrivateAggColList);
		groupby_col_list = list_nth(PrivateList, mongoFdwPrivateGroupByColList);
		having_expr = list_nth(PrivateList, mongoFdwPrivateHavingExpr);
		is_having_list = list_nth(PrivateList, mongoFdwPrivateIsHavingList);

		/* $group stage. */
		bsonAppendStartObject(&root_pipeline, psprintf("%d", root_index++),
							  &group_stage);
		bsonAppendStartObject(&group_stage, "$group", &group);

		/*
		 * Add columns from the GROUP BY clause in the "_id" field of $group
		 * stage.  In case of aggregation on join result, a column of the
		 * inner table needs to be accessed by prefixing it using
		 * "Join_Result", which is been hardcoded.
		 */
		if (groupby_col_list)
		{
			ListCell   *columnCell;

			bsonAppendStartObject(&group, "_id", &groupby_expr);
			foreach(columnCell, groupby_col_list)
			{
				Var		   *column = (Var *) lfirst(columnCell);
				bool		found = false;
				ColInfoHashKey key;
				ColInfoHashEntry *columnInfo;

				key.varNo = column->varno;
				key.varAttno = column->varattno;

				columnInfo = (ColInfoHashEntry *) hash_search(columnInfoHash,
															  (void *) &key,
															  HASH_FIND,
															  &found);
				if (found)
				{
					if (columnInfo->isOuter)
						bsonAppendUTF8(&groupby_expr, columnInfo->colName,
									   psprintf("$%s", columnInfo->colName));
					else
						bsonAppendUTF8(&groupby_expr, columnInfo->colName,
									   psprintf("$Join_Result.%s",
												columnInfo->colName));
				}
			}
			bsonAppendFinishObject(&group, &groupby_expr);	/* End "_id" */
		}
		else
		{
			/* If no GROUP BY clause then append null to the _id. */
			bsonAppendNull(&group, "_id");
		}

		/* Add grouping operation */
		forthree(cell1, func_list, cell2, agg_col_list, cell3, is_having_list)
		{
			ColInfoHashKey key;
			ColInfoHashEntry *columnInfo;
			bool		found = false;
			char	   *func_name = strVal(lfirst(cell1));
			Var		   *column = (Var *) lfirst(cell2);
			bool		is_having_agg = lfirst_int(cell3);

			if (is_having_agg)
				bsonAppendStartObject(&group, "v_having", &group_expr);
			else
				bsonAppendStartObject(&group,
									  psprintf("AGG_RESULT_KEY%d",
											   aggIndex++),
									  &group_expr);

			key.varNo = column->varno;
			key.varAttno = column->varattno;

			columnInfo = (ColInfoHashEntry *) hash_search(columnInfoHash,
														  (void *) &key,
														  HASH_FIND,
														  &found);

			/*
			 * The aggregation operation in MongoDB other than COUNT has the
			 * same name as PostgreSQL but COUNT needs to be performed using
			 * the $sum operator because MongoDB doesn't have a direct $count
			 * operator for the currently supported version (i.e. v4.4).
			 *
			 * There is no syntax in MongoDB to provide column names for COUNT
			 * operation but for other supported operations, we can do so.
			 *
			 * In case of aggregation over the join, the resulted columns of
			 * inner relation need to be accessed by prefixing it with
			 * "Join_Result".
			 */
			if (found && strcmp(func_name, "count") != 0)
			{
				if (columnInfo->isOuter)
					bsonAppendUTF8(&group_expr, psprintf("$%s", func_name),
								   psprintf("$%s", columnInfo->colName));
				else
					bsonAppendUTF8(&group_expr, psprintf("$%s", func_name),
								   psprintf("$Join_Result.%s",
											columnInfo->colName));
			}
			else
			{
				/*
				 * The COUNT(*) in PostgreSQL is equivalent to {$sum: 1} in
				 * the MongoDB.
				 */
				bsonAppendInt32(&group_expr, psprintf("$%s", "sum"), 1);
			}

			bsonAppendFinishObject(&group, &group_expr);
		}

		bsonAppendFinishObject(&group_stage, &group);
		bsonAppendFinishObject(&root_pipeline, &group_stage);

		/* Add HAVING operation */
		if (having_expr)
		{
			pipeline_cxt context;

			context.colInfoHash = columnInfoHash;
			context.isBoolExpr = false;
			context.isJoinClause = false;
			context.scanStateNode = scanStateNode;

			/* $match stage.  Add a filter for the HAVING clause */
			bsonAppendStartObject(&root_pipeline, psprintf("%d", root_index++),
								  &match_stage);
			/* Form equivalent HAVING clauses in MongoDB */
			mongo_prepare_pipeline(having_expr, &match_stage, &context);

			bsonAppendFinishObject(&root_pipeline, &match_stage);

			if (!bsonFinish(filter))
				ereport(ERROR,
						(errmsg("could not create document for query"),
						 errhint("BSON flags: %d", queryDocument->flags)));
		}
	}

	/* Add sort stage */
	if (pathkey_list)
	{
		BSON		sort_stage;
		BSON		sort;
		ListCell   *cell1;
		ListCell   *cell2;

		bsonAppendStartObject(&root_pipeline, psprintf("%d", root_index++),
							  &sort_stage);
		bsonAppendStartObject(&sort_stage, "$sort", &sort);

		forboth(cell1, pathkey_list, cell2, is_ascsort_list)
		{
			Var		   *column = (Var *) lfirst(cell1);
			int			is_asc_sort = lfirst_int(cell2);
			bool		found = false;
			ColInfoHashKey key;
			ColInfoHashEntry *columnInfo;

			/* Find column name */
			key.varNo = column->varno;
			key.varAttno = column->varattno;

			columnInfo = (ColInfoHashEntry *) hash_search(columnInfoHash,
														  (void *) &key,
														  HASH_FIND,
														  &found);
			if (found)
			{
				/*
				 * In the case of upper rel, access the column by prefixing it
				 * with "_id".  To access the column of the inner relation in
				 * the join operation, use the prefix "Join_result" because
				 * direct access is not possible.  However, columns of the
				 * simple relation and outer relation of the join can be
				 * accessed directly.
				 */
				if (fmstate->relType == UPPER_JOIN_REL ||
					fmstate->relType == UPPER_REL)
					bsonAppendInt32(&sort,
									psprintf("_id.%s", columnInfo->colName),
									is_asc_sort);
				else if (!columnInfo->isOuter && fmstate->relType != BASE_REL)
					bsonAppendInt32(&sort,
									psprintf("Join_result.%s",
											 columnInfo->colName),
									is_asc_sort);
				else
					bsonAppendInt32(&sort, columnInfo->colName, is_asc_sort);
			}
		}
		bsonAppendFinishObject(&sort_stage, &sort);
		bsonAppendFinishObject(&root_pipeline, &sort_stage);	/* End sort */
	}

	/* Add LIMIT/SKIP stage */
	if (has_limit)
	{
		int64		limit_value;
		int64		offset_value;

		/*
		 * Add skip stage for OFFSET clause.  However, don't add the same if
		 * either offset is not provided or the offset value is zero.
		 */
		offset_value = (int64) intVal(list_nth(PrivateList,
											   mongoFdwPrivateLimitOffsetList));
		if (offset_value != -1 && offset_value != 0)
		{
			BSON		skip_stage;

			bsonAppendStartObject(&root_pipeline, psprintf("%d", root_index++),
								  &skip_stage);
			bsonAppendInt64(&skip_stage, "$skip", offset_value);
			bsonAppendFinishObject(&root_pipeline, &skip_stage);
		}

		/*
		 * Add limit stage for LIMIT clause.  However, don't add the same if
		 * the limit is not provided.
		 */
		limit_value = (int64) intVal(list_nth(PrivateList,
											  mongoFdwPrivateLimitCountList));

		if (limit_value != -1)
		{
			BSON		limit_stage;

			bsonAppendStartObject(&root_pipeline, psprintf("%d", root_index++),
								  &limit_stage);
			bsonAppendInt64(&limit_stage, "$limit", limit_value);
			bsonAppendFinishObject(&root_pipeline, &limit_stage);
		}
	}

	bsonAppendFinishArray(queryDocument, &root_pipeline);

	if (!bsonFinish(queryDocument))
	{
		ereport(ERROR,
				(errmsg("could not create document for query"),
				 errhint("BSON flags: %d", queryDocument->flags)));
	}

	return queryDocument;
#endif

	return filter;
}

/*
 * mongo_operator_name
 * 		Takes in the given PostgreSQL comparison operator name, and returns its
 * 		equivalent in MongoDB.
 */
char *
mongo_operator_name(const char *operatorName)
{
	const char *mongoOperatorName = NULL;
	const int32 nameCount = 14;
	static const char *nameMappings[][2] = {{"<", "$lt"},
	{">", "$gt"},
	{"<=", "$lte"},
	{">=", "$gte"},
	{"<>", "$ne"},
	{"=", "$eq"},
	{"+", "$add"},
	{"-", "$subtract"},
	{"*", "$multiply"},
	{"/", "$divide"},
	{"%", "$mod"},
	{"^", "$pow"},
	{"|/", "$sqrt"},
	{"@", "$abs"}};
	int32		nameIndex;

	for (nameIndex = 0; nameIndex < nameCount; nameIndex++)
	{
		const char *pgOperatorName = nameMappings[nameIndex][0];

		if (strncmp(pgOperatorName, operatorName, NAMEDATALEN) == 0)
		{
			mongoOperatorName = nameMappings[nameIndex][1];
			break;
		}
	}

	return (char *) mongoOperatorName;
}

#ifndef META_DRIVER
/*
 * equality_operator_list
 *		Finds the equality (=) operators in the given list, and returns these
 *		operators in a new list.
 */
static List *
equality_operator_list(List *operatorList)
{
	List	   *equalityOperatorList = NIL;
	ListCell   *operatorCell;

	foreach(operatorCell, operatorList)
	{
		OpExpr	   *operator = (OpExpr *) lfirst(operatorCell);

		if (strncmp(get_opname(operator->opno), EQUALITY_OPERATOR_NAME,
					NAMEDATALEN) == 0)
			equalityOperatorList = lappend(equalityOperatorList, operator);
	}

	return equalityOperatorList;
}

/*
 * unique_column_list
 *		Walks over the given operator list, and extracts the column argument in
 *		each operator.
 *
 * The function then de-duplicates extracted columns, and returns them in a new
 * list.
 */
static List *
unique_column_list(List *operatorList)
{
	List	   *uniqueColumnList = NIL;
	ListCell   *operatorCell;

	foreach(operatorCell, operatorList)
	{
		OpExpr	   *operator = (OpExpr *) lfirst(operatorCell);
		List	   *argumentList = operator->args;
		Var		   *column = (Var *) find_argument_of_type(argumentList,
														   T_Var);

#if PG_VERSION_NUM >= 160000
		uniqueColumnList = mongo_append_unique_var(uniqueColumnList, column);
#else
		/* List membership is determined via column's equal() function */
		uniqueColumnList = list_append_unique(uniqueColumnList, column);
#endif
	}

	return uniqueColumnList;
}

/*
 * column_operator_list
 *		Finds all expressions that correspond to the given column, and returns
 *		them in a new list.
 */
static List *
column_operator_list(Var *column, List *operatorList)
{
	List	   *columnOperatorList = NIL;
	ListCell   *operatorCell;

	foreach(operatorCell, operatorList)
	{
		OpExpr	   *operator = (OpExpr *) lfirst(operatorCell);
		List	   *argumentList = operator->args;
		Var		   *foundColumn = (Var *) find_argument_of_type(argumentList,
																T_Var);

		if (equal(column, foundColumn))
			columnOperatorList = lappend(columnOperatorList, operator);
	}

	return columnOperatorList;
}
#endif

void
append_param_value(BSON *queryDocument, const char *keyName, Param *paramNode,
				   ForeignScanState *scanStateNode)
{
	ExprState  *param_expr;
	Datum		param_value;
	bool		isNull;
	ExprContext *econtext;

	if (scanStateNode == NULL)
		return;

	econtext = scanStateNode->ss.ps.ps_ExprContext;

	/* Prepare for parameter expression evaluation */
	param_expr = ExecInitExpr((Expr *) paramNode, (PlanState *) scanStateNode);

	/* Evaluate the parameter expression */
	param_value = ExecEvalExpr(param_expr, econtext, &isNull);

	append_mongo_value(queryDocument, keyName, param_value, isNull,
					   paramNode->paramtype);
}

/*
 * append_constant_value
 *		Appends to the query document the key name and constant value.
 *
 * The function translates the constant value from its PostgreSQL type
 * to its MongoDB equivalent.
 */
void
append_constant_value(BSON *queryDocument, const char *keyName, Const *constant)
{
	if (constant->constisnull)
	{
		bsonAppendNull(queryDocument, keyName);
		return;
	}

	append_mongo_value(queryDocument, keyName, constant->constvalue, false,
					   constant->consttype);
}

bool
append_mongo_value(BSON *queryDocument, const char *keyName, Datum value,
				   bool isnull, Oid id)
{
	bool		status = false;

	if (isnull)
	{
		status = bsonAppendNull(queryDocument, keyName);
		return status;
	}

	switch (id)
	{
		case INT2OID:
			{
				int16		valueInt = DatumGetInt16(value);

				status = bsonAppendInt32(queryDocument, keyName,
										 (int) valueInt);
			}
			break;
		case INT4OID:
			{
				int32		valueInt = DatumGetInt32(value);

				status = bsonAppendInt32(queryDocument, keyName, valueInt);
			}
			break;
		case INT8OID:
			{
				int64		valueLong = DatumGetInt64(value);

				status = bsonAppendInt64(queryDocument, keyName, valueLong);
			}
			break;
		case FLOAT4OID:
			{
				float4		valueFloat = DatumGetFloat4(value);

				status = bsonAppendDouble(queryDocument, keyName,
										  (double) valueFloat);
			}
			break;
		case FLOAT8OID:
			{
				float8		valueFloat = DatumGetFloat8(value);

				status = bsonAppendDouble(queryDocument, keyName, valueFloat);
			}
			break;
		case NUMERICOID:
			{
				Datum		valueDatum = DirectFunctionCall1(numeric_float8,
															 value);
				float8		valueFloat = DatumGetFloat8(valueDatum);

				status = bsonAppendDouble(queryDocument, keyName, valueFloat);
			}
			break;
		case BOOLOID:
			{
				bool		valueBool = DatumGetBool(value);

				status = bsonAppendBool(queryDocument, keyName,
										(int) valueBool);
			}
			break;
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
			{
				char	   *outputString;
				Oid			outputFunctionId;
				bool		typeVarLength;

				getTypeOutputInfo(id, &outputFunctionId, &typeVarLength);
				outputString = OidOutputFunctionCall(outputFunctionId, value);
				status = bsonAppendUTF8(queryDocument, keyName, outputString);
			}
			break;
		case BYTEAOID:
			{
				int			len;
				char	   *data;
				char	   *result = DatumGetPointer(value);

				if (VARATT_IS_1B(result))
				{
					len = VARSIZE_1B(result) - VARHDRSZ_SHORT;
					data = VARDATA_1B(result);
				}
				else
				{
					len = VARSIZE_4B(result) - VARHDRSZ;
					data = VARDATA_4B(result);
				}
#ifdef META_DRIVER
				if (strcmp(keyName, "_id") == 0)
				{
					bson_oid_t	oid;

					bson_oid_init_from_data(&oid, (const uint8_t *) data);
					status = bsonAppendOid(queryDocument, keyName, &oid);
				}
				else
					status = bsonAppendBinary(queryDocument, keyName, data,
											  len);
#else
				status = bsonAppendBinary(queryDocument, keyName, data, len);
#endif
			}
			break;
		case NAMEOID:
			{
				char	   *outputString;
				Oid			outputFunctionId;
				bool		typeVarLength;
				bson_oid_t	bsonObjectId;

				memset(bsonObjectId.bytes, 0, sizeof(bsonObjectId.bytes));
				getTypeOutputInfo(id, &outputFunctionId, &typeVarLength);
				outputString = OidOutputFunctionCall(outputFunctionId, value);
				bsonOidFromString(&bsonObjectId, outputString);
				status = bsonAppendOid(queryDocument, keyName, &bsonObjectId);
			}
			break;
		case DATEOID:
			{
				Datum		valueDatum = DirectFunctionCall1(date_timestamp,
															 value);
				Timestamp	valueTimestamp = DatumGetTimestamp(valueDatum);
				int64		valueMicroSecs = valueTimestamp + POSTGRES_TO_UNIX_EPOCH_USECS;
				int64		valueMilliSecs = valueMicroSecs / 1000;

				status = bsonAppendDate(queryDocument, keyName,
										valueMilliSecs);
			}
			break;
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			{
				Timestamp	valueTimestamp = DatumGetTimestamp(value);
				int64		valueMicroSecs = valueTimestamp + POSTGRES_TO_UNIX_EPOCH_USECS;
				int64		valueMilliSecs = valueMicroSecs / 1000;

				status = bsonAppendDate(queryDocument, keyName,
										valueMilliSecs);
			}
			break;
		case NUMERICARRAY_OID:
			{
				ArrayType  *array;
				Oid			elmtype;
				int16		elmlen;
				bool		elmbyval;
				char		elmalign;
				int			num_elems;
				Datum	   *elem_values;
				bool	   *elem_nulls;
				int			i;
				BSON		childDocument;

				array = DatumGetArrayTypeP(value);
				elmtype = ARR_ELEMTYPE(array);
				get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

				deconstruct_array(array, elmtype, elmlen, elmbyval, elmalign,
								  &elem_values, &elem_nulls, &num_elems);

				bsonAppendStartArray(queryDocument, keyName, &childDocument);
				for (i = 0; i < num_elems; i++)
				{
					Datum		valueDatum;
					float8		valueFloat;

					if (elem_nulls[i])
						continue;

					valueDatum = DirectFunctionCall1(numeric_float8,
													 elem_values[i]);
					valueFloat = DatumGetFloat8(valueDatum);
#ifdef META_DRIVER
					status = bsonAppendDouble(&childDocument, keyName,
											  valueFloat);
#else
					status = bsonAppendDouble(queryDocument, keyName,
											  valueFloat);
#endif
				}
				bsonAppendFinishArray(queryDocument, &childDocument);
				pfree(elem_values);
				pfree(elem_nulls);
			}
			break;
		case TEXTARRAYOID:
			{
				ArrayType  *array;
				Oid			elmtype;
				int16		elmlen;
				bool		elmbyval;
				char		elmalign;
				int			num_elems;
				Datum	   *elem_values;
				bool	   *elem_nulls;
				int			i;
				BSON		childDocument;

				array = DatumGetArrayTypeP(value);
				elmtype = ARR_ELEMTYPE(array);
				get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

				deconstruct_array(array, elmtype, elmlen, elmbyval, elmalign,
								  &elem_values, &elem_nulls, &num_elems);

				bsonAppendStartArray(queryDocument, keyName, &childDocument);
				for (i = 0; i < num_elems; i++)
				{
					char	   *valueString;
					Oid			outputFunctionId;
					bool		typeVarLength;

					if (elem_nulls[i])
						continue;

					getTypeOutputInfo(TEXTOID, &outputFunctionId,
									  &typeVarLength);
					valueString = OidOutputFunctionCall(outputFunctionId,
														elem_values[i]);
#ifdef META_DRIVER
					status = bsonAppendUTF8(&childDocument, keyName,
											valueString);
#else
					status = bsonAppendUTF8(queryDocument, keyName,
											valueString);
#endif
				}
				bsonAppendFinishArray(queryDocument, &childDocument);
				pfree(elem_values);
				pfree(elem_nulls);
			}
			break;
		case JSONOID:
			{
				char	   *outputString;
				Oid			outputFunctionId;
				struct json_object *o;
				bool		typeVarLength;

				getTypeOutputInfo(id, &outputFunctionId, &typeVarLength);
				outputString = OidOutputFunctionCall(outputFunctionId, value);
				o = jsonTokenerPrase(outputString);

				if (o == NULL)
				{
					elog(WARNING, "cannot parse the document");
					status = 0;
					break;
				}

				status = jsonToBsonAppendElement(queryDocument, keyName, o);
			}
			break;
		default:

			/*
			 * We currently error out on other data types. Some types such as
			 * byte arrays are easy to add, but they need testing.
			 *
			 * Other types such as money or inet, do not have equivalents in
			 * MongoDB.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("cannot convert constant value to BSON value"),
					 errhint("Constant value data type: %u", id)));
			break;
	}

	return status;
}

/*
 * mongo_get_column_list
 *		Process scan_var_list to find all columns needed for query execution
 *		and return them.
 *
 * Also, form two separate lists:
 * 1. column_name_list: column names of needed columns.
 * 2. is_inner_column_list: column is of inner relation or not.
 */
List *
mongo_get_column_list(PlannerInfo *root, RelOptInfo *foreignrel,
					  List *scan_var_list, List **column_name_list,
					  List **is_inner_column_list)
{
	List	   *columnList = NIL;
	ListCell   *lc;
	RelOptInfo *scanrel;
	MongoFdwRelationInfo *fpinfo = (MongoFdwRelationInfo *) foreignrel->fdw_private;
	MongoFdwRelationInfo *ofpinfo;

	scanrel = IS_UPPER_REL(foreignrel) ? fpinfo->outerrel : foreignrel;

	if (IS_UPPER_REL(foreignrel) && IS_JOIN_REL(scanrel))
		ofpinfo = (MongoFdwRelationInfo *) fpinfo->outerrel->fdw_private;

	foreach(lc, scan_var_list)
	{
		Var		   *var = (Var *) lfirst(lc);
		RangeTblEntry *rte = planner_rt_fetch(var->varno, root);
		int			is_innerrel = false;

		/*
		 * Add aggregation target also in the needed column list.  This would
		 * be handled in the function column_mapping_hash.
		 */
		if (IsA(var, Aggref))
		{
#if PG_VERSION_NUM >= 160000
			columnList = mongo_append_unique_var(columnList, var);
#else
			columnList = list_append_unique(columnList, var);
#endif
			continue;
		}

		if (!IsA(var, Var))
			continue;

		/* Var belongs to foreign table? */
		if (!bms_is_member(var->varno, scanrel->relids))
			continue;

		/* Is whole-row reference requested? */
		if (var->varattno == 0)
		{
			List	   *wr_var_list;
			Bitmapset  *attrs_used;
#if PG_VERSION_NUM >= 160000
			ListCell   *cell;
#endif

			Assert(OidIsValid(rte->relid));

			/*
			 * Get list of Var nodes for all undropped attributes of the base
			 * relation.
			 */
			attrs_used = bms_make_singleton(0 -
											FirstLowInvalidHeapAttributeNumber);

			wr_var_list = prepare_var_list_for_baserel(rte->relid, var->varno,
													   attrs_used);

#if PG_VERSION_NUM >= 160000
			foreach(cell, wr_var_list)
			{
				Var		   *tlvar = (Var *) lfirst(cell);

				columnList = mongo_append_unique_var(columnList, tlvar);
			}
#else
			columnList = list_concat_unique(columnList, wr_var_list);
#endif
			bms_free(attrs_used);
		}
		else
#if PG_VERSION_NUM >= 160000
			columnList = mongo_append_unique_var(columnList, var);
#else
			columnList = list_append_unique(columnList, var);
#endif

		if (IS_JOIN_REL(foreignrel) ||
			(IS_UPPER_REL(foreignrel) && IS_JOIN_REL(scanrel)))
		{
			char	   *columnName;

			columnName = get_attname(rte->relid, var->varattno, false);
			*column_name_list = lappend(*column_name_list,
										makeString(columnName));
			if (IS_UPPER_REL(foreignrel) && IS_JOIN_REL(scanrel) &&
				bms_is_member(var->varno, ofpinfo->innerrel->relids))
				is_innerrel = true;
			else if (IS_JOIN_REL(foreignrel) &&
					 bms_is_member(var->varno, fpinfo->innerrel->relids))
				is_innerrel = true;

			*is_inner_column_list = lappend_int(*is_inner_column_list,
												is_innerrel);
		}
	}

	return columnList;
}

/*
 * Check if expression is safe to execute remotely, and return true if so.
 *
 * In addition, *outer_cxt is updated with collation information.
 *
 * We must check that the expression contains only node types we can deparse,
 * that all types/operators are safe to send (which we approximate
 * as being built-in), and that all collations used in the expression derive
 * from Vars of the foreign table.
 *
 * For WHERE as well as JOIN clauses, in the case of operator expression, we do
 * support arithmetic (=, <, >, <=, >=, <>, +, -, *, /, %, ^, @ and |/)
 * operators.  Also, both operands of the binary operator can be a column.  If
 * the expression is a tree, we do recurse into it.  Supports Boolean
 * expression as well.
 */
static bool
foreign_expr_walker(Node *node, foreign_glob_cxt *glob_cxt,
					foreign_loc_cxt *outer_cxt)
{
	foreign_loc_cxt inner_cxt;
	Oid			collation;
	FDWCollateState state;

	/* Need do nothing for empty subexpressions */
	if (node == NULL)
		return true;

	/* Set up inner_cxt for possible recursion to child nodes */
	inner_cxt.collation = InvalidOid;
	inner_cxt.state = FDW_COLLATE_NONE;

	switch (nodeTag(node))
	{
		case T_Var:
			{
				Var		   *var = (Var *) node;

#ifndef META_DRIVER
				/* Increment the Var count */
				glob_cxt->varcount++;
#endif

				/*
				 * If the Var is from the foreign table, we consider its
				 * collation (if any) safe to use.  If it is from another
				 * table, don't push it down.
				 */
				if (bms_is_member(var->varno, glob_cxt->relids) &&
					var->varlevelsup == 0)
				{
					/* Var belongs to foreign table */
					collation = var->varcollid;
					state = OidIsValid(collation) ? FDW_COLLATE_SAFE : FDW_COLLATE_NONE;
				}
				else
				{
					/*
					 * Var belongs to some other table.  Unlike postgres_fdw,
					 * can't be treated like Param because MongoDB doesn't
					 * have corresponding syntax to represent it in the query
					 * pipeline.
					 */
					return false;
				}
			}
			break;
		case T_Const:
			{
				Const	   *c = (Const *) node;

				/*
				 * We don't push down operators where the constant is an
				 * array, since conditional operators for arrays in MongoDB
				 * aren't properly defined.
				 */
				if (OidIsValid(get_element_type(c->consttype)))
					return false;

				/*
				 * If the constant has nondefault collation, either it's of a
				 * non-builtin type, or it reflects folding of a CollateExpr.
				 * It's unsafe to send to the remote unless it's used in a
				 * non-collation-sensitive context.
				 */
				collation = c->constcollid;
				if (collation == InvalidOid ||
					collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_Param:
			{
				Param	   *p = (Param *) node;

				/*
				 * Bail out on planner internal params. We could perhaps pass
				 * them to the remote server as regular params, but we don't
				 * have the machinery to do that at the moment.
				 */
				if (p->paramkind != PARAM_EXTERN)
					return false;

				/*
				 * Collation rule is same as for Consts and non-foreign Vars.
				 */
				collation = p->paramcollid;
				if (collation == InvalidOid ||
					collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_OpExpr:
			{
				OpExpr	   *oe = (OpExpr *) node;
				char	   *oname = get_opname(oe->opno);

				/* Don't support operator expression in grouping targets */
				if (IS_UPPER_REL(glob_cxt->foreignrel) &&
					!glob_cxt->is_having_cond)
					return false;

#ifndef META_DRIVER
				/* Increment the operator expression count */
				glob_cxt->opexprcount++;
#endif

				/*
				 * We support =, <, >, <=, >=, <>, +, -, *, /, %, ^, |/, and @
				 * operators for joinclause of join relation.
				 */
				if (!(strncmp(oname, EQUALITY_OPERATOR_NAME, NAMEDATALEN) == 0) &&
					(mongo_operator_name(oname) == NULL))
					return false;

				/*
				 * Recurse to input subexpressions.
				 *
				 * We support same operators as joinclause for WHERE
				 * conditions of simple as well as join relation.
				 */
				if (!foreign_expr_walker((Node *) oe->args, glob_cxt,
										 &inner_cxt)
#ifndef META_DRIVER
					|| (glob_cxt->opexprcount > 1)
#endif
					)
					return false;

				/*
				 * If operator's input collation is not derived from a foreign
				 * Var, it can't be sent to remote.
				 */
				if (oe->inputcollid == InvalidOid)
					 /* OK, inputs are all noncollatable */ ;
				else if (inner_cxt.state != FDW_COLLATE_SAFE ||
						 oe->inputcollid != inner_cxt.collation)
					return false;

				/* Result-collation handling */
				collation = oe->opcollid;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (inner_cxt.state == FDW_COLLATE_SAFE &&
						 collation == inner_cxt.collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_RelabelType:
			{
				RelabelType *r = (RelabelType *) node;

				/*
				 * Recurse to input subexpression.
				 */
				if (!foreign_expr_walker((Node *) r->arg,
										 glob_cxt, &inner_cxt))
					return false;

				/*
				 * RelabelType must not introduce a collation not derived from
				 * an input foreign Var.
				 */
				collation = r->resultcollid;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (inner_cxt.state == FDW_COLLATE_SAFE &&
						 collation == inner_cxt.collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
		case T_List:
			{
				List	   *l = (List *) node;
				ListCell   *lc;

				/*
				 * Recurse to component subexpressions.
				 *
				 * For simple relation, if the comparison is between two
				 * columns of the same table, then we don't push down because
				 * building corresponding MongoDB query is not possible with
				 * the cirrent MongoC driver.
				 */
				foreach(lc, l)
				{
					if ((!foreign_expr_walker((Node *) lfirst(lc),
											  glob_cxt, &inner_cxt))
#ifndef META_DRIVER
						|| (glob_cxt->varcount > 1)
#endif
						)
						return false;
				}

				/*
				 * When processing a list, collation state just bubbles up
				 * from the list elements.
				 */
				collation = inner_cxt.collation;
				state = inner_cxt.state;
			}
			break;
		case T_BoolExpr:
			{
				BoolExpr   *b = (BoolExpr *) node;

				/*
				 * Recurse to input sub-expressions.
				 */
				if (!foreign_expr_walker((Node *) b->args,
										 glob_cxt, &inner_cxt))
					return false;

				/* Output is always boolean and so noncollatable. */
				collation = InvalidOid;
				state = FDW_COLLATE_NONE;
			}
			break;
#ifdef META_DRIVER
		case T_Aggref:
			{
				Aggref	   *agg = (Aggref *) node;
				ListCell   *lc;
				const char *func_name = get_func_name(agg->aggfnoid);

				/* Not safe to pushdown when not in a grouping context */
				if (!IS_UPPER_REL(glob_cxt->foreignrel))
					return false;

				/* Only non-split aggregates are pushable. */
				if (agg->aggsplit != AGGSPLIT_SIMPLE)
					return false;

				/*
				 * Aggregates with the order, FILTER, VARIADIC, and DISTINCT
				 * are not supported on MongoDB.
				 */
				if (agg->aggorder || agg->aggfilter || agg->aggvariadic ||
					agg->aggdistinct)
					return false;

				if (!(strcmp(func_name, "min") == 0 ||
					  strcmp(func_name, "max") == 0 ||
					  strcmp(func_name, "sum") == 0 ||
					  strcmp(func_name, "avg") == 0 ||
					  strcmp(func_name, "count") == 0))
					return false;

				/*
				 * Don't push down when the count is on the column.  This
				 * restriction is due to the unavailability of syntax in the
				 * MongoDB to provide a count of the particular column.
				 */
				if (!strcmp(func_name, "count") && agg->args)
					return false;

				/*
				 * Recurse to input args. aggdirectargs, aggorder, and
				 * aggdistinct are all present in args, so no need to check
				 * their shippability explicitly.
				 */
				foreach(lc, agg->args)
				{
					Node	   *n = (Node *) lfirst(lc);

					/* If TargetEntry, extract the expression from it. */
					if (IsA(n, TargetEntry))
					{
						TargetEntry *tle = (TargetEntry *) n;

						n = (Node *) tle->expr;
					}

					if (!IsA(n, Var) || !foreign_expr_walker(n, glob_cxt,
															 &inner_cxt))
						return false;
				}

				/*
				 * If aggregate's input collation is not derived from a
				 * foreign Var, it can't be sent to remote.
				 */
				if (agg->inputcollid == InvalidOid)
					 /* OK, inputs are all noncollatable */ ;
				else if (inner_cxt.state != FDW_COLLATE_SAFE ||
						 agg->inputcollid != inner_cxt.collation)
					return false;

				/*
				 * Detect whether the node is introducing a collation not
				 * derived from a foreign Var.  (If so, we just mark it unsafe
				 * for now rather than immediately returning false, since th e
				 * parent node might not care.)
				 */
				collation = agg->aggcollid;
				if (collation == InvalidOid)
					state = FDW_COLLATE_NONE;
				else if (inner_cxt.state == FDW_COLLATE_SAFE &&
						 collation == inner_cxt.collation)
					state = FDW_COLLATE_SAFE;
				else if (collation == DEFAULT_COLLATION_OID)
					state = FDW_COLLATE_NONE;
				else
					state = FDW_COLLATE_UNSAFE;
			}
			break;
#endif
		default:

			/*
			 * If it's anything else, assume it's unsafe.  This list can be
			 * expanded later, but don't forget to add deparse support.
			 */
			return false;
	}

	/*
	 * Now, merge my collation information into my parent's state.
	 */
	if (state > outer_cxt->state)
	{
		/* Override previous parent state */
		outer_cxt->collation = collation;
		outer_cxt->state = state;
	}
	else if (state == outer_cxt->state)
	{
		/* Merge, or detect error if there's a collation conflict */
		switch (state)
		{
			case FDW_COLLATE_NONE:
				/* Nothing + nothing is still nothing */
				break;
			case FDW_COLLATE_SAFE:
				if (collation != outer_cxt->collation)
				{
					/*
					 * Non-default collation always beats default.
					 */
					if (outer_cxt->collation == DEFAULT_COLLATION_OID)
					{
						/* Override previous parent state */
						outer_cxt->collation = collation;
					}
					else if (collation != DEFAULT_COLLATION_OID)
					{
						/*
						 * Conflict; show state as indeterminate.  We don't
						 * want to "return false" right away, since parent
						 * node might not care about collation.
						 */
						outer_cxt->state = FDW_COLLATE_UNSAFE;
					}
				}
				break;
			case FDW_COLLATE_UNSAFE:
				/* We're still conflicted ... */
				break;
		}
	}

	/* It looks OK */
	return true;
}

/*
 * mongo_is_foreign_expr
 *		Returns true if given expr is safe to evaluate on the foreign server.
 */
bool
mongo_is_foreign_expr(PlannerInfo *root, RelOptInfo *baserel, Expr *expression,
					  bool is_having_cond)
{
	foreign_glob_cxt glob_cxt;
	foreign_loc_cxt loc_cxt;
	MongoFdwRelationInfo *fpinfo = (MongoFdwRelationInfo *) baserel->fdw_private;

	/*
	 * Check that the expression consists of nodes that are safe to execute
	 * remotely.
	 */
	glob_cxt.root = root;
	glob_cxt.foreignrel = baserel;

	/*
	 * For an upper relation, use relids from its underneath scan relation,
	 * because the upperrel's own relids currently aren't set to anything
	 * meaningful by the core code.  For other relations, use their own
	 * relids.
	 */
	if (IS_UPPER_REL(baserel))
		glob_cxt.relids = fpinfo->outerrel->relids;
	else
		glob_cxt.relids = baserel->relids;

#ifndef META_DRIVER
	glob_cxt.varcount = 0;
	glob_cxt.opexprcount = 0;
#endif
	glob_cxt.is_having_cond = is_having_cond;
	loc_cxt.collation = InvalidOid;
	loc_cxt.state = FDW_COLLATE_NONE;
	if (!foreign_expr_walker((Node *) expression, &glob_cxt, &loc_cxt))
		return false;

	/*
	 * If the expression has a valid collation that does not arise from a
	 * foreign var, the expression can not be sent over.
	 */
	if (loc_cxt.state == FDW_COLLATE_UNSAFE)
		return false;

	/* OK to evaluate on the remote server */
	return true;
}

/*
 * prepare_var_list_for_baserel
 *		Build list of nodes corresponding to the attributes requested for given
 *		base relation.
 *
 * The list contains Var nodes corresponding to the attributes specified in
 * attrs_used. If whole-row reference is required, add Var nodes corresponding
 * to all the attributes in the relation.
 */
static List *
prepare_var_list_for_baserel(Oid relid, Index varno, Bitmapset *attrs_used)
{
	int			attno;
	List	   *tlist = NIL;
	Node	   *node;
	bool		wholerow_requested = false;
	Relation	relation;
	TupleDesc	tupdesc;

	Assert(OidIsValid(relid));

	/* Planner must have taken a lock, so request no lock here */
#if PG_VERSION_NUM < 130000
	relation = heap_open(relid, NoLock);
#else
	relation = table_open(relid, NoLock);
#endif

	tupdesc = RelationGetDescr(relation);

	/* Is whole-row reference requested? */
	wholerow_requested = bms_is_member(0 - FirstLowInvalidHeapAttributeNumber,
									   attrs_used);

	/* Handle user defined attributes first. */
	for (attno = 1; attno <= tupdesc->natts; attno++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupdesc, attno - 1);

		/* Ignore dropped attributes. */
		if (attr->attisdropped)
			continue;

		/* For a required attribute create a Var node */
		if (wholerow_requested ||
			bms_is_member(attno - FirstLowInvalidHeapAttributeNumber,
						  attrs_used))
		{
			node = (Node *) makeVar(varno, attno, attr->atttypid,
									attr->atttypmod, attr->attcollation, 0);
			tlist = lappend(tlist, node);

		}
	}

#if PG_VERSION_NUM < 130000
	heap_close(relation, NoLock);
#else
	table_close(relation, NoLock);
#endif

	return tlist;
}

#ifdef META_DRIVER
/*
 * column_info_hash
 *		Creates a hash table that maps varno and varattno to the column names,
 *		and also stores whether the column is part of outer relation or not.
 *
 * This table helps us to form the pipeline quickly.
 */
static HTAB *
column_info_hash(List *colname_list, List *colnum_list, List *rti_list,
				 List *isouter_list)
{
	HTAB	   *columnInfoHash;
	ColInfoHashKey key;
	HASHCTL		hashInfo;
	ListCell   *l1;
	ListCell   *l2;
	ListCell   *l3;
	ListCell   *l4;

	memset(&hashInfo, 0, sizeof(hashInfo));
	hashInfo.keysize = sizeof(ColInfoHashKey);
	hashInfo.entrysize = sizeof(ColInfoHashEntry);
	hashInfo.hcxt = CurrentMemoryContext;

	columnInfoHash = hash_create("Column Information Hash", MaxHashTableSize,
								 &hashInfo,
								 (HASH_ELEM | HASH_BLOBS | HASH_CONTEXT));
	Assert(columnInfoHash != NULL);

	/*
	 * There's no forfour() in version 11 and below, so need to traverse one
	 * list the hard way.
	 */
	l4 = list_head(isouter_list);
	forthree(l1, colname_list, l2, colnum_list, l3, rti_list)
	{
		ColInfoHashEntry *columnInfo;
		char	   *columnName = strVal(lfirst(l1));
		int			columnNum = lfirst_int(l2);
		int			varNo = lfirst_int(l3);
		bool		isOuter = lfirst_int(l4);

		key.varNo = varNo;
		key.varAttno = columnNum;

		columnInfo = (ColInfoHashEntry *) hash_search(columnInfoHash,
													  (void *) &key,
													  HASH_ENTER,
													  NULL);
		Assert(columnInfo != NULL);

		columnInfo->colName = columnName;
		columnInfo->isOuter = isOuter;

#if PG_VERSION_NUM >= 130000
		l4 = lnext(isouter_list, l4);
#else
		l4 = lnext(l4);
#endif
	}

	return columnInfoHash;
}

/*
 * mongo_prepare_pipeline
 *		Form query pipeline syntax equivalent to postgresql.
 *
 * From the example given on mongo_query_document, the following part of
 * MongoDB query formed by this function:
 *
 *          "pipeline": [
 *            {
 *              "$match":
 *              {
 *                "$expr":
 *                {
 *                  "$and": [
 *                    { "$eq": [ "$$v_age", "$old" ] }
 *                    { "$ne": [ "$$v_age", null ] },
 *                    { "$ne": [ "$old", null ] },
 *                  ]
 *                }
 *              }
 *            }
 *          ]
 */
static void
mongo_prepare_pipeline(List *clause, BSON *inner_pipeline,
					   pipeline_cxt *context)
{
	BSON	   *and_query_doc = bsonCreate();
	BSON		match_object;
	BSON		match_stage;
	BSON		expr;
	BSON		and_op;

	if (context->isJoinClause)
	{
		int			inner_pipeline_index = 0;

		bsonAppendStartObject(inner_pipeline,
							  psprintf("%d", inner_pipeline_index++),
							  &match_object);
		bsonAppendStartObject(&match_object, "$match", &match_stage);
	}
	else
		bsonAppendStartObject(inner_pipeline, "$match", &match_stage);

	bsonAppendStartObject(&match_stage, "$expr", &expr);

	bsonAppendStartArray(and_query_doc, "$and", &and_op);

	context->arrayIndex = 0;
	context->opExprCount = 0;

	/* Append JOIN/WHERE/HAVING clause expression */
	mongo_append_clauses_to_pipeline(clause, &and_op, context);

	/* Append $and array to $expr */
	bson_append_array(&expr, "$and", (int) strlen("$and"), &and_op);

	bsonAppendFinishArray(and_query_doc, &and_op);
	bsonAppendFinishObject(&match_stage, &expr);
	if (context->isJoinClause)
	{
		bsonAppendFinishObject(&match_object, &match_stage);
		bsonAppendFinishObject(inner_pipeline, &match_object);
	}
	else
		bsonAppendFinishObject(inner_pipeline, &match_stage);
}

/*
 * mongo_append_clauses_to_pipeline
 *		Append all JOIN/WHERE/HAVING clauses to mongoDB's $and array.
 */
static void
mongo_append_clauses_to_pipeline(List *clause, BSON *child_doc,
								 pipeline_cxt *context)
{
	ListCell   *lc;

	/* loop through all clauses */
	foreach(lc, clause)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		/* Extract clause from RestrictInfo */
		if (IsA(expr, RestrictInfo))
		{
			RestrictInfo *ri = (RestrictInfo *) expr;

			expr = ri->clause;
		}

		mongo_append_expr(expr, child_doc, context);
		context->arrayIndex++;
	}
}

/*
 * mongo_is_foreign_param
 * 		Returns true if given expr is something we'd have to send the
 * 		value of to the foreign server.
 */
bool
mongo_is_foreign_param(PlannerInfo *root, RelOptInfo *baserel, Expr *expr)
{
	if (expr == NULL)
		return false;

	switch (nodeTag(expr))
	{
		case T_Var:
			{
				/* It would have to be sent unless it's a foreign Var. */
				Var		   *var = (Var *) expr;
				Relids		relids;
				MongoFdwRelationInfo *fpinfo = (MongoFdwRelationInfo *) (baserel->fdw_private);

				if (IS_UPPER_REL(baserel))
					relids = fpinfo->outerrel->relids;
				else
					relids = baserel->relids;

				if (bms_is_member(var->varno, relids) && var->varlevelsup == 0)
					return false;	/* foreign Var, so not a param. */
				else
					return true;	/* it'd have to be a param. */
				break;
			}
		case T_Param:
			/* Params always have to be sent to the foreign server. */
			return true;
		default:
			break;
	}
	return false;
}
#endif

#if PG_VERSION_NUM >= 160000
/*
 * mongo_append_unique_var
 * 		Append var to var list, but only if it isn't already in the list.
 *
 * Whether a var is already a member of list is determined using varno and
 * varattno.
 */
static List *
mongo_append_unique_var(List *varlist, Var *var)
{
	ListCell   *lc;

	foreach(lc, varlist)
	{
		Var		   *tlvar = (Var *) lfirst(lc);

		if (IsA(tlvar, Var) &&
			tlvar->varno == var->varno &&
			tlvar->varattno == var->varattno)
			return varlist;
	}

	return lappend(varlist, var);
}
#endif
