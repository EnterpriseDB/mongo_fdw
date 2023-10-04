/*-------------------------------------------------------------------------
 *
 * deparse.c
 * 		Query deparser for mongo_fdw
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2023, EnterpriseDB Corporation.
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		deparse.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "mongo_wrapper.h"

#include <bson.h>
#include <json.h>

#include "access/htup_details.h"
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
 * Functions to gather information related to columns involved in the given
 * query, which is useful at the time of execution to prepare MongoDB query.
 */
static void mongo_check_op_expr(OpExpr *node, MongoRelQualInfo *qual_info);
static void mongo_check_var(Var *column, MongoRelQualInfo *qual_info);

/* Helper functions to form MongoDB query document. */
static void mongo_append_bool_expr(BoolExpr *node, BSON *queryDoc,
								   pipeline_cxt *context);
static void mongo_append_op_expr(OpExpr *node, BSON *child,
								 pipeline_cxt *context);
static void mongo_append_column_name(Var *column, BSON *queryDoc,
									 pipeline_cxt *context);
static void mongo_add_null_check(Var *column, BSON *expr,
								 pipeline_cxt *context);

/*
 * mongo_check_qual
 *		Check the given qual expression and find the columns used in it.  We
 *		recursively traverse until we get a Var node and then retrieve the
 *		required information from it.
 */
void
mongo_check_qual(Expr *node, MongoRelQualInfo *qual_info)
{
	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
		case T_Var:
			mongo_check_var((Var *) node, qual_info);
			break;
		case T_OpExpr:
			mongo_check_op_expr((OpExpr *) node, qual_info);
			break;
		case T_List:
			{
				ListCell   *lc;

				foreach(lc, (List *) node)
					mongo_check_qual((Expr *) lfirst(lc), qual_info);
			}
			break;
		case T_RelabelType:
			mongo_check_qual(((RelabelType *) node)->arg, qual_info);
			break;
		case T_BoolExpr:
			mongo_check_qual((Expr *) ((BoolExpr *) node)->args, qual_info);
			break;
		case T_Aggref:
			{
				ListCell   *lc;
				char	   *func_name = get_func_name(((Aggref *) node)->aggfnoid);

				/* Save aggregation operation name */
				qual_info->aggTypeList = lappend(qual_info->aggTypeList,
												 makeString(func_name));

				qual_info->is_agg_column = true;

				/* Save information whether this is a HAVING clause or not */
				if (qual_info->is_having)
					qual_info->isHavingList = lappend_int(qual_info->isHavingList,
														  true);
				else
					qual_info->isHavingList = lappend_int(qual_info->isHavingList,
														  false);

				/*
				 * The aggregation over '*' doesn't need column information.
				 * Hence, only to maintain the length of column information
				 * lists add dummy members into it.
				 *
				 * For aggregation over the column, add required information
				 * into the column information lists.
				 */
				if (((Aggref *) node)->aggstar)
				{
					qual_info->colNameList = lappend(qual_info->colNameList,
													 makeString("*"));
					qual_info->colNumList = lappend_int(qual_info->colNumList,
														0);
					qual_info->rtiList = lappend_int(qual_info->rtiList, 0);
					qual_info->isOuterList = lappend_int(qual_info->isOuterList,
														 0);
					/* Append dummy var */
					qual_info->aggColList = lappend(qual_info->aggColList,
													makeVar(0, 0, 0, 0, 0, 0));
					qual_info->is_agg_column = false;
				}
				else
				{
					foreach(lc, ((Aggref *) node)->args)
					{
						Node	   *n = (Node *) lfirst(lc);

						/* If TargetEntry, extract the expression from it */
						if (IsA(n, TargetEntry))
						{
							TargetEntry *tle = (TargetEntry *) n;

							n = (Node *) tle->expr;
						}

						mongo_check_qual((Expr *) n, qual_info);
					}
				}
			}
			break;
		case T_Const:
		case T_Param:
			/* Nothing to do here because we are looking only for Var's */
			break;
		default:
			elog(ERROR, "unsupported expression type to check: %d",
				 (int) nodeTag(node));
			break;
	}
}

/*
 * mongo_check_op_expr
 *		Check given operator expression.
 */
static void
mongo_check_op_expr(OpExpr *node, MongoRelQualInfo *qual_info)
{
	HeapTuple	tuple;
	Form_pg_operator form;
	char		oprkind;
	ListCell   *arg;

	/* Retrieve information about the operator from the system catalog. */
	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(node->opno));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator %u", node->opno);

	form = (Form_pg_operator) GETSTRUCT(tuple);
	oprkind = form->oprkind;

	/* Sanity check. */
	Assert((oprkind == 'r' && list_length(node->args) == 1) ||
		   (oprkind == 'l' && list_length(node->args) == 1) ||
		   (oprkind == 'b' && list_length(node->args) == 2));

	/* Deparse left operand. */
	if (oprkind == 'r' || oprkind == 'b')
	{
		arg = list_head(node->args);
		mongo_check_qual(lfirst(arg), qual_info);
	}

	/* Deparse right operand. */
	if (oprkind == 'l' || oprkind == 'b')
	{
		arg = list_tail(node->args);
		mongo_check_qual(lfirst(arg), qual_info);
	}

	ReleaseSysCache(tuple);
}

/*
 * mongo_check_var
 *		Check the given Var and append required information related to columns
 *		involved in qual clauses to separate lists in context. Prepare separate
 *		list for aggregated columns directly (not related information).
 *
 * Save required information in the form of a list in MongoRelQualInfo
 * structure.  Prepare a hash table to avoid duplication of entry if one column
 * is involved in the multiple qual expressions.
 */
static void
mongo_check_var(Var *column, MongoRelQualInfo *qual_info)
{
	RangeTblEntry *rte;
	char	   *colname;
	ColumnHashKey key;
	bool		found;
	bool		is_outerrel = false;

	if (!(bms_is_member(column->varno, qual_info->foreignRel->relids) &&
		  column->varlevelsup == 0))
		return;					/* Var does not belong to foreign table */

	Assert(!IS_SPECIAL_VARNO(column->varno));

	if (!qual_info->exprColHash)
	{
		HASHCTL		hashInfo;

		memset(&hashInfo, 0, sizeof(hashInfo));
		hashInfo.keysize = sizeof(ColumnHashKey);
		hashInfo.entrysize = sizeof(ColumnHashKey);
		hashInfo.hcxt = CurrentMemoryContext;

		qual_info->exprColHash = hash_create("Join Expression Column Hash",
											 MaxHashTableSize,
											 &hashInfo,
											 (HASH_ELEM | HASH_BLOBS | HASH_CONTEXT));
	}

	key.varno = column->varno;
	key.varattno = column->varattno;

	hash_search(qual_info->exprColHash, (void *) &key, HASH_ENTER, &found);

	/*
	 * Add aggregated column in the aggColList even if it's already available
	 * in the hash table.  This is because multiple aggregation operations can
	 * be done on the same column.  So, to maintain the same length of
	 * aggregation functions and their columns, add each aggregation column.
	 */
	if (qual_info->is_agg_column)
	{
		qual_info->aggColList = lappend(qual_info->aggColList, column);
		qual_info->is_agg_column = false;
		if (found)
			return;
	}

	/*
	 * Don't add the duplicate column.  The Aggregated column is already taken
	 * care of.
	 */
	if (found)
		return;

	/* Get RangeTblEntry from array in PlannerInfo. */
	rte = planner_rt_fetch(column->varno, qual_info->root);

	colname = get_attname(rte->relid, column->varattno, false);

	/* Is relation inner or outer? */
	if (bms_is_member(column->varno, qual_info->outerRelids))
		is_outerrel = true;

	/* Fill the lists with elements */
	qual_info->colNameList = lappend(qual_info->colNameList, makeString(colname));
	qual_info->colNumList = lappend_int(qual_info->colNumList, column->varattno);
	qual_info->rtiList = lappend_int(qual_info->rtiList, column->varno);
	qual_info->isOuterList = lappend_int(qual_info->isOuterList, is_outerrel);
}

/*
 * mongo_get_jointype_name
 * 		Output join name for given join type
 */
const char *
mongo_get_jointype_name(JoinType jointype)
{
	switch (jointype)
	{
		case JOIN_INNER:
			return "INNER";

		case JOIN_LEFT:
			return "LEFT";

		case JOIN_RIGHT:
			return "RIGHT";

		default:
			/* Shouldn't come here, but protect from buggy code. */
			elog(ERROR, "unsupported join type %d", jointype);
	}

	/* Keep compiler happy */
	return NULL;
}

/*
 * mongo_append_expr
 *		Append given expression node.
 */
void
mongo_append_expr(Expr *node, BSON *child_doc, pipeline_cxt *context)
{
	if (node == NULL)
		return;

	switch (nodeTag(node))
	{
		case T_Var:
			mongo_append_column_name((Var *) node, child_doc, context);
			break;
		case T_Const:
			append_constant_value(child_doc,
								  psprintf("%d", context->arrayIndex),
								  (Const *) node);
			break;
		case T_OpExpr:
			mongo_append_op_expr((OpExpr *) node, child_doc, context);
			break;
		case T_RelabelType:
			mongo_append_expr(((RelabelType *) node)->arg, child_doc, context);
			break;
		case T_BoolExpr:
			mongo_append_bool_expr((BoolExpr *) node, child_doc, context);
			break;
		case T_Param:
			append_param_value(child_doc, psprintf("%d", context->arrayIndex),
							   (Param *) node, context->scanStateNode);
			break;
		case T_Aggref:
			bsonAppendUTF8(child_doc, "0", "$v_having");
			break;
		default:
			elog(ERROR, "unsupported expression type to append: %d",
				 (int) nodeTag(node));
			break;
	}
}

/*
 * mongo_append_bool_expr
 *		Recurse through a BoolExpr node to form MongoDB query pipeline.
 */
static void
mongo_append_bool_expr(BoolExpr *node, BSON *child_doc, pipeline_cxt *context)
{
	BSON		child;
	BSON		expr;
	const char *op = NULL;
	ListCell   *lc;
	int			saved_array_index;
	int			reset_index = 0;

	switch (node->boolop)
	{
		case AND_EXPR:
			op = "$and";
			break;
		case OR_EXPR:
			op = "$or";
			break;
		case NOT_EXPR:
			op = "$not";
			break;
	}

	bsonAppendStartObject(child_doc, psprintf("%d", context->arrayIndex), &expr);
	bsonAppendStartArray(&expr, op, &child);

	/* Save array index */
	saved_array_index = context->arrayIndex;

	/* Reset to zero to be used for nested arrays */
	context->arrayIndex = reset_index;

	/* Save join expression type boolean "TRUE" */
	context->isBoolExpr = true;

	foreach(lc, node->args)
	{
		mongo_append_expr((Expr *) lfirst(lc), &child, context);
		context->arrayIndex++;
	}

	bsonAppendFinishArray(&expr, &child);
	bsonAppendFinishObject(child_doc, &expr);

	/* Retain array index */
	context->arrayIndex = saved_array_index;
}

/*
 * mongo_append_op_expr
 *		Deparse given operator expression.
 *
 * Build and append following syntax into $and array:
 *
 *      {"$eq": [ "$$v_age", "$old" ] }
 *
 * Each element of operator (e.g. "$eq") array is appended by function called
 * mongo_append_column_name.
 *
 * In MongoDB, (null = null), (null < 1) is TRUE but that is FALSE in Postgres.
 * To eliminate null value rows, add equality check for null values for columns
 * involved in JOIN and WHERE clauses.  E.g. add the following syntax:
 *
 * 	    {"$ne": [ "$$v_age", null ]},
 *	    {"$ne": [ "$old", null ]}
 */
static void
mongo_append_op_expr(OpExpr *node, BSON *child_doc, pipeline_cxt *context)
{
	HeapTuple	tuple;
	Form_pg_operator form;
	char		oprkind;
	ListCell   *arg;
	BSON		expr;
	BSON		child1;
	char	   *mongo_operator;
	int			saved_array_index;
	int			reset_index = 0;
	int			and_index = 0;
	BSON		and_op;
	BSON		and_obj;

	/* Increament operator expression count */
	context->opExprCount++;

	/* Retrieve information about the operator from the system catalog. */
	tuple = SearchSysCache1(OPEROID, ObjectIdGetDatum(node->opno));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for operator %u", node->opno);

	form = (Form_pg_operator) GETSTRUCT(tuple);
	oprkind = form->oprkind;

	/* Sanity check. */
	Assert((oprkind == 'r' && list_length(node->args) == 1) ||
		   (oprkind == 'l' && list_length(node->args) == 1) ||
		   (oprkind == 'b' && list_length(node->args) == 2));

	if (context->isBoolExpr == true)
	{
		bsonAppendStartObject(child_doc, psprintf("%d", and_index++),
							  &and_obj);
		bsonAppendStartArray(&and_obj, "$and", &and_op);
		bsonAppendStartObject(&and_op, psprintf("%d", context->arrayIndex),
							  &expr);
	}
	else
		bsonAppendStartObject(child_doc, psprintf("%d", context->arrayIndex),
							  &expr);

	/* Deparse operator name. */
	mongo_operator = mongo_operator_name(get_opname(node->opno));

	bsonAppendStartArray(&expr, mongo_operator, &child1);

	/* Save array index */
	saved_array_index = context->arrayIndex;

	/* Reset to zero to be used for nested arrays */
	context->arrayIndex = reset_index;

	/* Deparse left operand. */
	if (oprkind == 'r' || oprkind == 'b')
	{
		arg = list_head(node->args);
		mongo_append_expr(lfirst(arg), &child1, context);
	}

	/* Deparse right operand. */
	if (oprkind == 'l' || oprkind == 'b')
	{
		if (oprkind == 'l')
			context->arrayIndex = reset_index;
		else
			context->arrayIndex++;
		arg = list_tail(node->args);
		mongo_append_expr(lfirst(arg), &child1, context);
	}

	/* Decreament operator expression count */
	context->opExprCount--;

	bsonAppendFinishArray(&expr, &child1);
	if (context->isBoolExpr)
		bsonAppendFinishObject(&and_op, &expr);
	else
		bsonAppendFinishObject(child_doc, &expr);

	/*
	 * Add equality check for null values for columns involved in JOIN and
	 * WHERE clauses.
	 */
	if (context->opExprCount == 0)
	{
		List	   *var_list;
		ListCell   *lc;

		var_list = pull_var_clause((Node *) node, PVC_RECURSE_PLACEHOLDERS ||
								   PVC_RECURSE_AGGREGATES);

		foreach(lc, var_list)
		{
			Var		   *var = (Var *) lfirst(lc);

			if (context->isBoolExpr)
				bsonAppendStartObject(&and_op, psprintf("%d", and_index++),
									  &expr);
			else
				bsonAppendStartObject(child_doc,
									  psprintf("%d", context->arrayIndex++),
									  &expr);
			mongo_add_null_check(var, &expr, context);

			if (context->isBoolExpr)
				bsonAppendFinishObject(&and_op, &expr);
			else
				bsonAppendFinishObject(child_doc, &expr);
		}
	}

	if (context->isBoolExpr == true)
	{
		bsonAppendFinishArray(&and_obj, &and_op);
		bsonAppendFinishObject(child_doc, &and_obj);
	}

	/* Retain array index */
	context->arrayIndex = saved_array_index;

	ReleaseSysCache(tuple);
}

/*
 * mongo_append_column_name
 *		Deparse Var and append corresponding column name to operator array.
 *
 * The elements of the operator array are appended by this function.
 */
static void
mongo_append_column_name(Var *column, BSON *child_doc, pipeline_cxt *context)
{
	bool		found = false;
	ColInfoHashKey key;
	ColInfoHashEntry *columnInfo;
	char	   *field;

	key.varNo = column->varno;
	key.varAttno = column->varattno;

	columnInfo = (ColInfoHashEntry *) hash_search(context->colInfoHash,
												  (void *) &key,
												  HASH_FIND,
												  &found);
	if (!found)
		return;

	if (columnInfo->isOuter && context->isJoinClause)
		field = psprintf("$$v_%s", columnInfo->colName);
	else
		field = psprintf("$%s", columnInfo->colName);

	bsonAppendUTF8(child_doc, psprintf("%d", context->arrayIndex), field);
}

/*
 * mongo_add_null_check
 *		Eliminate null value rows of columns involved in the join and WHERE
 *		clauses.
 */
static void
mongo_add_null_check(Var *column, BSON *expr, pipeline_cxt *context)
{
	BSON		ne_expr;
	bool		found = false;
	ColInfoHashKey key;
	ColInfoHashEntry *columnInfo;
	char	   *field;

	key.varNo = column->varno;
	key.varAttno = column->varattno;

	columnInfo = (ColInfoHashEntry *) hash_search(context->colInfoHash,
												  (void *) &key,
												  HASH_FIND,
												  &found);
	if (!found)
		return;

	if (columnInfo->isOuter && context->isJoinClause)
		field = psprintf("$$v_%s", columnInfo->colName);
	else
		field = psprintf("$%s", columnInfo->colName);

	bsonAppendStartArray(expr, "$ne", &ne_expr);
	bsonAppendUTF8(&ne_expr, "0", field);
	bsonAppendNull(&ne_expr, "1");
	bsonAppendFinishArray(expr, &ne_expr);
}

/*
 * mongo_is_foreign_pathkey
 *		Returns true if it's safe to push down the sort expression described by
 *		'pathkey' to the foreign server.
 */
bool
mongo_is_foreign_pathkey(PlannerInfo *root, RelOptInfo *baserel,
						 PathKey *pathkey)
{
	EquivalenceMember *em;
	EquivalenceClass *pathkey_ec = pathkey->pk_eclass;
	Expr	   *em_expr;

	/*
	 * mongo_is_foreign_expr would detect volatile expressions as well, but
	 * checking ec_has_volatile here saves some cycles.
	 */
	if (pathkey_ec->ec_has_volatile)
		return false;

	/* can push if a suitable EC member exists */
	if (!(em = mongo_find_em_for_rel(root, pathkey_ec, baserel)))
		return false;

	/* Ignore binary-compatible relabeling */
	em_expr = em->em_expr;
	while (em_expr && IsA(em_expr, RelabelType))
		em_expr = ((RelabelType *) em_expr)->arg;

	/* Only Vars are allowed per MongoDB. */
	if (!IsA(em_expr, Var))
		return false;

	/* Check for sort operator pushability. */
	if (!mongo_is_default_sort_operator(em, pathkey))
		return false;

	return true;
}

/*
 * mongo_is_builtin
 *		Return true if given object is one of PostgreSQL's built-in objects.
 *
 * We use FirstBootstrapObjectId as the cutoff, so that we only consider
 * objects with hand-assigned OIDs to be "built in", not for instance any
 * function or type defined in the information_schema.
 *
 * Our constraints for dealing with types are tighter than they are for
 * functions or operators: we want to accept only types that are in pg_catalog,
 * else format_type might incorrectly fail to schema-qualify their names.
 * (This could be fixed with some changes to format_type, but for now there's
 * no need.)  Thus we must exclude information_schema types.
 *
 * XXX there is a problem with this, which is that the set of built-in
 * objects expands over time.  Something that is built-in to us might not
 * be known to the remote server, if it's of an older version.  But keeping
 * track of that would be a huge exercise.
 */
bool
mongo_is_builtin(Oid oid)
{
#if PG_VERSION_NUM >= 120000
	return (oid < FirstGenbkiObjectId);
#else
	return (oid < FirstBootstrapObjectId);
#endif
}
