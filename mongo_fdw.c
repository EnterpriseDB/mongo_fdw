/*-------------------------------------------------------------------------
 *
 * mongo_fdw.c
 * 		Foreign-data wrapper for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2024, EnterpriseDB Corporation.
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		mongo_fdw.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"
#include "mongo_wrapper.h"

#include "access/htup_details.h"
#if PG_VERSION_NUM < 120000
#include "access/sysattr.h"
#endif
#if PG_VERSION_NUM >= 120000
#include "access/table.h"
#endif
#include "catalog/heap.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#if PG_VERSION_NUM >= 130000
#include "common/hashfn.h"
#include "common/jsonapi.h"
#endif
#include "miscadmin.h"
#include "mongo_fdw.h"
#include "mongo_query.h"
#include "nodes/nodeFuncs.h"
#if PG_VERSION_NUM >= 140000
#include "optimizer/appendinfo.h"
#endif
#if PG_VERSION_NUM >= 120000
#include "optimizer/optimizer.h"
#endif
#include "optimizer/paths.h"
#include "optimizer/tlist.h"
#if PG_VERSION_NUM < 120000
#include "optimizer/var.h"
#endif
#include "parser/parsetree.h"
#if PG_VERSION_NUM >= 160000
#include "parser/parse_relation.h"
#endif
#include "storage/ipc.h"
#include "utils/guc.h"
#include "utils/jsonb.h"
#if PG_VERSION_NUM < 130000
#include "utils/jsonapi.h"
#else
#include "utils/jsonfuncs.h"
#endif
#include "utils/rel.h"
#include "utils/selfuncs.h"
#include "utils/syscache.h"
#include "utils/typcache.h"

/* Declarations for dynamic loading */
PG_MODULE_MAGIC;

/*
 * In PG 9.5.1 the number will be 90501,
 * our version is 5.5.1 so number will be 50501
 */
#define CODE_VERSION   50501

/*
 * Macro to check unsupported sorting methods.  Currently, ASC NULLS FIRST and
 * DESC NULLS LAST give the same sorting result on MongoDB and Postgres.  So,
 * sorting methods other than these are not pushed down.
 */
#define IS_PATHKEY_PUSHABLE(pathkey) \
	((pathkey->pk_strategy == BTLessStrategyNumber && pathkey->pk_nulls_first) || \
	 (pathkey->pk_strategy != BTLessStrategyNumber && !pathkey->pk_nulls_first))

/* Maximum path keys supported by MongoDB */
#define MAX_PATHKEYS			32

/*
 * The number of rows in a foreign relation are estimated to be so less that
 * an in-memory sort on those many rows wouldn't cost noticeably higher than
 * the underlying scan.  Hence for now, cost sorts same as underlying scans.
 */
#define DEFAULT_MONGO_SORT_MULTIPLIER 1

/* GUC variables. */
static bool enable_join_pushdown = true;
static bool enable_order_by_pushdown = true;
static bool enable_aggregate_pushdown = true;

/*
 * This enum describes what's kept in the fdw_private list for a ForeignPath.
 * We store:
 *
 * 1) Boolean flag showing if the remote query has the final sort
 * 2) Boolean flag showing if the remote query has the LIMIT clause
 */
enum FdwPathPrivateIndex
{
	/* has-final-sort flag (as an integer Value node) */
	FdwPathPrivateHasFinalSort,
	/* has-limit flag (as an integer Value node) */
	FdwPathPrivateHasLimit
};

extern PGDLLEXPORT void _PG_init(void);

PG_FUNCTION_INFO_V1(mongo_fdw_handler);
PG_FUNCTION_INFO_V1(mongo_fdw_version);

/* FDW callback routines */
static void mongoGetForeignRelSize(PlannerInfo *root,
								   RelOptInfo *baserel,
								   Oid foreigntableid);
static void mongoGetForeignPaths(PlannerInfo *root,
								 RelOptInfo *baserel,
								 Oid foreigntableid);
static ForeignScan *mongoGetForeignPlan(PlannerInfo *root,
										RelOptInfo *foreignrel,
										Oid foreigntableid,
										ForeignPath *best_path,
										List *targetlist,
										List *restrictionClauses,
										Plan *outer_plan);
static void mongoExplainForeignScan(ForeignScanState *node, ExplainState *es);
static void mongoBeginForeignScan(ForeignScanState *node, int eflags);
static TupleTableSlot *mongoIterateForeignScan(ForeignScanState *node);
static void mongoEndForeignScan(ForeignScanState *node);
static void mongoReScanForeignScan(ForeignScanState *node);
static TupleTableSlot *mongoExecForeignUpdate(EState *estate,
											  ResultRelInfo *resultRelInfo,
											  TupleTableSlot *slot,
											  TupleTableSlot *planSlot);
static TupleTableSlot *mongoExecForeignDelete(EState *estate,
											  ResultRelInfo *resultRelInfo,
											  TupleTableSlot *slot,
											  TupleTableSlot *planSlot);
static void mongoEndForeignModify(EState *estate,
								  ResultRelInfo *resultRelInfo);
#if PG_VERSION_NUM >= 140000
static void mongoAddForeignUpdateTargets(PlannerInfo *root,
										 Index rtindex,
										 RangeTblEntry *target_rte,
										 Relation target_relation);
#else
static void mongoAddForeignUpdateTargets(Query *parsetree,
										 RangeTblEntry *target_rte,
										 Relation target_relation);
#endif
static void mongoBeginForeignModify(ModifyTableState *mtstate,
									ResultRelInfo *resultRelInfo,
									List *fdw_private,
									int subplan_index,
									int eflags);
static TupleTableSlot *mongoExecForeignInsert(EState *estate,
											  ResultRelInfo *resultRelInfo,
											  TupleTableSlot *slot,
											  TupleTableSlot *planSlot);
static List *mongoPlanForeignModify(PlannerInfo *root,
									ModifyTable *plan,
									Index resultRelation,
									int subplan_index);
static void mongoExplainForeignModify(ModifyTableState *mtstate,
									  ResultRelInfo *rinfo,
									  List *fdw_private,
									  int subplan_index,
									  ExplainState *es);
static bool mongoAnalyzeForeignTable(Relation relation,
									 AcquireSampleRowsFunc *func,
									 BlockNumber *totalpages);
static void mongoBeginForeignInsert(ModifyTableState *mtstate,
									ResultRelInfo *resultRelInfo);
static void mongoEndForeignInsert(EState *estate,
								  ResultRelInfo *resultRelInfo);
static void mongoGetForeignJoinPaths(PlannerInfo *root, RelOptInfo *joinrel,
									 RelOptInfo *outerrel,
									 RelOptInfo *innerrel,
									 JoinType jointype,
									 JoinPathExtraData *extra);
static void mongoGetForeignUpperPaths(PlannerInfo *root,
									  UpperRelationKind stage,
									  RelOptInfo *input_rel,
									  RelOptInfo *output_rel,
									  void *extra);

/*
 * Helper functions
 */
static double foreign_table_document_count(Oid foreignTableId);
static HTAB *column_mapping_hash(Oid foreignTableId, List *columnList,
								 List *colNameList, List *colIsInnerList,
								 uint32 relType);
static void fill_tuple_slot(const BSON *bsonDocument,
							const char *bsonDocumentKey,
							HTAB *columnMappingHash,
							Datum *columnValues,
							bool *columnNulls,
							uint32 relType);
static bool column_types_compatible(BSON_TYPE bsonType, Oid columnTypeId);
static Datum column_value_array(BSON_ITERATOR *bsonIterator, Oid valueTypeId);
static Datum column_value(BSON_ITERATOR *bsonIterator,
						  Oid columnTypeId,
						  int32 columnTypeMod);
static void mongo_free_scan_state(MongoFdwModifyState *fmstate);
static int	mongo_acquire_sample_rows(Relation relation,
									  int errorLevel,
									  HeapTuple *sampleRows,
									  int targetRowCount,
									  double *totalRowCount,
									  double *totalDeadRowCount);
static void mongo_fdw_exit(int code, Datum arg);
static bool mongo_foreign_join_ok(PlannerInfo *root, RelOptInfo *joinrel,
								  JoinType jointype, RelOptInfo *outerrel,
								  RelOptInfo *innerrel,
								  JoinPathExtraData *extra);
static void mongo_prepare_qual_info(List *quals, MongoRelQualInfo *qual_info);
static bool mongo_foreign_grouping_ok(PlannerInfo *root,
									  RelOptInfo *grouped_rel,
									  Node *havingQual);
static void mongo_add_foreign_grouping_paths(PlannerInfo *root,
											 RelOptInfo *input_rel,
											 RelOptInfo *grouped_rel,
											 GroupPathExtraData *extra);
#if PG_VERSION_NUM >= 120000
static void mongo_add_foreign_final_paths(PlannerInfo *root,
										  RelOptInfo *input_rel,
										  RelOptInfo *final_rel,
										  FinalPathExtraData *extra);
#endif
static void mongoEstimateCosts(RelOptInfo *baserel, Cost *startup_cost,
							   Cost *total_cost, Oid foreigntableid);

static List *mongo_get_useful_ecs_for_relation(PlannerInfo *root,
											   RelOptInfo *rel);
static List *mongo_get_useful_pathkeys_for_relation(PlannerInfo *root,
													RelOptInfo *rel);
static void mongo_add_paths_with_pathkeys(PlannerInfo *root,
										  RelOptInfo *rel,
										  Path *epq_path,
										  Cost base_startup_cost,
										  Cost base_total_cost);
static EquivalenceMember *mongo_find_em_for_rel_target(PlannerInfo *root,
													   EquivalenceClass *ec,
													   RelOptInfo *rel);
#if PG_VERSION_NUM >= 120000
static void mongo_add_foreign_ordered_paths(PlannerInfo *root,
											RelOptInfo *input_rel,
											RelOptInfo *ordered_rel);
#endif

/* The null action object used for pure validation */
#if PG_VERSION_NUM < 130000
static JsonSemAction nullSemAction =
{
	NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL
};
#else
JsonSemAction nullSemAction =
{
	NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL
};
#endif

/*
 * Library load-time initalization, sets on_proc_exit() callback for
 * backend shutdown.
 */
void
_PG_init(void)
{
	/*
	 * Sometimes getting a join or sorted result from MongoDB server is slower
	 * than performing those operations locally.  To have that flexibility add
	 * a few GUCs to control those push-downs.
	 */
	DefineCustomBoolVariable("mongo_fdw.enable_join_pushdown",
							 "enable/disable join pushdown",
							 NULL,
							 &enable_join_pushdown,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("mongo_fdw.enable_order_by_pushdown",
							 "Enable/Disable ORDER BY push down",
							 NULL,
							 &enable_order_by_pushdown,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	DefineCustomBoolVariable("mongo_fdw.enable_aggregate_pushdown",
							 "Enable/Disable aggregate push down",
							 NULL,
							 &enable_aggregate_pushdown,
							 true,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);

	/* Initialize MongoDB C driver */
	mongoc_init();

	on_proc_exit(&mongo_fdw_exit, PointerGetDatum(NULL));
}

/*
 * mongo_fdw_handler
 *		Creates and returns a struct with pointers to foreign table callback
 *		functions.
 */
Datum
mongo_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwRoutine = makeNode(FdwRoutine);

	/* Functions for scanning foreign tables */
	fdwRoutine->GetForeignRelSize = mongoGetForeignRelSize;
	fdwRoutine->GetForeignPaths = mongoGetForeignPaths;
	fdwRoutine->GetForeignPlan = mongoGetForeignPlan;
	fdwRoutine->BeginForeignScan = mongoBeginForeignScan;
	fdwRoutine->IterateForeignScan = mongoIterateForeignScan;
	fdwRoutine->ReScanForeignScan = mongoReScanForeignScan;
	fdwRoutine->EndForeignScan = mongoEndForeignScan;

	/* Support for insert/update/delete */
	fdwRoutine->AddForeignUpdateTargets = mongoAddForeignUpdateTargets;
	fdwRoutine->PlanForeignModify = mongoPlanForeignModify;
	fdwRoutine->BeginForeignModify = mongoBeginForeignModify;
	fdwRoutine->ExecForeignInsert = mongoExecForeignInsert;
	fdwRoutine->ExecForeignUpdate = mongoExecForeignUpdate;
	fdwRoutine->ExecForeignDelete = mongoExecForeignDelete;
	fdwRoutine->EndForeignModify = mongoEndForeignModify;

	/* Support for EXPLAIN */
	fdwRoutine->ExplainForeignScan = mongoExplainForeignScan;
	fdwRoutine->ExplainForeignModify = mongoExplainForeignModify;

	/* Support for ANALYZE */
	fdwRoutine->AnalyzeForeignTable = mongoAnalyzeForeignTable;

	/* Partition routing and/or COPY from */
	fdwRoutine->BeginForeignInsert = mongoBeginForeignInsert;
	fdwRoutine->EndForeignInsert = mongoEndForeignInsert;

	/* Support function for join push-down */
	fdwRoutine->GetForeignJoinPaths = mongoGetForeignJoinPaths;

	/* Support functions for upper relation push-down */
	fdwRoutine->GetForeignUpperPaths = mongoGetForeignUpperPaths;

	PG_RETURN_POINTER(fdwRoutine);
}

/*
 * mongo_fdw_exit
 *		Exit callback function.
 */
static void
mongo_fdw_exit(int code, Datum arg)
{
	mongo_cleanup_connection();
	/* Release all memory and other resources allocated by the driver */
	mongoc_cleanup();
}

/*
 * MongoGetForeignRelSize
 * 		Obtains relation size estimates for mongo foreign table.
 */
static void
mongoGetForeignRelSize(PlannerInfo *root,
					   RelOptInfo *baserel,
					   Oid foreigntableid)
{
	RangeTblEntry *rte = planner_rt_fetch(baserel->relid, root);
	MongoFdwRelationInfo *fpinfo;
	MongoFdwOptions *options;
	ListCell   *lc;
	char	   *relname;
	char	   *database;
	char	   *refname;

	/*
	 * We use MongoFdwRelationInfo to pass various information to subsequent
	 * functions.
	 */
	fpinfo = (MongoFdwRelationInfo *) palloc0(sizeof(MongoFdwRelationInfo));
	baserel->fdw_private = (void *) fpinfo;

	/*
	 * Identify which baserestrictinfo clauses can be sent to the remote
	 * server and which can't.  Only the OpExpr clauses are sent to the remote
	 * server.
	 */
	foreach(lc, baserel->baserestrictinfo)
	{
		RestrictInfo *ri = (RestrictInfo *) lfirst(lc);

		if (mongo_is_foreign_expr(root, baserel, ri->clause, false))
			fpinfo->remote_conds = lappend(fpinfo->remote_conds, ri);
		else
			fpinfo->local_conds = lappend(fpinfo->local_conds, ri);
	}

	/* Base foreign tables need to be pushed down always. */
	fpinfo->pushdown_safe = true;

	/* Fetch options */
	options = mongo_get_options(foreigntableid);

	/*
	 * Retrieve exact document count for remote collection if asked,
	 * otherwise, use default estimate in planning.
	 */
	if (options->use_remote_estimate)
	{
		double		documentCount = foreign_table_document_count(foreigntableid);

		if (documentCount > 0.0)
		{
			double		rowSelectivity;

			/*
			 * We estimate the number of rows returned after restriction
			 * qualifiers are applied.  This will be more accurate if analyze
			 * is run on this relation.
			 */
			rowSelectivity = clauselist_selectivity(root,
													baserel->baserestrictinfo,
													0, JOIN_INNER, NULL);
			baserel->rows = clamp_row_est(documentCount * rowSelectivity);
		}
		else
			ereport(DEBUG1,
					(errmsg("could not retrieve document count for collection"),
					 errhint("Falling back to default estimates in planning.")));
	}

	relname = options->collectionName;
	database = options->svr_database;
	fpinfo->base_relname = relname;

	/*
	 * Set the name of relation in fpinfo, while we are constructing it here.
	 * It will be used to build the string describing the join relation in
	 * EXPLAIN output.  We can't know whether the VERBOSE option is specified
	 * or not, so always schema-qualify the foreign table name.
	 */
	fpinfo->relation_name = makeStringInfo();
	refname = rte->eref->aliasname;
	appendStringInfo(fpinfo->relation_name, "%s.%s",
					 quote_identifier(database),
					 quote_identifier(relname));
	if (*refname && strcmp(refname, relname) != 0)
		appendStringInfo(fpinfo->relation_name, " %s",
						 quote_identifier(rte->eref->aliasname));

	/* Also store the options in fpinfo for further use */
	fpinfo->options = options;

	/*
	 * Store aggregation enable/disable option in the fpinfo directly for
	 * further use.  This flag can be useful when options are not accessible
	 * in the recursive cases.
	 */
	fpinfo->is_agg_scanrel_pushable = options->enable_aggregate_pushdown;

	/* Set the flag is_order_by_pushable of the base relation */
	fpinfo->is_order_by_pushable = options->enable_order_by_pushdown;
}

/*
 * mongoGetForeignPaths
 *		Creates the only scan path used to execute the query.
 *
 * Note that MongoDB may decide to use an underlying index for this scan, but
 * that decision isn't deterministic or visible to us.  We therefore create a
 * single table scan path.
 */
static void
mongoGetForeignPaths(PlannerInfo *root,
					 RelOptInfo *baserel,
					 Oid foreigntableid)
{
	Path	   *foreignPath;
	MongoFdwOptions *options;
	Cost		startupCost;
	Cost		totalCost;

	/* Fetch options */
	options = mongo_get_options(foreigntableid);

	/*
	 * Retrieve exact document count for remote collection if asked,
	 * otherwise, use default estimate in planning.
	 */
	if (options->use_remote_estimate)
	{
		double		documentCount = foreign_table_document_count(foreigntableid);

		if (documentCount > 0.0)
		{
			MongoFdwRelationInfo *fpinfo = (MongoFdwRelationInfo *) baserel->fdw_private;
			double		tupleFilterCost = baserel->baserestrictcost.per_tuple;
			double		inputRowCount;
			double		documentSelectivity;
			double		foreignTableSize;
			int32		documentWidth;
			BlockNumber pageCount;
			double		totalDiskAccessCost;
			double		cpuCostPerDoc;
			double		cpuCostPerRow;
			double		totalCpuCost;
			double		connectionCost;
			List	   *opExpressionList;

			/*
			 * We estimate the number of rows returned after restriction
			 * qualifiers are applied by MongoDB.
			 */
			opExpressionList = fpinfo->remote_conds;
			documentSelectivity = clauselist_selectivity(root,
														 opExpressionList, 0,
														 JOIN_INNER, NULL);
			inputRowCount = clamp_row_est(documentCount * documentSelectivity);

			/*
			 * We estimate disk costs assuming a sequential scan over the
			 * data. This is an inaccurate assumption as Mongo scatters the
			 * data over disk pages, and may rely on an index to retrieve the
			 * data. Still, this should at least give us a relative cost.
			 */
			documentWidth = get_relation_data_width(foreigntableid,
													baserel->attr_widths);
			foreignTableSize = documentCount * documentWidth;

			pageCount = (BlockNumber) rint(foreignTableSize / BLCKSZ);
			totalDiskAccessCost = seq_page_cost * pageCount;

			/*
			 * The cost of processing a document returned by Mongo (input row)
			 * is 5x the cost of processing a regular row.
			 */
			cpuCostPerDoc = cpu_tuple_cost;
			cpuCostPerRow = (cpu_tuple_cost * MONGO_TUPLE_COST_MULTIPLIER) + tupleFilterCost;
			totalCpuCost = (cpuCostPerDoc * documentCount) + (cpuCostPerRow * inputRowCount);

			connectionCost = MONGO_CONNECTION_COST_MULTIPLIER * seq_page_cost;
			startupCost = baserel->baserestrictcost.startup + connectionCost;
			totalCost = startupCost + totalDiskAccessCost + totalCpuCost;
		}
		else
			ereport(DEBUG1,
					(errmsg("could not retrieve document count for collection"),
					 errhint("Falling back to default estimates in planning.")));
	}
	else
	{
		/* Estimate default costs */
		mongoEstimateCosts(baserel, &startupCost, &totalCost, foreigntableid);
	}

	/* Create a foreign path node */
	foreignPath = (Path *) create_foreignscan_path(root, baserel,
												   NULL,	/* default pathtarget */
												   baserel->rows,
												   startupCost,
												   totalCost,
												   NIL, /* no pathkeys */
												   baserel->lateral_relids,
												   NULL,	/* no extra plan */
												   NULL);	/* no fdw_private data */

	/* Add foreign path as the only possible path */
	add_path(baserel, foreignPath);

	/* Add paths with pathkeys */
	mongo_add_paths_with_pathkeys(root, baserel, NULL, startupCost, totalCost);
}

/*
 * mongoGetForeignPlan
 *		Creates a foreign scan plan node for scanning the MongoDB collection.
 *
 * Note that MongoDB may decide to use an underlying index for this
 * scan, but that decision isn't deterministic or visible to us.
 */
static ForeignScan *
mongoGetForeignPlan(PlannerInfo *root,
					RelOptInfo *foreignrel,
					Oid foreigntableid,
					ForeignPath *best_path,
					List *targetList,
					List *restrictionClauses,
					Plan *outer_plan)
{
	MongoFdwRelationInfo *fpinfo = (MongoFdwRelationInfo *) foreignrel->fdw_private;
	Index		scan_relid = foreignrel->relid;
	ForeignScan *foreignScan;
	List	   *fdw_private;
	List	   *columnList;
	List	   *scan_var_list;
	ListCell   *lc;
	List	   *local_exprs = NIL;
	List	   *remote_exprs = NIL;
	List	   *fdw_scan_tlist = NIL;
	List	   *column_name_list = NIL;
	List	   *is_inner_column_list = NIL;
	List	   *quals = NIL;
	MongoFdwRelType mongofdwreltype;
	MongoRelQualInfo *qual_info;
	MongoFdwRelationInfo *ofpinfo;
	List	   *pathKeyList = NIL;
	List	   *isAscSortList = NIL;
	bool		has_final_sort = false;
	bool		has_limit = false;
	int64		limit_value;
	int64		offset_value;

	/*
	 * Get FDW private data created by mongoGetForeignUpperPaths(), if any.
	 */
	if (best_path->fdw_private)
	{
		has_final_sort = intVal(list_nth(best_path->fdw_private,
										 FdwPathPrivateHasFinalSort));
		has_limit = intVal(list_nth(best_path->fdw_private,
									FdwPathPrivateHasLimit));
	}

	/* Set scan relation id */
	if (IS_SIMPLE_REL(foreignrel))
		scan_relid = foreignrel->relid;
	else
	{
		/* Join/Upper relation - set scan_relid to 0. */
		scan_relid = 0;

		Assert(!restrictionClauses);

		/* Extract local expressions from local conditions */
		foreach(lc, fpinfo->local_conds)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			Assert(IsA(rinfo, RestrictInfo));
			local_exprs = lappend(local_exprs, rinfo->clause);
		}

		/* Extract remote expressions from remote conditions */
		foreach(lc, fpinfo->remote_conds)
		{
			RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

			Assert(IsA(rinfo, RestrictInfo));
			remote_exprs = lappend(remote_exprs, rinfo->clause);
		}
	}

	if (IS_UPPER_REL(foreignrel))
		scan_var_list = pull_var_clause((Node *) fpinfo->grouped_tlist,
										PVC_RECURSE_AGGREGATES);
	else
		scan_var_list = pull_var_clause((Node *) foreignrel->reltarget->exprs,
										PVC_RECURSE_PLACEHOLDERS);

	/* System attributes are not allowed. */
	foreach(lc, scan_var_list)
	{
		Var		   *var = lfirst(lc);
		const FormData_pg_attribute *attr;

		Assert(IsA(var, Var));

		if (var->varattno >= 0)
			continue;

#if PG_VERSION_NUM >= 120000
		attr = SystemAttributeDefinition(var->varattno);
#else
		attr = SystemAttributeDefinition(var->varattno, false);
#endif
		ereport(ERROR,
				(errcode(ERRCODE_FDW_COLUMN_NAME_NOT_FOUND),
				 errmsg("system attribute \"%s\" can't be fetched from remote relation",
						attr->attname.data)));
	}

	/*
	 * Separate the restrictionClauses into those that can be executed
	 * remotely and those that can't.  baserestrictinfo clauses that were
	 * previously determined to be safe or unsafe are shown in
	 * fpinfo->remote_conds and fpinfo->local_conds.  Anything else in the
	 * restrictionClauses list will be a join clause, which we have to check
	 * for remote-safety.  Only the OpExpr clauses are sent to the remote
	 * server.
	 */
	foreach(lc, restrictionClauses)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

		Assert(IsA(rinfo, RestrictInfo));

		/* Ignore pseudoconstants, they are dealt with elsewhere */
		if (rinfo->pseudoconstant)
			continue;

		if (list_member_ptr(fpinfo->remote_conds, rinfo))
			remote_exprs = lappend(remote_exprs, rinfo->clause);
		else if (list_member_ptr(fpinfo->local_conds, rinfo))
			local_exprs = lappend(local_exprs, rinfo->clause);
		else if (IsA(rinfo->clause, OpExpr) &&
				 mongo_is_foreign_expr(root, foreignrel, rinfo->clause, false))
			remote_exprs = lappend(remote_exprs, rinfo->clause);
		else
			local_exprs = lappend(local_exprs, rinfo->clause);
	}

	/* Add local expression Var nodes to scan_var_list. */
	scan_var_list = list_concat_unique(NIL, scan_var_list);
	if (IS_UPPER_REL(foreignrel))
		scan_var_list = list_concat_unique(scan_var_list,
										   pull_var_clause((Node *) local_exprs,
														   PVC_RECURSE_AGGREGATES));
	else
		scan_var_list = list_concat_unique(scan_var_list,
										   pull_var_clause((Node *) local_exprs,
														   PVC_RECURSE_PLACEHOLDERS));

	if (IS_JOIN_REL(foreignrel))
	{
		/*
		 * For join relations, the planner needs a targetlist, which
		 * represents the output of the ForeignScan node.
		 */
		fdw_scan_tlist = add_to_flat_tlist(NIL, scan_var_list);

		/*
		 * Ensure that the outer plan produces a tuple whose descriptor
		 * matches our scan tuple slot.  Also, remove the local conditions
		 * from the outer plan's quals, lest they be evaluated twice, once by
		 * the local plan and once by the scan.
		 */
		if (outer_plan)
		{
			/*
			 * First, update the plan's qual list if possible.  In some cases,
			 * the quals might be enforced below the topmost plan level, in
			 * which case we'll fail to remove them; it's not worth working
			 * harder than this.
			 */
			foreach(lc, local_exprs)
			{
				Node	   *qual = lfirst(lc);

				outer_plan->qual = list_delete(outer_plan->qual, qual);

				/*
				 * For an inner join, the local conditions of the foreign scan
				 * plan can be part of the joinquals as well.  (They might
				 * also be in the mergequals or hashquals, but we can't touch
				 * those without breaking the plan.)
				 */
				if (IsA(outer_plan, NestLoop) ||
					IsA(outer_plan, MergeJoin) ||
					IsA(outer_plan, HashJoin))
				{
					Join	   *join_plan = (Join *) outer_plan;

					if (join_plan->jointype == JOIN_INNER)
						join_plan->joinqual = list_delete(join_plan->joinqual,
														  qual);
				}
			}

			/*
			 * Now fix the subplan's tlist --- this might result in inserting
			 * a Result node atop the plan tree.
			 */
			outer_plan = change_plan_targetlist(outer_plan, fdw_scan_tlist,
												best_path->path.parallel_safe);
		}
	}
	else if (IS_UPPER_REL(foreignrel))
	{
		/*
		 * scan_var_list should have expressions and not TargetEntry nodes.
		 * However, grouped_tlist created has TLEs, and thus retrieve them
		 * into scan_var_list.
		 */
		scan_var_list = list_concat_unique(NIL,
										   get_tlist_exprs(fpinfo->grouped_tlist,
														   false));

		/*
		 * The targetlist computed while assessing push-down safety represents
		 * the result we expect from the foreign server.
		 */
		fdw_scan_tlist = fpinfo->grouped_tlist;
		local_exprs = extract_actual_clauses(fpinfo->local_conds, false);
	}

	/* Form column list required for query execution from scan_var_list. */
	columnList = mongo_get_column_list(root, foreignrel, scan_var_list,
									   &column_name_list,
									   &is_inner_column_list);


	/*
	 * Identify the relation type.  We can have a simple base rel, join rel,
	 * upper rel, and upper rel with join rel inside.  Find out that.
	 */
	if (IS_UPPER_REL(foreignrel) && IS_JOIN_REL(fpinfo->outerrel))
		mongofdwreltype = UPPER_JOIN_REL;
	else if (IS_UPPER_REL(foreignrel))
		mongofdwreltype = UPPER_REL;
	else if (IS_JOIN_REL(foreignrel))
		mongofdwreltype = JOIN_REL;
	else
		mongofdwreltype = BASE_REL;

	/*
	 * We use MongoRelQualInfo to pass various information related to joining
	 * quals and grouping target to fdw_private which is used to form
	 * equivalent MongoDB query during the execution phase.
	 */
	qual_info = (MongoRelQualInfo *) palloc(sizeof(MongoRelQualInfo));

	qual_info->root = root;
	qual_info->foreignRel = foreignrel;
	qual_info->exprColHash = NULL;
	qual_info->colNameList = NIL;
	qual_info->colNumList = NIL;
	qual_info->rtiList = NIL;
	qual_info->isOuterList = NIL;
	qual_info->is_having = false;
	qual_info->is_agg_column = false;
	qual_info->aggTypeList = NIL;
	qual_info->aggColList = NIL;
	qual_info->isHavingList = NIL;

	/*
	 * Prepare separate lists of information.  This information would be
	 * useful at the time of execution to prepare the MongoDB query.
	 */
	if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
	{
		ofpinfo = (MongoFdwRelationInfo *) fpinfo->outerrel->fdw_private;

		/*
		 * Save foreign relation and relid's of an outer relation involved in
		 * the join depending on the relation type.
		 */
		if (mongofdwreltype == UPPER_JOIN_REL)
		{
			/* For aggregation over join relation */
			qual_info->foreignRel = fpinfo->outerrel;
			qual_info->outerRelids = ofpinfo->outerrel->relids;
		}
		else if (mongofdwreltype == UPPER_REL)
		{
			/* For aggregation relation */
			qual_info->foreignRel = fpinfo->outerrel;
			qual_info->outerRelids = fpinfo->outerrel->relids;
		}
		else
		{
			Assert(mongofdwreltype == JOIN_REL);
			qual_info->foreignRel = foreignrel;
			qual_info->outerRelids = fpinfo->outerrel->relids;
		}

		/*
		 * Extract required data of columns involved in join clauses and
		 * append it into the various lists required to pass it to the
		 * executor.
		 *
		 * Check and extract data for outer relation and its join clauses in
		 * case of aggregation on top of the join operation.
		 */
		if (IS_JOIN_REL(foreignrel) && fpinfo->joinclauses)
			mongo_prepare_qual_info(fpinfo->joinclauses, qual_info);
		else if (IS_JOIN_REL(fpinfo->outerrel) && ofpinfo->joinclauses)
			mongo_prepare_qual_info(ofpinfo->joinclauses, qual_info);

		/*
		 * Extract required data of columns involved in the WHERE clause and
		 * append it into the various lists required to pass it to the
		 * executor.
		 */
		if (IS_JOIN_REL(foreignrel) && fpinfo->remote_conds)
			mongo_prepare_qual_info(fpinfo->remote_conds, qual_info);

		/* Gather required information of an upper relation */
		if (IS_UPPER_REL(foreignrel))
		{
			/* Extract remote expressions from the remote conditions */
			foreach(lc, ofpinfo->remote_conds)
			{
				RestrictInfo *rinfo = (RestrictInfo *) lfirst(lc);

				Assert(IsA(rinfo, RestrictInfo));
				quals = lappend(quals, rinfo->clause);
			}

			/* Extract WHERE clause column information */
			mongo_prepare_qual_info(quals, qual_info);

			/*
			 * Extract grouping target information i.e grouping operation and
			 * grouping clause.
			 */
			mongo_prepare_qual_info(scan_var_list, qual_info);

			/* Extract HAVING clause information */
			if (fpinfo->remote_conds)
			{
				qual_info->is_having = true;
				mongo_prepare_qual_info(fpinfo->remote_conds, qual_info);
			}
		}
		else
			quals = remote_exprs;
	}
	else
	{
		quals = remote_exprs;

		/* For baserel */
		qual_info->foreignRel = foreignrel;
		qual_info->outerRelids = NULL;

		/*
		 * Extract required data of columns involved in WHERE clause of the
		 * simple relation.
		 */
		mongo_prepare_qual_info(quals, qual_info);
	}

	/*
	 * Check the ORDER BY clause, and if we found any useful pathkeys, then
	 * store the required information.
	 */
	foreach(lc, best_path->path.pathkeys)
	{
		EquivalenceMember *em;
		PathKey    *pathkey = lfirst(lc);
		Expr	   *em_expr;

		if (has_final_sort)
		{
			/*
			 * By construction, foreignrel is the input relation to the final
			 * sort.
			 */
			em = mongo_find_em_for_rel_target(root, pathkey->pk_eclass,
											  foreignrel);
		}
		else
			em = mongo_find_em_for_rel(root, pathkey->pk_eclass,
									   qual_info->foreignRel);

		/*
		 * We don't expect any error here; it would mean that shippability
		 * wasn't verified earlier.  For the same reason, we don't recheck
		 * shippability of the sort operator.
		 */
		if (em == NULL)
			elog(ERROR, "could not find pathkey item to sort");

		/* Ignore binary-compatible relabeling */
		em_expr = em->em_expr;
		while (IsA(em_expr, RelabelType))
			em_expr = ((RelabelType *) em_expr)->arg;

		Assert(IsA(em_expr, Var));
		pathKeyList = list_append_unique(pathKeyList, (Var *) em_expr);

		if (pathkey->pk_strategy == BTLessStrategyNumber)
			isAscSortList = lappend_int(isAscSortList, 1);
		else
			isAscSortList = lappend_int(isAscSortList, -1);
	}

	/* Extract the required data of columns involved in the ORDER BY clause */
	mongo_prepare_qual_info(pathKeyList, qual_info);

	/* Destroy hash table used to get unique column info */
	hash_destroy(qual_info->exprColHash);

	/*
	 * Retrieve limit and offset values, which needs to be passed to the
	 * executor.  If any of the two clauses (limit or offset) is missing from
	 * the query, then default value -1 is used to indicate the same.
	 */
	limit_value = offset_value = -1;
	if (has_limit)
	{
		Node	   *node;

		node = root->parse->limitCount;
		if (node)
		{
			Assert(nodeTag(node) == T_Const &&
				   ((Const *) node)->consttype == INT8OID);

			/* Treat NULL as no limit */
			if (!((Const *) node)->constisnull)
				limit_value = DatumGetInt64(((Const *) node)->constvalue);
		}

		node = root->parse->limitOffset;
		if (node)
		{
			Assert(nodeTag(node) == T_Const &&
				   ((Const *) node)->consttype == INT8OID);

			/* Treat NULL as no offset */
			if (!((Const *) node)->constisnull)
				offset_value = DatumGetInt64(((Const *) node)->constvalue);
		}
	}

	/*
	 * Unlike postgres_fdw, remote query formation is done in the execution
	 * state.  There is NO way to get the correct information required to form
	 * a remote query during the execution state.  So, we are gathering
	 * information required to form a MongoDB query in the planning state and
	 * passing it to the execution state through fdw_private.
	 */

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum mongoFdwScanPrivateIndex.
	 */
	fdw_private = list_make2(columnList, quals);

	/* Append relation type */
	fdw_private = lappend(fdw_private, makeInteger(mongofdwreltype));

	fdw_private = lappend(fdw_private, qual_info->colNameList);
	fdw_private = lappend(fdw_private, qual_info->colNumList);
	fdw_private = lappend(fdw_private, qual_info->rtiList);
	fdw_private = lappend(fdw_private, qual_info->isOuterList);
	fdw_private = lappend(fdw_private, pathKeyList);
	fdw_private = lappend(fdw_private, isAscSortList);
	fdw_private = lappend(fdw_private, makeInteger(has_limit));
	fdw_private = lappend(fdw_private, makeInteger(limit_value));
	fdw_private = lappend(fdw_private, makeInteger(offset_value));

	if (IS_JOIN_REL(foreignrel) || IS_UPPER_REL(foreignrel))
	{
		MongoFdwRelationInfo *tfpinfo = NULL;

		fdw_private = lappend(fdw_private, qual_info->aggTypeList);
		fdw_private = lappend(fdw_private, qual_info->aggColList);
		fdw_private = lappend(fdw_private, ofpinfo->groupbyColList);
		fdw_private = lappend(fdw_private, remote_exprs);
		fdw_private = lappend(fdw_private, qual_info->isHavingList);
		fdw_private = lappend(fdw_private,
							  makeString(fpinfo->relation_name->data));
		fdw_private = lappend(fdw_private, column_name_list);
		fdw_private = lappend(fdw_private, is_inner_column_list);

		if (mongofdwreltype == JOIN_REL)
			tfpinfo = fpinfo;
		else if (mongofdwreltype == UPPER_JOIN_REL)
			tfpinfo = ofpinfo;

		if (tfpinfo)
		{
			fdw_private = lappend(fdw_private,
								  list_make2(makeString(tfpinfo->inner_relname),
											 makeString(tfpinfo->outer_relname)));
			fdw_private = lappend(fdw_private, tfpinfo->joinclauses);
			fdw_private = lappend(fdw_private, makeInteger(tfpinfo->jointype));
		}
	}

	/* Create the foreign scan node */
	foreignScan = make_foreignscan(targetList, local_exprs,
								   scan_relid,
								   NIL, /* No expressions to evaluate */
								   fdw_private
								   ,fdw_scan_tlist
								   ,NIL
								   ,outer_plan
		);

	return foreignScan;
}

/*
 * mongoExplainForeignScan
 *		Produces extra output for the Explain command.
 */
static void
mongoExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	RangeTblEntry *rte;
	EState	   *estate = node->ss.ps.state;
	List	   *fdw_private = fsplan->fdw_private;
	int			rtindex;

	if (fsplan->scan.scanrelid > 0)
		rtindex = fsplan->scan.scanrelid;
	else
#if PG_VERSION_NUM >= 160000
		rtindex = bms_next_member(fsplan->fs_base_relids, -1);
#else
		rtindex = bms_next_member(fsplan->fs_relids, -1);
#endif
	rte = rt_fetch(rtindex, estate->es_range_table);

	if (list_length(fdw_private) > mongoFdwPrivateRelations)
	{
		char	   *relations = strVal(list_nth(fdw_private,
												mongoFdwPrivateRelations));

		ExplainPropertyText("Foreign Namespace", relations, es);
	}
	else
	{
		StringInfo	namespaceName;
		MongoFdwOptions *options;

		options = mongo_get_options(rte->relid);

		/* Construct fully qualified collection name */
		namespaceName = makeStringInfo();
		appendStringInfo(namespaceName, "%s.%s", options->svr_database,
						 options->collectionName);
		ExplainPropertyText("Foreign Namespace", namespaceName->data, es);
		mongo_free_options(options);
	}
}

static void
mongoExplainForeignModify(ModifyTableState *mtstate,
						  ResultRelInfo *rinfo,
						  List *fdw_private,
						  int subplan_index,
						  ExplainState *es)
{
	MongoFdwOptions *options;
	StringInfo	namespaceName;
	Oid			foreignTableId;

	foreignTableId = RelationGetRelid(rinfo->ri_RelationDesc);
	options = mongo_get_options(foreignTableId);

	/* Construct fully qualified collection name */
	namespaceName = makeStringInfo();
	appendStringInfo(namespaceName, "%s.%s", options->svr_database,
					 options->collectionName);

	mongo_free_options(options);
	ExplainPropertyText("Foreign Namespace", namespaceName->data, es);
}

/*
 * mongoBeginForeignScan
 *		Connects to the MongoDB server, and opens a cursor that uses the
 *		database name, collection name, and the remote query to send to the
 *		server.
 *
 * The function also creates a hash table that maps referenced
 * column names to column index and type information.
 */
static void
mongoBeginForeignScan(ForeignScanState *node, int eflags)
{
	MONGO_CONN *mongoConnection;
	List	   *columnList;
	HTAB	   *columnMappingHash;
	MongoFdwOptions *options;
	MongoFdwModifyState *fmstate;
	RangeTblEntry *rte;
	EState	   *estate = node->ss.ps.state;
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;
	List	   *fdw_private = fsplan->fdw_private;
	Oid			userid;
	ForeignServer *server;
	UserMapping *user;
	ForeignTable *table;
	int			rtindex;
	List	   *colNameList = NIL;
	List	   *colIsInnerList = NIL;

	/* If Explain with no Analyze, do nothing */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	fmstate = (MongoFdwModifyState *) palloc0(sizeof(MongoFdwModifyState));

	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckRTEPerms() does. In the case of a join, use the
	 * lowest-numbered member RTE as a representative; we would get the same
	 * result from any.
	 */
	if (fsplan->scan.scanrelid > 0)
		rtindex = fsplan->scan.scanrelid;
	else
#if PG_VERSION_NUM >= 160000
		rtindex = bms_next_member(fsplan->fs_base_relids, -1);
#else
		rtindex = bms_next_member(fsplan->fs_relids, -1);
#endif

#if PG_VERSION_NUM >= 160000
	rte = exec_rt_fetch(rtindex, estate);
	userid = fsplan->checkAsUser ? fsplan->checkAsUser : GetUserId();
#else
	rte = rt_fetch(rtindex, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();
#endif

	/* Get info about foreign table. */
	fmstate->rel = node->ss.ss_currentRelation;
	table = GetForeignTable(rte->relid);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	options = mongo_get_options(rte->relid);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	mongoConnection = mongo_get_connection(server, user, options);

	columnList = list_nth(fdw_private, mongoFdwPrivateColumnList);
	fmstate->relType = intVal(list_nth(fdw_private, mongoFdwPrivateRelType));

	if (fmstate->relType == JOIN_REL || fmstate->relType == UPPER_JOIN_REL)
	{
		colNameList = list_nth(fdw_private, mongoFdwPrivateColNameList);
		colIsInnerList = list_nth(fdw_private, mongoFdwPrivateColIsInnerList);
	}

	columnMappingHash = column_mapping_hash(rte->relid, columnList,
											colNameList, colIsInnerList,
											fmstate->relType);

	/* Create and set foreign execution state */
	fmstate->columnMappingHash = columnMappingHash;
	fmstate->mongoConnection = mongoConnection;
	fmstate->options = options;

	node->fdw_state = (void *) fmstate;
}

/*
 * mongoIterateForeignScan
 *		Opens a Mongo cursor that uses the database name, collection name, and
 *		the remote query to send to the server.
 *
 *	Reads the next document from MongoDB, converts it to a PostgreSQL tuple,
 *	and stores the converted tuple into the ScanTupleSlot as a virtual tuple.
 */
static TupleTableSlot *
mongoIterateForeignScan(ForeignScanState *node)
{
	MongoFdwModifyState *fmstate = (MongoFdwModifyState *) node->fdw_state;
	TupleTableSlot *tupleSlot = node->ss.ss_ScanTupleSlot;
	MONGO_CURSOR *mongoCursor = fmstate->mongoCursor;
	HTAB	   *columnMappingHash = fmstate->columnMappingHash;
	TupleDesc	tupleDescriptor = tupleSlot->tts_tupleDescriptor;
	Datum	   *columnValues = tupleSlot->tts_values;
	bool	   *columnNulls = tupleSlot->tts_isnull;
	int32		columnCount = tupleDescriptor->natts;

	/* Create cursor for collection name and set query */
	if (mongoCursor == NULL)
	{
		BSON	   *queryDocument;
		char	   *collectionName;

		/*
		 * We construct the query document to have MongoDB filter its rows. We
		 * could also construct a column name document here to retrieve only
		 * the needed columns.  However, we found this optimization to degrade
		 * performance on the MongoDB server-side, so we instead filter out
		 * columns on our side.
		 */
		queryDocument = mongo_query_document(node);

		/*
		 * Decide input collection to the aggregation.  In case of join, outer
		 * relation should be given as input collection to the aggregation.
		 */
		if (fmstate->relType == JOIN_REL ||
			fmstate->relType == UPPER_JOIN_REL)
			collectionName = fmstate->outerRelName;
		else
			collectionName = fmstate->options->collectionName;

		mongoCursor = mongoCursorCreate(fmstate->mongoConnection,
										fmstate->options->svr_database,
										collectionName,
										queryDocument);

		/* Save mongoCursor */
		fmstate->mongoCursor = mongoCursor;
	}

	/*
	 * We execute the protocol to load a virtual tuple into a slot.  We first
	 * call ExecClearTuple, then fill in values / isnull arrays, and last call
	 * ExecStoreVirtualTuple.  If we are done fetching documents from Mongo,
	 * we just return an empty slot as required.
	 */
	ExecClearTuple(tupleSlot);

	/* Initialize all values for this row to null */
	memset(columnValues, 0, columnCount * sizeof(Datum));
	memset(columnNulls, true, columnCount * sizeof(bool));

	if (mongoCursorNext(mongoCursor, NULL))
	{
		const BSON *bsonDocument = mongoCursorBson(mongoCursor);
		const char *bsonDocumentKey = NULL; /* Top level document */

		fill_tuple_slot(bsonDocument, bsonDocumentKey, columnMappingHash,
						columnValues, columnNulls, fmstate->relType);

		ExecStoreVirtualTuple(tupleSlot);
	}

	return tupleSlot;
}

/*
 * mongoEndForeignScan
 *		Finishes scanning the foreign table, closes the cursor and the
 *		connection to MongoDB, and reclaims scan related resources.
 */
static void
mongoEndForeignScan(ForeignScanState *node)
{
	MongoFdwModifyState *fmstate;

	fmstate = (MongoFdwModifyState *) node->fdw_state;
	/* If we executed a query, reclaim mongo related resources */
	if (fmstate != NULL)
	{
		if (fmstate->options)
		{
			mongo_free_options(fmstate->options);
			fmstate->options = NULL;
		}
		mongo_free_scan_state(fmstate);
	}
}

/*
 * mongoReScanForeignScan
 *		Rescans the foreign table.
 *
 * Note that rescans in Mongo end up being notably more expensive than what the
 * planner expects them to be, since MongoDB cursors don't provide reset/rewind
 * functionality.
 */
static void
mongoReScanForeignScan(ForeignScanState *node)
{
	MongoFdwModifyState *fmstate = (MongoFdwModifyState *) node->fdw_state;

	/* Close down the old cursor */
	if (fmstate->mongoCursor)
	{
		mongoCursorDestroy(fmstate->mongoCursor);
		fmstate->mongoCursor = NULL;
	}
}

static List *
mongoPlanForeignModify(PlannerInfo *root,
					   ModifyTable *plan,
					   Index resultRelation,
					   int subplan_index)
{
	CmdType		operation = plan->operation;
	RangeTblEntry *rte = planner_rt_fetch(resultRelation, root);
	Relation	rel;
	List	   *targetAttrs = NIL;

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
#if PG_VERSION_NUM < 130000
	rel = heap_open(rte->relid, NoLock);
#else
	rel = table_open(rte->relid, NoLock);
#endif
	if (operation == CMD_INSERT)
	{
		TupleDesc	tupdesc = RelationGetDescr(rel);
		int			attnum;

		for (attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}
	else if (operation == CMD_UPDATE)
	{
		Bitmapset  *tmpset;
#if PG_VERSION_NUM >= 160000
		RTEPermissionInfo *perminfo;
		int			attidx;
#endif
		AttrNumber	col;

#if PG_VERSION_NUM >= 160000
		perminfo = getRTEPermissionInfo(root->parse->rteperminfos, rte);
		tmpset = bms_copy(perminfo->updatedCols);
		attidx = -1;
#else
		tmpset = bms_copy(rte->updatedCols);
#endif

#if PG_VERSION_NUM >= 160000
		while ((attidx = bms_next_member(tmpset, attidx)) >= 0)
#else
		while ((col = bms_first_member(tmpset)) >= 0)
#endif
		{
#if PG_VERSION_NUM >= 160000
			col = attidx + FirstLowInvalidHeapAttributeNumber;
#else
			col += FirstLowInvalidHeapAttributeNumber;
#endif
			if (col <= InvalidAttrNumber)	/* Shouldn't happen */
				elog(ERROR, "system-column update is not supported");

			/*
			 * We also disallow updates to the first column which happens to
			 * be the row identifier in MongoDb (_id)
			 */
			if (col == 1)		/* Shouldn't happen */
				elog(ERROR, "row identifier column update is not supported");

			targetAttrs = lappend_int(targetAttrs, col);
		}
		/* We also want the rowid column to be available for the update */
		targetAttrs = lcons_int(1, targetAttrs);
	}
	else
		targetAttrs = lcons_int(1, targetAttrs);

	/*
	 * RETURNING list not supported
	 */
	if (plan->returningLists)
		elog(ERROR, "RETURNING is not supported by this FDW");

#if PG_VERSION_NUM < 130000
	heap_close(rel, NoLock);
#else
	table_close(rel, NoLock);
#endif

	return list_make1(targetAttrs);
}

/*
 * mongoBeginForeignModify
 *		Begin an insert/update/delete operation on a foreign table.
 */
static void
mongoBeginForeignModify(ModifyTableState *mtstate,
						ResultRelInfo *resultRelInfo,
						List *fdw_private,
						int subplan_index,
						int eflags)
{
	MongoFdwModifyState *fmstate;
	Relation	rel = resultRelInfo->ri_RelationDesc;
	AttrNumber	n_params;
	Oid			typefnoid = InvalidOid;
	bool		isvarlena = false;
	ListCell   *lc;
	Oid			foreignTableId;
	Oid			userid;
	ForeignServer *server;
	UserMapping *user;
	ForeignTable *table;
#if PG_VERSION_NUM >= 160000
	ForeignScan *fsplan = (ForeignScan *) mtstate->ps.plan;
#else
	EState	   *estate = mtstate->ps.state;
	RangeTblEntry *rte;
#endif

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  resultRelInfo->ri_FdwState
	 * stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

#if PG_VERSION_NUM >= 160000
	userid = fsplan->checkAsUser ? fsplan->checkAsUser : GetUserId();
#else
	rte = rt_fetch(resultRelInfo->ri_RangeTableIndex, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();
#endif

	foreignTableId = RelationGetRelid(rel);

	/* Get info about foreign table. */
	table = GetForeignTable(foreignTableId);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/* Begin constructing MongoFdwModifyState. */
	fmstate = (MongoFdwModifyState *) palloc0(sizeof(MongoFdwModifyState));

	fmstate->rel = rel;
	fmstate->options = mongo_get_options(foreignTableId);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	fmstate->mongoConnection = mongo_get_connection(server, user,
													fmstate->options);

	fmstate->target_attrs = (List *) list_nth(fdw_private, 0);

	n_params = list_length(fmstate->target_attrs) + 1;
	fmstate->p_flinfo = (FmgrInfo *) palloc(sizeof(FmgrInfo) * n_params);
	fmstate->p_nums = 0;

	if (mtstate->operation == CMD_UPDATE)
	{
		Form_pg_attribute attr;
#if PG_VERSION_NUM >= 140000
		Plan	   *subplan = outerPlanState(mtstate)->plan;
#else
		Plan	   *subplan = mtstate->mt_plans[subplan_index]->plan;
#endif

		Assert(subplan != NULL);

		attr = TupleDescAttr(RelationGetDescr(rel), 0);

		/* Find the rowid resjunk column in the subplan's result */
		fmstate->rowidAttno = ExecFindJunkAttributeInTlist(subplan->targetlist,
														   NameStr(attr->attname));
		if (!AttributeNumberIsValid(fmstate->rowidAttno))
			elog(ERROR, "could not find junk row identifier column");
	}

	/* Set up for remaining transmittable parameters */
	foreach(lc, fmstate->target_attrs)
	{
		int			attnum = lfirst_int(lc);
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(rel),
											   attnum - 1);

		Assert(!attr->attisdropped);

		getTypeOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
		fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
		fmstate->p_nums++;
	}
	Assert(fmstate->p_nums <= n_params);

	resultRelInfo->ri_FdwState = fmstate;
}

/*
 * mongoExecForeignInsert
 *		Insert one row into a foreign table.
 */
static TupleTableSlot *
mongoExecForeignInsert(EState *estate,
					   ResultRelInfo *resultRelInfo,
					   TupleTableSlot *slot,
					   TupleTableSlot *planSlot)
{
	BSON	   *bsonDoc;
	Oid			typoid;
	Datum		value;
	bool		isnull = false;
	MongoFdwModifyState *fmstate;

	fmstate = (MongoFdwModifyState *) resultRelInfo->ri_FdwState;

	bsonDoc = bsonCreate();

	typoid = get_atttype(RelationGetRelid(resultRelInfo->ri_RelationDesc), 1);

	/* Get following parameters from slot */
	if (slot != NULL && fmstate->target_attrs != NIL)
	{
		ListCell   *lc;

		foreach(lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);

			value = slot_getattr(slot, attnum, &isnull);

			/* First column of MongoDB's foreign table must be _id */
			if (strcmp(TupleDescAttr(slot->tts_tupleDescriptor, 0)->attname.data, "_id") != 0)
				elog(ERROR, "first column of MongoDB's foreign table must be \"_id\"");

			if (typoid != NAMEOID)
				elog(ERROR, "type of first column of MongoDB's foreign table must be \"NAME\"");
			if (strcmp(TupleDescAttr(slot->tts_tupleDescriptor, 0)->attname.data, "__doc") == 0)
				continue;

			/*
			 * Ignore the value of first column which is row identifier in
			 * MongoDb (_id) and let MongoDB to insert the unique value for
			 * that column.
			 */
			if (attnum == 1)
			{
				ereport(DEBUG1,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot insert given data for \"_id\" column, skipping"),
						 errhint("Let MongoDB insert the unique value for \"_id\" column.")));

				continue;
			}

			append_mongo_value(bsonDoc,
							   TupleDescAttr(slot->tts_tupleDescriptor, attnum - 1)->attname.data,
							   value,
							   isnull,
							   TupleDescAttr(slot->tts_tupleDescriptor, attnum - 1)->atttypid);
		}
	}

	/* Now we are ready to insert tuple/document into MongoDB */
	mongoInsert(fmstate->mongoConnection, fmstate->options->svr_database,
				fmstate->options->collectionName, bsonDoc);

	bsonDestroy(bsonDoc);

	return slot;
}

/*
 * mongoAddForeignUpdateTargets
 *		Add column(s) needed for update/delete on a foreign table, we are using
 *		first column as row identification column, so we are adding that into
 *		target list.
 */
#if PG_VERSION_NUM >= 140000
static void
mongoAddForeignUpdateTargets(PlannerInfo *root,
							 Index rtindex,
							 RangeTblEntry *target_rte,
							 Relation target_relation)
#else
static void
mongoAddForeignUpdateTargets(Query *parsetree,
							 RangeTblEntry *target_rte,
							 Relation target_relation)
#endif
{
	Var		   *var;
	const char *attrname;
#if PG_VERSION_NUM < 140000
	TargetEntry *tle;
#endif

	/*
	 * What we need is the rowid which is the first column
	 */
	Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(target_relation),
										   0);

	/* Make a Var representing the desired value */
#if PG_VERSION_NUM >= 140000
	var = makeVar(rtindex,
#else
	var = makeVar(parsetree->resultRelation,
#endif
				  1,
				  attr->atttypid,
				  attr->atttypmod,
				  InvalidOid,
				  0);

	/* Get name of the row identifier column */
	attrname = NameStr(attr->attname);

#if PG_VERSION_NUM >= 140000
	/* Register it as a row-identity column needed by this target rel */
	add_row_identity_var(root, var, rtindex, attrname);
#else
	/* Wrap it in a TLE with the right name ... */
	tle = makeTargetEntry((Expr *) var,
						  list_length(parsetree->targetList) + 1,
						  pstrdup(attrname),
						  true);

	/* ... And add it to the query's targetlist */
	parsetree->targetList = lappend(parsetree->targetList, tle);
#endif
}

static TupleTableSlot *
mongoExecForeignUpdate(EState *estate,
					   ResultRelInfo *resultRelInfo,
					   TupleTableSlot *slot,
					   TupleTableSlot *planSlot)
{
	Datum		datum;
	bool		isNull = false;
	Oid			foreignTableId;
	char	   *columnName;
	Oid			typoid;
	BSON	   *document;
	BSON	   *op = NULL;
	BSON		set;
	MongoFdwModifyState *fmstate;

	fmstate = (MongoFdwModifyState *) resultRelInfo->ri_FdwState;
	foreignTableId = RelationGetRelid(resultRelInfo->ri_RelationDesc);

	/* Get the id that was passed up as a resjunk column */
	datum = ExecGetJunkAttribute(planSlot, fmstate->rowidAttno, &isNull);

	columnName = get_attname(foreignTableId, 1, false);

	/* First column of MongoDB's foreign table must be _id */
	if (strcmp(columnName, "_id") != 0)
		elog(ERROR, "first column of MongoDB's foreign table must be \"_id\"");

	typoid = get_atttype(foreignTableId, 1);

	/* The type of first column of MongoDB's foreign table must be NAME */
	if (typoid != NAMEOID)
		elog(ERROR, "type of first column of MongoDB's foreign table must be \"NAME\"");

	document = bsonCreate();
	bsonAppendStartObject(document, "$set", &set);

	/* Get following parameters from slot */
	if (slot != NULL && fmstate->target_attrs != NIL)
	{
		ListCell   *lc;

		foreach(lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
			Form_pg_attribute attr = TupleDescAttr(slot->tts_tupleDescriptor,
												   attnum - 1);
			Datum		value;
			bool		isnull;

			if (strcmp("_id", attr->attname.data) == 0)
				continue;

			if (strcmp("__doc", attr->attname.data) == 0)
				elog(ERROR, "system column '__doc' update is not supported");

			value = slot_getattr(slot, attnum, &isnull);
			append_mongo_value(&set, attr->attname.data, value,
							   isnull ? true : false, attr->atttypid);
		}
	}
	bsonAppendFinishObject(document, &set);

	op = bsonCreate();
	if (!append_mongo_value(op, columnName, datum, isNull, typoid))
	{
		bsonDestroy(document);
		return NULL;
	}

	/* We are ready to update the row into MongoDB */
	mongoUpdate(fmstate->mongoConnection, fmstate->options->svr_database,
				fmstate->options->collectionName, op, document);

	bsonDestroy(op);
	bsonDestroy(document);

	/* Return NULL if nothing was updated on the remote end */
	return slot;
}

/*
 * mongoExecForeignDelete
 *		Delete one row from a foreign table
 */
static TupleTableSlot *
mongoExecForeignDelete(EState *estate,
					   ResultRelInfo *resultRelInfo,
					   TupleTableSlot *slot,
					   TupleTableSlot *planSlot)
{
	Datum		datum;
	bool		isNull = false;
	Oid			foreignTableId;
	char	   *columnName = NULL;
	Oid			typoid;
	BSON	   *document;
	MongoFdwModifyState *fmstate;

	fmstate = (MongoFdwModifyState *) resultRelInfo->ri_FdwState;

	foreignTableId = RelationGetRelid(resultRelInfo->ri_RelationDesc);

	/* Get the id that was passed up as a resjunk column */
	datum = ExecGetJunkAttribute(planSlot, 1, &isNull);

	columnName = get_attname(foreignTableId, 1, false);

	/* First column of MongoDB's foreign table must be _id */
	if (strcmp(columnName, "_id") != 0)
		elog(ERROR, "first column of MongoDB's foreign table must be \"_id\"");

	typoid = get_atttype(foreignTableId, 1);

	/* The type of first column of MongoDB's foreign table must be NAME */
	if (typoid != NAMEOID)
		elog(ERROR, "type of first column of MongoDB's foreign table must be \"NAME\"");

	document = bsonCreate();
	if (!append_mongo_value(document, columnName, datum, isNull, typoid))
	{
		bsonDestroy(document);
		return NULL;
	}

	/* Now we are ready to delete a single document from MongoDB */
	mongoDelete(fmstate->mongoConnection, fmstate->options->svr_database,
				fmstate->options->collectionName, document);

	bsonDestroy(document);

	/* Return NULL if nothing was updated on the remote end */
	return slot;
}

/*
 * mongoEndForeignModify
 *		Finish an insert/update/delete operation on a foreign table
 */
static void
mongoEndForeignModify(EState *estate, ResultRelInfo *resultRelInfo)
{
	MongoFdwModifyState *fmstate;

	fmstate = (MongoFdwModifyState *) resultRelInfo->ri_FdwState;
	if (fmstate)
	{
		if (fmstate->options)
		{
			mongo_free_options(fmstate->options);
			fmstate->options = NULL;
		}
		mongo_free_scan_state(fmstate);
		pfree(fmstate);
	}
}

/*
 * foreign_table_document_count
 * 		Connects to the MongoDB server, and queries it for the number of
 * 		documents in the foreign collection. On success, the function returns
 * 		the document count.  On failure, the function returns -1.0.
 */
static double
foreign_table_document_count(Oid foreignTableId)
{
	MongoFdwOptions *options;
	MONGO_CONN *mongoConnection;
	const BSON *emptyQuery = NULL;
	double		documentCount;
	Oid			userid = GetUserId();
	ForeignServer *server;
	UserMapping *user;
	ForeignTable *table;

	/* Get info about foreign table. */
	table = GetForeignTable(foreignTableId);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/* Resolve foreign table options; and connect to mongo server */
	options = mongo_get_options(foreignTableId);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	mongoConnection = mongo_get_connection(server, user, options);

	documentCount = mongoAggregateCount(mongoConnection, options->svr_database,
										options->collectionName, emptyQuery);

	mongo_free_options(options);

	return documentCount;
}

/*
 * column_mapping_hash
 *		Creates a hash table that maps column names to column index and types.
 *
 * This table helps us quickly translate BSON document key/values to the
 * corresponding PostgreSQL columns.
 */
static HTAB *
column_mapping_hash(Oid foreignTableId, List *columnList, List *colNameList,
					List *colIsInnerList, uint32 relType)
{
	ListCell   *columnCell;
	HTAB	   *columnMappingHash;
	HASHCTL		hashInfo;
	uint32		attnum = 0;
	Index		listIndex = 0;
	Index		aggIndex = 0;

	memset(&hashInfo, 0, sizeof(hashInfo));
	hashInfo.keysize = NAMEDATALEN;
	hashInfo.entrysize = sizeof(ColumnMapping);
	hashInfo.hash = string_hash;
	hashInfo.hcxt = CurrentMemoryContext;

	columnMappingHash = hash_create("Column Mapping Hash", MaxHashTableSize,
									&hashInfo,
									(HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT));
	Assert(columnMappingHash != NULL);

	foreach(columnCell, columnList)
	{
		Var		   *column = (Var *) lfirst(columnCell);
		ColumnMapping *columnMapping;
		char	   *columnName;
		bool		handleFound = false;
		void	   *hashKey;

		if (relType == JOIN_REL)
		{
			int			is_innerrel = list_nth_int(colIsInnerList, listIndex);

			columnName = strVal(list_nth(colNameList, listIndex++));

			/*
			 * In MongoDB, columns involved in join result-set from inner
			 * table prefixed with Join result name.  Uses hard-coded string
			 * "Join Result" to be prefixed to form the hash key to read the
			 * joined result set.  This same prefix needs to be given as
			 * joined result set name in the $lookup stage when building the
			 * remote query.
			 *
			 * For a simple relation scan, the column name would be the hash
			 * key.
			 */
			if (is_innerrel)
			{
				const char *resultName = "Join_Result";
				StringInfo	KeyString = makeStringInfo();

				appendStringInfo(KeyString, "%s.%s", resultName, columnName);

				hashKey = (void *) KeyString->data;
			}
			else
				hashKey = (void *) columnName;
		}

		/*
		 * In MongoDB, columns involved in upper result-set named as
		 * "_id.column_name_variable" not the actual column names.  Use this
		 * as hashKey to match the bson key we get at the time of fetching the
		 * column values.
		 *
		 * Use the hard-coded string v_agg* to get the aggregation result.
		 * This same name needs to be given as an aggregation result name
		 * while building the remote query.
		 */
		else if (relType == UPPER_REL || relType == UPPER_JOIN_REL)
		{
			if (IsA(column, Var))
			{
				if (relType == UPPER_REL)
					columnName = get_attname(foreignTableId, column->varattno,
											 false);
				else
					columnName = strVal(list_nth(colNameList, listIndex++));

				/*
				 * Keep variable name same as a column name.  Use the same
				 * name while building the MongoDB query in the
				 * mongo_query_document function.
				 */
				hashKey = psprintf("_id.%s", columnName);
			}
			else
				hashKey = psprintf("AGG_RESULT_KEY%d", aggIndex++);
		}
		else
		{
			columnName = get_attname(foreignTableId, column->varattno, false);
			hashKey = (void *) columnName;
		}

		columnMapping = (ColumnMapping *) hash_search(columnMappingHash,
													  hashKey,
													  HASH_ENTER,
													  &handleFound);
		Assert(columnMapping != NULL);

		/*
		 * Save attribute number of the current column in the resulting tuple.
		 * For join/upper relation, it is continuously increasing integers
		 * starting from 0, and for simple relation, it's varattno.
		 */
		if (relType != BASE_REL)
		{
			columnMapping->columnIndex = attnum;
			attnum++;
		}
		else
			columnMapping->columnIndex = column->varattno - 1;

		/* Save other information */
		if ((relType == UPPER_REL || relType == UPPER_JOIN_REL) &&
			!strncmp(hashKey, "AGG_RESULT_KEY", 5))
		{
			Aggref	   *agg = (Aggref *) lfirst(columnCell);

			columnMapping->columnTypeId = agg->aggtype;
			columnMapping->columnTypeMod = agg->aggcollid;
			columnMapping->columnArrayTypeId = InvalidOid;
		}
		else
		{
			columnMapping->columnTypeId = column->vartype;
			columnMapping->columnTypeMod = column->vartypmod;
			columnMapping->columnArrayTypeId = get_element_type(column->vartype);
		}
	}

	return columnMappingHash;
}

/*
 * fill_tuple_slot
 *		Walks over all key/value pairs in the given document.
 *
 * For each pair, the function checks if the key appears in the column mapping
 * hash, and if the value type is compatible with the one specified for the
 * column.  If so, the function converts the value and fills the corresponding
 * tuple position.  The bsonDocumentKey parameter is used for recursion, and
 * should always be passed as NULL.
 */
static void
fill_tuple_slot(const BSON *bsonDocument, const char *bsonDocumentKey,
				HTAB *columnMappingHash, Datum *columnValues,
				bool *columnNulls, uint32 relType)
{
	ColumnMapping *columnMapping;
	bool		handleFound = false;
	void	   *hashKey;
	BSON_ITERATOR bsonIterator = {NULL, 0};

	if (bsonIterInit(&bsonIterator, (BSON *) bsonDocument) == false)
		elog(ERROR, "failed to initialize BSON iterator");

	hashKey = "__doc";
	columnMapping = (ColumnMapping *) hash_search(columnMappingHash, hashKey,
												  HASH_FIND, &handleFound);

	if (columnMapping != NULL && handleFound == true &&
		columnValues[columnMapping->columnIndex] == 0)
	{
		JsonLexContext *lex;
		text	   *result;
		Datum		columnValue;
		char	   *str;

		str = bsonAsJson(bsonDocument);
		result = cstring_to_text_with_len(str, strlen(str));
		lex = makeJsonLexContext(result, false);
		pg_parse_json(lex, &nullSemAction);
		columnValue = PointerGetDatum(result);

		switch (columnMapping->columnTypeId)
		{
			case BOOLOID:
			case INT2OID:
			case INT4OID:
			case INT8OID:
			case BOXOID:
			case BYTEAOID:
			case CHAROID:
			case VARCHAROID:
			case NAMEOID:
			case JSONOID:
			case XMLOID:
			case POINTOID:
			case LSEGOID:
			case LINEOID:
			case UUIDOID:
			case LSNOID:
			case TEXTOID:
			case CASHOID:
			case DATEOID:
			case MACADDROID:
			case TIMESTAMPOID:
			case TIMESTAMPTZOID:
			case BPCHAROID:
				columnValue = PointerGetDatum(result);
				break;
			case JSONBOID:
				columnValue = DirectFunctionCall1(jsonb_in,
												  PointerGetDatum(str));
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
						 errmsg("unsupported type for column __doc"),
						 errhint("Column type: %u",
								 (uint32) columnMapping->columnTypeId)));
				break;
		}

		columnValues[columnMapping->columnIndex] = columnValue;
		columnNulls[columnMapping->columnIndex] = false;

		return;
	}

	while (bsonIterNext(&bsonIterator))
	{
		const char *bsonKey = bsonIterKey(&bsonIterator);
		BSON_TYPE	bsonType = bsonIterType(&bsonIterator);
		Oid			columnTypeId = InvalidOid;
		Oid			columnArrayTypeId = InvalidOid;
		bool		compatibleTypes = false;
		const char *bsonFullKey;
		int32		attnum;
		bool		is_agg = false;

		if (!strncmp(bsonKey, "AGG_RESULT_KEY", 5) && bsonType == BSON_TYPE_INT32)
			is_agg = true;

		columnMapping = NULL;
		if (bsonDocumentKey != NULL)
		{
			/*
			 * For fields in nested BSON objects, we use fully qualified field
			 * name to check the column mapping.
			 */
			StringInfo	bsonFullKeyString = makeStringInfo();

			appendStringInfo(bsonFullKeyString, "%s.%s", bsonDocumentKey,
							 bsonKey);
			bsonFullKey = bsonFullKeyString->data;
		}
		else
			bsonFullKey = bsonKey;

		/* Look up the corresponding column for this bson key */
		hashKey = (void *) bsonFullKey;
		columnMapping = (ColumnMapping *) hash_search(columnMappingHash,
													  hashKey,
													  HASH_FIND,
													  &handleFound);
		if (columnMapping != NULL)
		{
			columnTypeId = columnMapping->columnTypeId;
			columnArrayTypeId = columnMapping->columnArrayTypeId;
		}

		/* Recurse into nested objects */
		if (bsonType == BSON_TYPE_DOCUMENT)
		{
			if (columnTypeId != JSONOID)
			{
				BSON		subObject;

				bsonIterSubObject(&bsonIterator, &subObject);
				fill_tuple_slot(&subObject, bsonFullKey, columnMappingHash,
								columnValues, columnNulls, relType);
				continue;
			}
		}

		/* If no corresponding column or null BSON value, continue */
		if (!is_agg && (columnMapping == NULL || bsonType == BSON_TYPE_NULL))
			continue;

		/* Check if columns have compatible types */
		if ((OidIsValid(columnArrayTypeId) && bsonType == BSON_TYPE_ARRAY))
			compatibleTypes = true;
		else
			compatibleTypes = column_types_compatible(bsonType, columnTypeId);

		/* If types are incompatible, leave this column null */
		if (!compatibleTypes)
			continue;

		if (columnMapping != NULL)
			attnum = columnMapping->columnIndex;

		/* Fill in corresponding column value and null flag */
		if (OidIsValid(columnArrayTypeId))
			columnValues[attnum] = column_value_array(&bsonIterator,
													  columnArrayTypeId);
		else
			columnValues[attnum] = column_value(&bsonIterator, columnTypeId,
												columnMapping->columnTypeMod);
		columnNulls[attnum] = false;
	}
}

/*
 * column_types_compatible
 * 		Checks if the given BSON type can be converted to the given PostgreSQL
 * 		type.
 *
 * In this check, the function also uses its knowledge of internal conversions
 * applied by BSON APIs.
 */
static bool
column_types_compatible(BSON_TYPE bsonType, Oid columnTypeId)
{
	bool		compatibleTypes = false;

	/* We consider the PostgreSQL column type as authoritative */
	switch (columnTypeId)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case FLOAT4OID:
		case FLOAT8OID:
		case NUMERICOID:
			if (bsonType == BSON_TYPE_INT32 || bsonType == BSON_TYPE_INT64 ||
				bsonType == BSON_TYPE_DOUBLE)
				compatibleTypes = true;
			if (bsonType == BSON_TYPE_BOOL)
				compatibleTypes = true;
			break;
		case BOOLOID:
			if (bsonType == BSON_TYPE_INT32 || bsonType == BSON_TYPE_INT64 ||
				bsonType == BSON_TYPE_DOUBLE || bsonType == BSON_TYPE_BOOL)
				compatibleTypes = true;
			break;
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
			if (bsonType == BSON_TYPE_UTF8)
				compatibleTypes = true;
			break;
		case BYTEAOID:
			if (bsonType == BSON_TYPE_BINDATA)
				compatibleTypes = true;
			if (bsonType == BSON_TYPE_OID)
				compatibleTypes = true;
			break;
		case NAMEOID:

			/*
			 * We currently error out on data types other than object
			 * identifier.  MongoDB supports more data types for the _id field
			 * but those are not yet handled in mongo_fdw.
			 */
			if (bsonType != BSON_TYPE_OID)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
						 errmsg("cannot convert BSON type to column type"),
						 errhint("Column type \"NAME\" is compatible only with BSON type \"ObjectId\".")));

			/*
			 * We currently overload the NAMEOID type to represent the BSON
			 * object identifier.  We can safely overload this 64-byte data
			 * type since it's reserved for internal use in PostgreSQL.
			 */
			compatibleTypes = true;
			break;
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			if (bsonType == BSON_TYPE_DATE_TIME)
				compatibleTypes = true;
			break;
		case NUMERICARRAY_OID:
			if (bsonType == BSON_TYPE_ARRAY)
				compatibleTypes = true;
			break;
		case JSONOID:
			if (bsonType == BSON_TYPE_DOCUMENT ||
				bsonType == BSON_TYPE_ARRAY)
				compatibleTypes = true;
			break;
		default:

			/*
			 * We currently error out on other data types.  Some types such as
			 * byte arrays are easy to add, but they need testing.
			 *
			 * Other types such as money or inet, do not have equivalents in
			 * MongoDB.
			 */
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("cannot convert BSON type to column type"),
					 errhint("Column type: %u", (uint32) columnTypeId)));
			break;
	}

	return compatibleTypes;
}

/*
 * column_value_array
 * 		Uses array element type id to read the current array pointed to by the
 * 		BSON iterator, and converts each array element (with matching type) to
 * 		the corresponding PostgreSQL datum.
 *
 * Then, the function constructs an array datum from element datums, and
 * returns the array datum.
 */
static Datum
column_value_array(BSON_ITERATOR *bsonIterator, Oid valueTypeId)
{
	Datum	   *columnValueArray = palloc(INITIAL_ARRAY_CAPACITY * sizeof(Datum));
	uint32		arrayCapacity = INITIAL_ARRAY_CAPACITY;
	uint32		arrayIndex = 0;
	ArrayType  *columnValueObject;
	Datum		columnValueDatum;
	bool		typeByValue;
	char		typeAlignment;
	int16		typeLength;

	BSON_ITERATOR bsonSubIterator = {NULL, 0};

	bsonIterSubIter(bsonIterator, &bsonSubIterator);
	while (bsonIterNext(&bsonSubIterator))
	{
		BSON_TYPE	bsonType = bsonIterType(&bsonSubIterator);
		bool		compatibleTypes = false;

		compatibleTypes = column_types_compatible(bsonType, valueTypeId);
		if (bsonType == BSON_TYPE_NULL || !compatibleTypes)
			continue;

		if (arrayIndex >= arrayCapacity)
		{
			/* Double the array capacity. */
			arrayCapacity *= 2;
			columnValueArray = repalloc(columnValueArray,
										arrayCapacity * sizeof(Datum));
		}

		/* Use default type modifier (0) to convert column value */
		columnValueArray[arrayIndex] = column_value(&bsonSubIterator,
													valueTypeId, 0);
		arrayIndex++;
	}

	get_typlenbyvalalign(valueTypeId, &typeLength, &typeByValue,
						 &typeAlignment);
	columnValueObject = construct_array(columnValueArray,
										arrayIndex,
										valueTypeId,
										typeLength,
										typeByValue,
										typeAlignment);

	columnValueDatum = PointerGetDatum(columnValueObject);

	pfree(columnValueArray);

	return columnValueDatum;
}

/*
 * column_value
 * 		Uses column type information to read the current value pointed to by
 * 		the BSON iterator, and converts this value to the corresponding
 * 		PostgreSQL datum.  The function then returns this datum.
 */
static Datum
column_value(BSON_ITERATOR *bsonIterator, Oid columnTypeId,
			 int32 columnTypeMod)
{
	Datum		columnValue;

	switch (columnTypeId)
	{
		case INT2OID:
			{
				int16		value = (int16) bsonIterInt32(bsonIterator);

				columnValue = Int16GetDatum(value);
			}
			break;
		case INT4OID:
			{
				int32		value = bsonIterInt32(bsonIterator);

				columnValue = Int32GetDatum(value);
			}
			break;
		case INT8OID:
			{
				int64		value = bsonIterInt64(bsonIterator);

				columnValue = Int64GetDatum(value);
			}
			break;
		case FLOAT4OID:
			{
				float4		value = (float4) bsonIterDouble(bsonIterator);

				columnValue = Float4GetDatum(value);
			}
			break;
		case FLOAT8OID:
			{
				float8		value = bsonIterDouble(bsonIterator);

				columnValue = Float8GetDatum(value);
			}
			break;
		case NUMERICOID:
			{
				float8		value = bsonIterDouble(bsonIterator);
				Datum		valueDatum = DirectFunctionCall1(float8_numeric,
															 Float8GetDatum(value));

				/*
				 * Since we have a Numeric value, using numeric() here instead
				 * of numeric_in() input function for typmod conversion.
				 */
				columnValue = DirectFunctionCall2(numeric, valueDatum,
												  Int32GetDatum(columnTypeMod));
			}
			break;
		case BOOLOID:
			{
				bool		value = bsonIterBool(bsonIterator);

				columnValue = BoolGetDatum(value);
			}
			break;
		case BPCHAROID:
			{
				const char *value = bsonIterString(bsonIterator);
				Datum		valueDatum = CStringGetDatum(value);

				columnValue = DirectFunctionCall3(bpcharin, valueDatum,
												  ObjectIdGetDatum(InvalidOid),
												  Int32GetDatum(columnTypeMod));
			}
			break;
		case VARCHAROID:
			{
				const char *value = bsonIterString(bsonIterator);
				Datum		valueDatum = CStringGetDatum(value);

				columnValue = DirectFunctionCall3(varcharin, valueDatum,
												  ObjectIdGetDatum(InvalidOid),
												  Int32GetDatum(columnTypeMod));
			}
			break;
		case TEXTOID:
			{
				const char *value = bsonIterString(bsonIterator);

				columnValue = CStringGetTextDatum(value);
			}
			break;
		case NAMEOID:
			{
				char		value[NAMEDATALEN];
				Datum		valueDatum = 0;

				bson_oid_t *bsonObjectId = (bson_oid_t *) bsonIterOid(bsonIterator);

				bson_oid_to_string(bsonObjectId, value);

				valueDatum = CStringGetDatum(value);
				columnValue = DirectFunctionCall3(namein, valueDatum,
												  ObjectIdGetDatum(InvalidOid),
												  Int32GetDatum(columnTypeMod));
			}
			break;
		case BYTEAOID:
			{
				int			value_len;
				char	   *value;
				bytea	   *result;

				switch (bsonIterType(bsonIterator))
				{
					case BSON_TYPE_OID:
						value = (char *) bsonIterOid(bsonIterator);
						value_len = 12;
						break;
					default:
						value = (char *) bsonIterBinData(bsonIterator,
														 (uint32_t *) &value_len);
						break;
				}
				result = (bytea *) palloc(value_len + VARHDRSZ);
				memcpy(VARDATA(result), value, value_len);
				SET_VARSIZE(result, value_len + VARHDRSZ);
				columnValue = PointerGetDatum(result);
			}
			break;
		case DATEOID:
			{
				int64		valueMillis = bsonIterDate(bsonIterator);
				int64		timestamp = (valueMillis * 1000L) - POSTGRES_TO_UNIX_EPOCH_USECS;
				Datum		timestampDatum = TimestampGetDatum(timestamp);

				columnValue = DirectFunctionCall1(timestamp_date,
												  timestampDatum);
			}
			break;
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			{
				int64		valueMillis = bsonIterDate(bsonIterator);
				int64		timestamp = (valueMillis * 1000L) - POSTGRES_TO_UNIX_EPOCH_USECS;

				/* Overlook type modifiers for timestamp */
				columnValue = TimestampGetDatum(timestamp);
			}
			break;
		case JSONOID:
			{
				JsonLexContext *lex;
				text	   *result;
				StringInfo	buffer = makeStringInfo();

				BSON_TYPE	type = BSON_ITER_TYPE(bsonIterator);

				if (type != BSON_TYPE_ARRAY && type != BSON_TYPE_DOCUMENT)
					ereport(ERROR,
							(errmsg("cannot convert to json")));

				/* Convert BSON to JSON value */
				bsonToJsonStringValue(buffer, bsonIterator,
									  BSON_TYPE_ARRAY == type);
				result = cstring_to_text_with_len(buffer->data, buffer->len);
				lex = makeJsonLexContext(result, false);
				pg_parse_json(lex, &nullSemAction);
				columnValue = PointerGetDatum(result);
			}
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("cannot convert BSON type to column type"),
					 errhint("Column type: %u", (uint32) columnTypeId)));
			break;
	}

	return columnValue;
}

/*
 * mongo_free_scan_state
 *		Closes the cursor and connection to MongoDB, and reclaims all Mongo
 *		related resources allocated for the foreign scan.
 */
static void
mongo_free_scan_state(MongoFdwModifyState *fmstate)
{
	if (fmstate == NULL)
		return;

	if (fmstate->queryDocument)
	{
		bsonDestroy(fmstate->queryDocument);
		fmstate->queryDocument = NULL;
	}

	if (fmstate->mongoCursor)
	{
		mongoCursorDestroy(fmstate->mongoCursor);
		fmstate->mongoCursor = NULL;
	}

	/* Release remote connection */
	mongo_release_connection(fmstate->mongoConnection);
}

/*
 * mongoAnalyzeForeignTable
 *		Collects statistics for the given foreign table.
 */
static bool
mongoAnalyzeForeignTable(Relation relation,
						 AcquireSampleRowsFunc *func,
						 BlockNumber *totalpages)
{
	BlockNumber pageCount = 0;
	int			attributeCount;
	int32	   *attributeWidths;
	Oid			foreignTableId;
	int32		documentWidth;
	double		documentCount;
	double		foreignTableSize;

	foreignTableId = RelationGetRelid(relation);
	documentCount = foreign_table_document_count(foreignTableId);

	if (documentCount > 0.0)
	{
		attributeCount = RelationGetNumberOfAttributes(relation);
		attributeWidths = (int32 *) palloc0((attributeCount + 1) * sizeof(int32));

		/*
		 * We estimate disk costs assuming a sequential scan over the data.
		 * This is an inaccurate assumption as Mongo scatters the data over
		 * disk pages, and may rely on an index to retrieve the data.  Still,
		 * this should at least give us a relative cost.
		 */
		documentWidth = get_relation_data_width(foreignTableId,
												attributeWidths);
		foreignTableSize = documentCount * documentWidth;

		pageCount = (BlockNumber) rint(foreignTableSize / BLCKSZ);
	}
	else
		ereport(DEBUG1,
				(errmsg("could not retrieve document count for collection"),
				 errhint("Could not	collect statistics about foreign table.")));

	(*totalpages) = pageCount;
	(*func) = mongo_acquire_sample_rows;

	return true;
}

/*
 * mongo_acquire_sample_rows
 *		Acquires a random sample of rows from the foreign table.
 *
 * Selected rows are returned in the caller allocated sampleRows array,
 * which must have at least target row count entries.  The actual number of
 * rows selected is returned as the function result.  We also count the number
 * of rows in the collection and return it in total row count.  We also always
 * set dead row count to zero.
 *
 * Note that the returned list of rows is not always in order by physical
 * position in the MongoDB collection.  Therefore, correlation estimates
 * derived later may be meaningless, but it's OK because we don't use the
 * estimates currently (the planner only pays attention to correlation for
 * index scans).
 */
static int
mongo_acquire_sample_rows(Relation relation,
						  int errorLevel,
						  HeapTuple *sampleRows,
						  int targetRowCount,
						  double *totalRowCount,
						  double *totalDeadRowCount)
{
	MONGO_CONN *mongoConnection;
	int			sampleRowCount = 0;
	double		rowCount = 0;
	double		rowCountToSkip = -1;	/* -1 means not set yet */
	double		randomState;
	Datum	   *columnValues;
	bool	   *columnNulls;
	Oid			foreignTableId;
	TupleDesc	tupleDescriptor;
	AttrNumber	columnCount;
	AttrNumber	columnId;
	HTAB	   *columnMappingHash;
	MONGO_CURSOR *mongoCursor;
	BSON	   *queryDocument = bsonCreate();
	List	   *columnList = NIL;
	char	   *relationName;
	MemoryContext oldContext = CurrentMemoryContext;
	MemoryContext tupleContext;
	MongoFdwOptions *options;
	ForeignServer *server;
	UserMapping *user;
	ForeignTable *table;

	/* Create list of columns in the relation */
	tupleDescriptor = RelationGetDescr(relation);
	columnCount = tupleDescriptor->natts;

	for (columnId = 1; columnId <= columnCount; columnId++)
	{
		Var		   *column = (Var *) palloc0(sizeof(Var));
		Form_pg_attribute attr = TupleDescAttr(tupleDescriptor, columnId - 1);

		column->varattno = columnId;
		column->vartype = attr->atttypid;
		column->vartypmod = attr->atttypmod;

		columnList = lappend(columnList, column);
	}

	foreignTableId = RelationGetRelid(relation);
	table = GetForeignTable(foreignTableId);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(GetUserId(), server->serverid);
	options = mongo_get_options(foreignTableId);

	/*
	 * Get connection to the foreign server.  Connection manager will
	 * establish new connection if necessary.
	 */
	mongoConnection = mongo_get_connection(server, user, options);

	/* Create cursor for collection name and set query */
	mongoCursor = mongoCursorCreate(mongoConnection, options->svr_database,
									options->collectionName, queryDocument);
	columnMappingHash = column_mapping_hash(foreignTableId, columnList, NIL,
											NIL, BASE_REL);

	/*
	 * Use per-tuple memory context to prevent leak of memory used to read
	 * rows from the file with copy routines.
	 */
	tupleContext = AllocSetContextCreate(CurrentMemoryContext,
										 "mongo_fdw temporary context",
										 ALLOCSET_DEFAULT_SIZES);

	/* Prepare for sampling rows */
	randomState = anl_init_selection_state(targetRowCount);

	columnValues = (Datum *) palloc(columnCount * sizeof(Datum));
	columnNulls = (bool *) palloc(columnCount * sizeof(bool));

	for (;;)
	{
		/* Check for user-requested abort or sleep */
		vacuum_delay_point();

		/* Initialize all values for this row to null */
		memset(columnValues, 0, columnCount * sizeof(Datum));
		memset(columnNulls, true, columnCount * sizeof(bool));

		if (mongoCursorNext(mongoCursor, NULL))
		{
			const BSON *bsonDocument = mongoCursorBson(mongoCursor);
			const char *bsonDocumentKey = NULL; /* Top level document */

			/* Fetch next tuple */
			MemoryContextReset(tupleContext);
			MemoryContextSwitchTo(tupleContext);

			fill_tuple_slot(bsonDocument, bsonDocumentKey, columnMappingHash,
							columnValues, columnNulls, BASE_REL);

			MemoryContextSwitchTo(oldContext);
		}
		else
		{
			bson_error_t error;

			if (mongoc_cursor_error(mongoCursor, &error))
				ereport(ERROR,
						(errmsg("could not iterate over mongo collection"),
						 errhint("Mongo driver error: %s", error.message)));
			break;
		}

		/*
		 * The first targetRowCount sample rows are simply copied into the
		 * reservoir.  Then we start replacing tuples in the sample until we
		 * reach the end of the relation.  This algorithm is from Jeff
		 * Vitter's paper (see more info in commands/analyze.c).
		 */
		if (sampleRowCount < targetRowCount)
			sampleRows[sampleRowCount++] = heap_form_tuple(tupleDescriptor,
														   columnValues,
														   columnNulls);
		else
		{
			/*
			 * t in Vitter's paper is the number of records already processed.
			 * If we need to compute a new S value, we must use the "not yet
			 * incremented" value of rowCount as t.
			 */
			if (rowCountToSkip < 0)
				rowCountToSkip = anl_get_next_S(rowCount, targetRowCount,
												&randomState);

			if (rowCountToSkip <= 0)
			{
				/*
				 * Found a suitable tuple, so save it, replacing one old tuple
				 * at random.
				 */
				int			rowIndex = (int) (targetRowCount * anl_random_fract());

				Assert(rowIndex >= 0);
				Assert(rowIndex < targetRowCount);

				heap_freetuple(sampleRows[rowIndex]);
				sampleRows[rowIndex] = heap_form_tuple(tupleDescriptor,
													   columnValues,
													   columnNulls);
			}

			rowCountToSkip -= 1;
		}

		rowCount += 1;
	}

	/* Only clean up the query struct, but not its data */
	bsonDestroy(queryDocument);

	/* Clean up */
	MemoryContextDelete(tupleContext);

	pfree(columnValues);
	pfree(columnNulls);

	/* Emit some interesting relation info */
	relationName = RelationGetRelationName(relation);
	ereport(errorLevel,
			(errmsg("\"%s\": collection contains %.0f rows; %d rows in sample",
					relationName, rowCount, sampleRowCount)));

	(*totalRowCount) = rowCount;
	(*totalDeadRowCount) = 0;

	return sampleRowCount;
}

Datum
mongo_fdw_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(CODE_VERSION);
}

/*
 * mongoBeginForeignInsert
 * 		Prepare for an insert operation triggered by partition routing
 * 		or COPY FROM.
 *
 * This is not yet supported, so raise an error.
 */
static void
mongoBeginForeignInsert(ModifyTableState *mtstate,
						ResultRelInfo *resultRelInfo)
{
	ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
			 errmsg("COPY and foreign partition routing not supported in mongo_fdw")));
}

/*
 * mongoEndForeignInsert
 * 		BeginForeignInsert() is not yet implemented, hence we do not
 * 		have anything to cleanup as of now. We throw an error here just
 * 		to make sure when we do that we do not forget to cleanup
 * 		resources.
 */
static void
mongoEndForeignInsert(EState *estate, ResultRelInfo *resultRelInfo)
{
	ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_CREATE_EXECUTION),
			 errmsg("COPY and foreign partition routing not supported in mongo_fdw")));
}

/*
 * mongoGetForeignJoinPaths
 *		Add possible ForeignPath to joinrel, if the join is safe to push down.
 */
static void
mongoGetForeignJoinPaths(PlannerInfo *root, RelOptInfo *joinrel,
						 RelOptInfo *outerrel, RelOptInfo *innerrel,
						 JoinType jointype, JoinPathExtraData *extra)
{
	MongoFdwRelationInfo *fpinfo;
	ForeignPath *joinpath;
	Cost		startup_cost;
	Cost		total_cost;
	Path	   *epq_path = NULL;	/* Path to create plan to be executed when
									 * EvalPlanQual gets triggered. */
	MongoFdwRelationInfo *fpinfo_o;
	MongoFdwRelationInfo *fpinfo_i;

	/*
	 * Skip if this join combination has been considered already.
	 */
	if (joinrel->fdw_private)
		return;

	fpinfo_o = (MongoFdwRelationInfo *) outerrel->fdw_private;
	fpinfo_i = (MongoFdwRelationInfo *) innerrel->fdw_private;

	/* If join pushdown is not enabled, honor it. */
	if ((!IS_JOIN_REL(outerrel) && !fpinfo_o->options->enable_join_pushdown) ||
		(!IS_JOIN_REL(innerrel) && !fpinfo_i->options->enable_join_pushdown) ||
		!enable_join_pushdown)
		return;

	/*
	 * Create unfinished MongoFdwRelationInfo entry which is used to indicate
	 * that the join relation is already considered, so that we won't waste
	 * time in judging the safety of join pushdown and adding the same paths
	 * again if found safe.  Once we know that this join can be pushed down,
	 * we fill the entry.
	 */
	fpinfo = (MongoFdwRelationInfo *) palloc0(sizeof(MongoFdwRelationInfo));
	fpinfo->pushdown_safe = false;
	joinrel->fdw_private = fpinfo;

	/*
	 * In case there is a possibility that EvalPlanQual will be executed, we
	 * should be able to reconstruct the row, from base relations applying all
	 * the conditions.  We create a local plan from a suitable local path
	 * available in the path list.  In case such a path doesn't exist, we can
	 * not push the join to the foreign server since we won't be able to
	 * reconstruct the row for EvalPlanQual().  Find an alternative local path
	 * before we add ForeignPath, lest the new path would kick possibly the
	 * only local path.  Do this before calling mongo_foreign_join_ok(), since
	 * that function updates fpinfo and marks it as pushable if the join is
	 * found to be pushable.
	 */
	if (root->parse->commandType == CMD_DELETE ||
		root->parse->commandType == CMD_UPDATE ||
		root->rowMarks)
	{
		epq_path = GetExistingLocalJoinPath(joinrel);
		if (!epq_path)
		{
			elog(DEBUG3, "could not push down foreign join because a local path suitable for EPQ checks was not found");
			return;
		}
	}
	else
		epq_path = NULL;

	if (!mongo_foreign_join_ok(root, joinrel, jointype, outerrel, innerrel,
							   extra))
	{
		/* Free path required for EPQ if we copied one; we don't need it now */
		if (epq_path)
			pfree(epq_path);
		return;
	}

	/* TODO: Put accurate estimates here */
	startup_cost = 15.0;
	total_cost = 20 + startup_cost;

	/*
	 * Create a new join path and add it to the joinrel which represents a
	 * join between foreign tables.
	 */
#if PG_VERSION_NUM >= 120000
	joinpath = create_foreign_join_path(root,
										joinrel,
										NULL,
										joinrel->rows,
										startup_cost,
										total_cost,
										NIL,	/* no pathkeys */
										joinrel->lateral_relids,
										epq_path,
										NULL);	/* no fdw_private */
#else
	joinpath = create_foreignscan_path(root,
									   joinrel,
									   NULL,	/* default pathtarget */
									   joinrel->rows,
									   startup_cost,
									   total_cost,
									   NIL, /* no pathkeys */
									   joinrel->lateral_relids,
									   epq_path,
									   NIL);	/* no fdw_private */
#endif

	/* Add generated path into joinrel by add_path(). */
	add_path(joinrel, (Path *) joinpath);

	/* Add paths with pathkeys */
	mongo_add_paths_with_pathkeys(root, joinrel, epq_path, startup_cost,
								  total_cost);

	/* XXX Consider parameterized paths for the join relation */
}

/*
 * mongo_foreign_join_ok
 * 		Assess whether the join between inner and outer relations can be
 * 		pushed down to the foreign server.
 */
static bool
mongo_foreign_join_ok(PlannerInfo *root, RelOptInfo *joinrel,
					  JoinType jointype, RelOptInfo *outerrel,
					  RelOptInfo *innerrel, JoinPathExtraData *extra)
{
	MongoFdwRelationInfo *fpinfo;
	MongoFdwRelationInfo *fpinfo_o;
	MongoFdwRelationInfo *fpinfo_i;
	ListCell   *lc;
	List	   *joinclauses = NIL;
	List	   *scan_var_list;
	RangeTblEntry *rte;
	char	   *colname;

	/* We support pushing down only INNER, LEFT, RIGHT OUTER join */
	if (jointype != JOIN_INNER && jointype != JOIN_LEFT &&
		jointype != JOIN_RIGHT)
		return false;

	fpinfo = (MongoFdwRelationInfo *) joinrel->fdw_private;
	fpinfo_o = (MongoFdwRelationInfo *) outerrel->fdw_private;
	fpinfo_i = (MongoFdwRelationInfo *) innerrel->fdw_private;

	/* Recursive joins can't be pushed down */
	if (IS_JOIN_REL(outerrel) || IS_JOIN_REL(innerrel))
		return false;

	/*
	 * If either of the joining relations is marked as unsafe to pushdown, the
	 * join can not be pushed down.
	 */
	if (!fpinfo_o || !fpinfo_o->pushdown_safe ||
		!fpinfo_i || !fpinfo_i->pushdown_safe)
		return false;

	/*
	 * If joining relations have local conditions, those conditions are
	 * required to be applied before joining the relations.  Hence the join
	 * can not be pushed down.
	 */
	if (fpinfo_o->local_conds || fpinfo_i->local_conds)
		return false;

	scan_var_list = pull_var_clause((Node *) joinrel->reltarget->exprs,
									PVC_RECURSE_PLACEHOLDERS);

	/*
	 * Don't push-down join when whole row reference and/or full document
	 * retrieval is involved in the target list.
	 */
	foreach(lc, scan_var_list)
	{
		Var		   *var = lfirst(lc);

		Assert(IsA(var, Var));

		/* Don't support whole row reference. */
		if (var->varattno == 0)
			return false;

		rte = planner_rt_fetch(var->varno, root);
		colname = get_attname(rte->relid, var->varattno, false);

		/* Don't support full document retrieval */
		if (strcmp("__doc", colname) == 0)
			return false;
	}

	/*
	 * Separate restrict list into join quals and pushed-down (other) quals.
	 *
	 * Join quals belonging to an outer join must all be shippable, else we
	 * cannot execute the join remotely.  Add such quals to 'joinclauses'.
	 *
	 * Add other quals to fpinfo->remote_conds if they are shippable, else to
	 * fpinfo->local_conds.  In an inner join it's okay to execute conditions
	 * either locally or remotely; the same is true for pushed-down conditions
	 * at an outer join.
	 *
	 * Note we might return failure after having already scribbled on
	 * fpinfo->remote_conds and fpinfo->local_conds.  That's okay because we
	 * won't consult those lists again if we deem the join unshippable.
	 */
	joinclauses = NIL;
	foreach(lc, extra->restrictlist)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
		bool		is_remote_clause = mongo_is_foreign_expr(root,
															 joinrel,
															 rinfo->clause,
															 false);

		if (IS_OUTER_JOIN(jointype) &&
			!RINFO_IS_PUSHED_DOWN(rinfo, joinrel->relids))
		{
			if (!is_remote_clause)
				return false;
			joinclauses = lappend(joinclauses, rinfo);
		}
		else
		{
			if (is_remote_clause && jointype == JOIN_INNER)
			{
				/*
				 * Unlike postgres_fdw, for inner join, don't append the join
				 * clauses to remote_conds, instead keep the join clauses
				 * separate.  Currently, we are providing limited operator
				 * push-ability support for join pushdown, hence we keep those
				 * clauses separate to avoid INNER JOIN not getting pushdown
				 * if any of the WHERE clauses are not shippable as per join
				 * pushdown shippability.
				 */
				joinclauses = lappend(joinclauses, rinfo);
			}
			else
				fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
		}
	}

	/*
	 * If there's some PlaceHolderVar that would need to be evaluated within
	 * this join tree (because there's an upper reference to a quantity that
	 * may go to NULL as a result of an outer join), then we can't try to push
	 * the join down.
	 */
	foreach(lc, root->placeholder_list)
	{
		PlaceHolderInfo *phinfo = lfirst(lc);
		Relids		relids;

		/* PlaceHolderInfo refers to parent relids, not child relids. */
		relids = IS_OTHER_REL(joinrel) ?
			joinrel->top_parent_relids : joinrel->relids;

		if (bms_is_subset(phinfo->ph_eval_at, relids) &&
			bms_nonempty_difference(relids, phinfo->ph_eval_at))
			return false;
	}

	/* Save the join clauses, for later use. */
	fpinfo->joinclauses = joinclauses;
	fpinfo->outerrel = outerrel;
	fpinfo->innerrel = innerrel;
	fpinfo->jointype = jointype;

	/*
	 * Pull the other remote conditions from the joining relations into join
	 * clauses or other remote clauses (remote_conds) of this relation.  This
	 * avoids building sub-queries at every join step.
	 *
	 * For an INNER and OUTER join, the clauses from the outer side are added
	 * to remote_conds since those can be evaluated after the join is
	 * evaluated.  The clauses from the inner side are added to the
	 * joinclauses, since they need to be evaluated while constructing the
	 * join.
	 *
	 * The joining sides cannot have local conditions, thus no need to test
	 * the shippability of the clauses being pulled up.
	 */
	switch (jointype)
	{
		case JOIN_INNER:
		case JOIN_LEFT:
			fpinfo->joinclauses = mongo_list_concat(fpinfo->joinclauses,
													fpinfo_i->remote_conds);
			fpinfo->remote_conds = mongo_list_concat(fpinfo->remote_conds,
													 fpinfo_o->remote_conds);
			break;
		case JOIN_RIGHT:
			fpinfo->joinclauses = mongo_list_concat(fpinfo->joinclauses,
													fpinfo_o->remote_conds);
			fpinfo->remote_conds = mongo_list_concat(fpinfo->remote_conds,
													 fpinfo_i->remote_conds);
			break;
		default:
			/* Should not happen, we have just checked this above */
			elog(ERROR, "unsupported join type %d", jointype);
	}

	fpinfo->outer_relname = fpinfo_o->base_relname;
	fpinfo->inner_relname = fpinfo_i->base_relname;

	/* Mark that this join can be pushed down safely */
	fpinfo->pushdown_safe = true;

	/* Joinrel's aggregation flag depends on each joining relation's flag. */
	fpinfo->is_agg_scanrel_pushable = fpinfo_o->is_agg_scanrel_pushable &&
		fpinfo_i->is_agg_scanrel_pushable;

	/* Set the flag is_order_by_pushable of the join relation */
	fpinfo->is_order_by_pushable = fpinfo_o->is_order_by_pushable &&
		fpinfo_i->is_order_by_pushable;

	/*
	 * Set the string describing this join relation to be used in EXPLAIN
	 * output of the corresponding ForeignScan.
	 */
	fpinfo->relation_name = makeStringInfo();
	appendStringInfo(fpinfo->relation_name, "(%s) %s JOIN (%s)",
					 fpinfo_o->relation_name->data,
					 mongo_get_jointype_name(fpinfo->jointype),
					 fpinfo_i->relation_name->data);

	return true;
}

/*
 * mongo_prepare_qual_info
 *		Gather information of columns involved in the quals by extracting
 *		clause from each qual and process it further using mongo_check_qual().
 */
static void
mongo_prepare_qual_info(List *quals, MongoRelQualInfo *qual_info)
{
	ListCell   *lc;

	foreach(lc, quals)
	{
		Expr	   *expr = (Expr *) lfirst(lc);

		/* Extract clause from RestrictInfo */
		if (IsA(expr, RestrictInfo))
		{
			RestrictInfo *ri = (RestrictInfo *) expr;

			expr = ri->clause;
		}

		mongo_check_qual(expr, qual_info);
	}
}

/*
 * mongo_foreign_grouping_ok
 * 		Assess whether the aggregation, grouping and having operations can
 * 		be pushed down to the foreign server.  As a side effect, save
 * 		information we obtain in this function to MongoFdwRelationInfo of
 * 		the input relation.
 */
static bool
mongo_foreign_grouping_ok(PlannerInfo *root, RelOptInfo *grouped_rel,
						  Node *havingQual)
{
	Query	   *query = root->parse;
	PathTarget *grouping_target = grouped_rel->reltarget;
	MongoFdwRelationInfo *fpinfo = (MongoFdwRelationInfo *) grouped_rel->fdw_private;
	MongoFdwRelationInfo *ofpinfo = (MongoFdwRelationInfo *) fpinfo->outerrel->fdw_private;
	ListCell   *lc;
	int			i;
	List	   *tlist = NIL;

	/* Grouping Sets are not pushable */
	if (query->groupingSets)
		return false;

	/*
	 * If underneath input relation has any local conditions, those conditions
	 * are required to be applied before performing aggregation.  Hence the
	 * aggregate cannot be pushed down.
	 */
	if (ofpinfo->local_conds)
		return false;

	/*
	 * Evaluate grouping targets and check whether they are safe to push down
	 * to the foreign side.  All GROUP BY expressions will be part of the
	 * grouping target and thus there is no need to evaluate them separately.
	 * While doing so, add required expressions into the target list which can
	 * then be used to pass to a foreign server.
	 */
	i = 0;
	foreach(lc, grouping_target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc);
		Index		sgref = get_pathtarget_sortgroupref(grouping_target, i);
		ListCell   *l;

		/* Check whether this expression is part of GROUP BY clause */
		if (sgref && get_sortgroupref_clause_noerr(sgref, query->groupClause))
		{
			TargetEntry *tle;

			/*
			 * If any of the GROUP BY expression is not shippable we can not
			 * push down aggregation to the foreign server.
			 */
			if (!mongo_is_foreign_expr(root, grouped_rel, expr, false))
				return false;

			/* Add column in group by column list */
			ofpinfo->groupbyColList = lappend(ofpinfo->groupbyColList, expr);

			/*
			 * If it would be a foreign param, we can't put it into the tlist,
			 * so we have to fail.
			 */
			if (mongo_is_foreign_param(root, grouped_rel, expr))
				return false;

			/*
			 * Pushable, so add to tlist.  We need to create a TLE for this
			 * expression and apply the sortgroupref to it.  We cannot use
			 * add_to_flat_tlist() here because that avoids making duplicate
			 * entries in the tlist.  If there are duplicate entries with
			 * distinct sortgrouprefs, we have to duplicate that situation in
			 * the output tlist.
			 */
			tle = makeTargetEntry(expr, list_length(tlist) + 1, NULL, false);
			tle->ressortgroupref = sgref;
			tlist = lappend(tlist, tle);
		}
		else
		{
			/* Check entire expression whether it is pushable or not */
			if (mongo_is_foreign_expr(root, grouped_rel, expr, false) &&
				!mongo_is_foreign_param(root, grouped_rel, expr))
			{
				/* Pushable, add to tlist */
				tlist = add_to_flat_tlist(tlist, list_make1(expr));
			}
			else
			{
				List	   *aggvars;

				/* Not matched exactly, pull the var with aggregates then */
				aggvars = pull_var_clause((Node *) expr,
										  PVC_INCLUDE_AGGREGATES);

				/*
				 * If any aggregate expression is not shippable, then we
				 * cannot push down aggregation to the foreign server.
				 */
				if (!mongo_is_foreign_expr(root, grouped_rel, (Expr *) aggvars,
										   false))
					return false;

				/*
				 * Add aggregates, if any, into the targetlist.  Plain var
				 * nodes should be either same as some GROUP BY expression or
				 * part of some GROUP BY expression. In later case, the query
				 * cannot refer plain var nodes without the surrounding
				 * expression.  In both the cases, they are already part of
				 * the targetlist and thus no need to add them again.  In fact
				 * adding pulled plain var nodes in SELECT clause will cause
				 * an error on the foreign server if they are not same as some
				 * GROUP BY expression.
				 */
				foreach(l, aggvars)
				{
					expr = (Expr *) lfirst(l);

					if (IsA(expr, Aggref))
						tlist = add_to_flat_tlist(tlist, list_make1(expr));
				}
			}
		}

		i++;
	}

	/*
	 * Classify the pushable and non-pushable having clauses and save them in
	 * remote_conds and local_conds of the grouped rel's fpinfo.
	 */
	if (havingQual)
	{
		foreach(lc, (List *) havingQual)
		{
			Expr	   *expr = (Expr *) lfirst(lc);
			RestrictInfo *rinfo;

			/*
			 * Currently, the core code doesn't wrap havingQuals in
			 * RestrictInfos, so we must make our own.
			 */
			Assert(!IsA(expr, RestrictInfo));
#if PG_VERSION_NUM >= 160000
			rinfo = make_restrictinfo(root,
									  expr,
									  true,
									  false,
									  false,
									  false,
									  root->qual_security_level,
									  grouped_rel->relids,
									  NULL,
									  NULL);
#elif PG_VERSION_NUM >= 140000
			rinfo = make_restrictinfo(root,
									  expr,
									  true,
									  false,
									  false,
									  root->qual_security_level,
									  grouped_rel->relids,
									  NULL,
									  NULL);
#else
			rinfo = make_restrictinfo(expr,
									  true,
									  false,
									  false,
									  root->qual_security_level,
									  grouped_rel->relids,
									  NULL,
									  NULL);
#endif

			if (!mongo_is_foreign_expr(root, grouped_rel, expr, true))
				fpinfo->local_conds = lappend(fpinfo->local_conds, rinfo);
			else
				fpinfo->remote_conds = lappend(fpinfo->remote_conds, rinfo);
		}
	}

	/*
	 * If there are any local conditions, pull Vars and aggregates from it and
	 * check whether they are safe to pushdown or not.
	 */
	if (fpinfo->local_conds)
	{
		List	   *aggvars = NIL;

		foreach(lc, fpinfo->local_conds)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			aggvars = list_concat(aggvars,
								  pull_var_clause((Node *) rinfo->clause,
												  PVC_INCLUDE_AGGREGATES));
		}

		foreach(lc, aggvars)
		{
			Expr	   *expr = (Expr *) lfirst(lc);

			/*
			 * If aggregates within local conditions are not safe to push
			 * down, then we cannot push down the query.  Vars are already
			 * part of GROUP BY clause which are checked above, so no need to
			 * access them again here.
			 */
			if (IsA(expr, Aggref))
			{
				if (!mongo_is_foreign_expr(root, grouped_rel, expr, false))
					return false;

				tlist = add_to_flat_tlist(tlist, list_make1(expr));
			}
		}
	}

	/* Store generated targetlist */
	fpinfo->grouped_tlist = tlist;

	/* Safe to pushdown */
	fpinfo->pushdown_safe = true;

	/*
	 * Set the string describing this grouped relation to be used in EXPLAIN
	 * output of corresponding ForeignScan.
	 */
	fpinfo->relation_name = makeStringInfo();
	appendStringInfo(fpinfo->relation_name, "Aggregate on (%s)",
					 ofpinfo->relation_name->data);

	return true;
}

/*
 * mongoGetForeignUpperPaths
 *		Add paths for post-join operations like aggregation, grouping etc. if
 *		corresponding operations are safe to push down.
 */
static void
mongoGetForeignUpperPaths(PlannerInfo *root, UpperRelationKind stage,
						  RelOptInfo *input_rel, RelOptInfo *output_rel,
						  void *extra)
{
	MongoFdwRelationInfo *fpinfo;

	/*
	 * If input rel is not safe to pushdown, then simply return as we cannot
	 * perform any post-join operations on the foreign server.
	 */
	if (!input_rel->fdw_private ||
		!((MongoFdwRelationInfo *) input_rel->fdw_private)->pushdown_safe)
		return;

	/* Ignore stages we don't support; and skip any duplicate calls. */
#if PG_VERSION_NUM >= 120000
	if ((stage != UPPERREL_GROUP_AGG && stage != UPPERREL_ORDERED &&
		 stage != UPPERREL_FINAL) ||
#else
	if (stage != UPPERREL_GROUP_AGG ||
#endif
		output_rel->fdw_private)
		return;

	fpinfo = (MongoFdwRelationInfo *) palloc0(sizeof(MongoFdwRelationInfo));
	fpinfo->pushdown_safe = false;
	fpinfo->stage = stage;
	output_rel->fdw_private = fpinfo;

#if PG_VERSION_NUM >= 120000
	switch (stage)
	{
		case UPPERREL_GROUP_AGG:
			mongo_add_foreign_grouping_paths(root, input_rel, output_rel,
											 (GroupPathExtraData *) extra);
			break;
		case UPPERREL_ORDERED:
			mongo_add_foreign_ordered_paths(root, input_rel, output_rel);
			break;
		case UPPERREL_FINAL:
			mongo_add_foreign_final_paths(root, input_rel, output_rel,
										  (FinalPathExtraData *) extra);
			break;
		default:
			elog(ERROR, "unexpected upper relation: %d", (int) stage);
			break;
	}
#else
	mongo_add_foreign_grouping_paths(root, input_rel, output_rel,
									 (GroupPathExtraData *) extra);
#endif
}

/*
 * mongo_add_foreign_grouping_paths
 *		Add foreign path for grouping and/or aggregation.
 *
 * Given input_rel represents the underlying scan.  The paths are added to the
 * given grouped_rel.
 */
static void
mongo_add_foreign_grouping_paths(PlannerInfo *root, RelOptInfo *input_rel,
								 RelOptInfo *grouped_rel,
								 GroupPathExtraData *extra)
{
	Query	   *parse = root->parse;
	MongoFdwRelationInfo *fpinfo = grouped_rel->fdw_private;
	ForeignPath *grouppath;
	Cost		startup_cost;
	Cost		total_cost;
	double		num_groups;

	/* Nothing to be done, if there is no grouping or aggregation required. */
	if (!parse->groupClause && !parse->groupingSets && !parse->hasAggs &&
		!root->hasHavingQual)
		return;

	/* Save the input_rel as outerrel in fpinfo */
	fpinfo->outerrel = input_rel;

	/* Set aggregation flag of aggregate relation */
	fpinfo->is_agg_scanrel_pushable =
		((MongoFdwRelationInfo *) input_rel->fdw_private)->is_agg_scanrel_pushable;

	/* If aggregate pushdown is not enabled, honor it. */
	if (!enable_aggregate_pushdown || !fpinfo->is_agg_scanrel_pushable)
		return;

	/* Assess if it is safe to push down aggregation and grouping. */
	if (!mongo_foreign_grouping_ok(root, grouped_rel, extra->havingQual))
		return;

	fpinfo->is_order_by_pushable =
		((MongoFdwRelationInfo *) input_rel->fdw_private)->is_order_by_pushable;

	/*
	 * TODO: Put accurate estimates here.
	 *
	 * Cost used here is minimum of the cost estimated for base and join
	 * relation.
	 */
	startup_cost = 15;
	total_cost = 10 + startup_cost;

	/* Estimate output tuples which should be same as number of groups */
#if PG_VERSION_NUM >= 140000
	num_groups = estimate_num_groups(root,
									 get_sortgrouplist_exprs(root->parse->groupClause,
															 fpinfo->grouped_tlist),
									 input_rel->rows, NULL, NULL);
#else
	num_groups = estimate_num_groups(root,
									 get_sortgrouplist_exprs(root->parse->groupClause,
															 fpinfo->grouped_tlist),
									 input_rel->rows, NULL);
#endif

	/* Create and add foreign path to the grouping relation. */
#if PG_VERSION_NUM >= 120000
	grouppath = create_foreign_upper_path(root,
										  grouped_rel,
										  grouped_rel->reltarget,
										  num_groups,
										  startup_cost,
										  total_cost,
										  NIL,	/* no pathkeys */
										  NULL,
										  NIL); /* no fdw_private */
#else
	grouppath = create_foreignscan_path(root,
										grouped_rel,
										grouped_rel->reltarget,
										num_groups,
										startup_cost,
										total_cost,
										NIL,	/* no pathkeys */
										grouped_rel->lateral_relids,
										NULL,
										NIL);	/* no fdw_private */
#endif

	/* Add generated path into grouped_rel by add_path(). */
	add_path(grouped_rel, (Path *) grouppath);
}

/*
 * mongoEstimateCosts
 * 		Estimate the remote query cost
 */
static void
mongoEstimateCosts(RelOptInfo *baserel, Cost *startup_cost, Cost *total_cost,
				   Oid foreigntableid)
{
	MongoFdwOptions *options;

	/* Fetch options */
	options = mongo_get_options(foreigntableid);

	/* Local databases are probably faster */
	if (strcmp(options->svr_address, "127.0.0.1") == 0 ||
		strcmp(options->svr_address, "localhost") == 0)
		*startup_cost = 10;
	else
		*startup_cost = 25;

	*total_cost = baserel->rows + *startup_cost;
}

/*
 * mongo_get_useful_ecs_for_relation
 *		Determine which EquivalenceClasses might be involved in useful
 *		orderings of this relation.
 *
 * This function is in some respects a mirror image of the core function
 * pathkeys_useful_for_merging: for a regular table, we know what indexes
 * we have and want to test whether any of them are useful.  For a foreign
 * table, we don't know what indexes are present on the remote side but
 * want to speculate about which ones we'd like to use if they existed.
 *
 * This function returns a list of potentially-useful equivalence classes,
 * but it does not guarantee that an EquivalenceMember exists which contains
 * Vars only from the given relation.  For example, given ft1 JOIN t1 ON
 * ft1.x + t1.x = 0, this function will say that the equivalence class
 * containing ft1.x + t1.x is potentially useful.  Supposing ft1 is remote and
 * t1 is local (or on a different server), it will turn out that no useful
 * ORDER BY clause can be generated.  It's not our job to figure that out
 * here; we're only interested in identifying relevant ECs.
 */
static List *
mongo_get_useful_ecs_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *useful_eclass_list = NIL;
	ListCell   *lc;
	Relids		relids;

	/*
	 * First, consider whether any active EC is potentially useful for a merge
	 * join against this relation.
	 */
	if (rel->has_eclass_joins)
	{
		foreach(lc, root->eq_classes)
		{
			EquivalenceClass *cur_ec = (EquivalenceClass *) lfirst(lc);

			if (eclass_useful_for_merging(root, cur_ec, rel))
				useful_eclass_list = lappend(useful_eclass_list, cur_ec);
		}
	}

	/*
	 * Next, consider whether there are any non-EC derivable join clauses that
	 * are merge-joinable.  If the joininfo list is empty, we can exit
	 * quickly.
	 */
	if (rel->joininfo == NIL)
		return useful_eclass_list;

	/* If this is a child rel, we must use the topmost parent rel to search. */
	if (IS_OTHER_REL(rel))
	{
		Assert(!bms_is_empty(rel->top_parent_relids));
		relids = rel->top_parent_relids;
	}
	else
		relids = rel->relids;

	/* Check each join clause in turn. */
	foreach(lc, rel->joininfo)
	{
		RestrictInfo *restrictinfo = (RestrictInfo *) lfirst(lc);

		/* Consider only mergejoinable clauses */
		if (restrictinfo->mergeopfamilies == NIL)
			continue;

		/* Make sure we've got canonical ECs. */
		update_mergeclause_eclasses(root, restrictinfo);

		/*
		 * restrictinfo->mergeopfamilies != NIL is sufficient to guarantee
		 * that left_ec and right_ec will be initialized, per comments in
		 * distribute_qual_to_rels.
		 *
		 * We want to identify which side of this merge-joinable clause
		 * contains columns from the relation produced by this RelOptInfo. We
		 * test for overlap, not containment, because there could be extra
		 * relations on either side.  For example, suppose we've got something
		 * like ((A JOIN B ON A.x = B.x) JOIN C ON A.y = C.y) LEFT JOIN D ON
		 * A.y = D.y.  The input rel might be the joinrel between A and B, and
		 * we'll consider the join clause A.y = D.y. relids contains a
		 * relation not involved in the join class (B) and the equivalence
		 * class for the left-hand side of the clause contains a relation not
		 * involved in the input rel (C).  Despite the fact that we have only
		 * overlap and not containment in either direction, A.y is potentially
		 * useful as a sort column.
		 *
		 * Note that it's even possible that relids overlaps neither side of
		 * the join clause.  For example, consider A LEFT JOIN B ON A.x = B.x
		 * AND A.x = 1.  The clause A.x = 1 will appear in B's joininfo list,
		 * but overlaps neither side of B.  In that case, we just skip this
		 * join clause, since it doesn't suggest a useful sort order for this
		 * relation.
		 */
		if (bms_overlap(relids, restrictinfo->right_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
														restrictinfo->right_ec);
		else if (bms_overlap(relids, restrictinfo->left_ec->ec_relids))
			useful_eclass_list = list_append_unique_ptr(useful_eclass_list,
														restrictinfo->left_ec);
	}

	return useful_eclass_list;
}

/*
 * mongo_get_useful_pathkeys_for_relation
 *		Determine which orderings of a relation might be useful.
 *
 * Getting data in sorted order can be useful either because the requested
 * order matches the final output ordering for the overall query we're
 * planning, or because it enables an efficient merge join.  Here, we try
 * to figure out which pathkeys to consider.
 *
 * MongoDB considers null values as the "smallest" ones, so they appear first
 * when sorting in ascending order, and appear last when sorting in descending
 * order.  MongoDB doesn't have provision for "NULLS FIRST" and "NULLS LAST"
 * like syntaxes.  So, by considering all these restrictions from MongoDB, we
 * can support push-down of only below two cases of the ORDER BY clause:
 *
 * 1. ORDER BY <expr> ASC NULLS FIRST
 * 2. ORDER BY <expr> DESC NULLS LAST
 *
 * Where, expr can only be a column and not any expression because MongoDB
 * sorts only on fields.  Multiple columns can be provided.
 */
static List *
mongo_get_useful_pathkeys_for_relation(PlannerInfo *root, RelOptInfo *rel)
{
	List	   *useful_pathkeys_list = NIL;
	List	   *useful_eclass_list;
	MongoFdwRelationInfo *fpinfo = (MongoFdwRelationInfo *) rel->fdw_private;
	EquivalenceClass *query_ec = NULL;
	ListCell   *lc;

	/*
	 * Pushing the query_pathkeys to the remote server is always worth
	 * considering, because it might let us avoid a local sort.
	 */
	fpinfo->qp_is_pushdown_safe = false;
	if (root->query_pathkeys)
	{
		bool		query_pathkeys_ok = true;

		foreach(lc, root->query_pathkeys)
		{
			PathKey    *pathkey = (PathKey *) lfirst(lc);

			/* Only ASC NULLS FIRST and DESC NULLS LAST can be pushed down */
			if (!IS_PATHKEY_PUSHABLE(pathkey))
			{
				query_pathkeys_ok = false;
				break;
			}

			/*
			 * The planner and executor don't have any clever strategy for
			 * taking data sorted by a prefix of the query's pathkeys and
			 * getting it to be sorted by all of those pathkeys. We'll just
			 * end up resorting the entire data set.  So, unless we can push
			 * down all of the query pathkeys, forget it.
			 */
			if (!mongo_is_foreign_pathkey(root, rel, pathkey))
			{
				query_pathkeys_ok = false;
				break;
			}
		}

		if (query_pathkeys_ok)
		{
			useful_pathkeys_list = list_make1(list_copy(root->query_pathkeys));
			fpinfo->qp_is_pushdown_safe = true;
		}
	}

	/* Get the list of interesting EquivalenceClasses. */
	useful_eclass_list = mongo_get_useful_ecs_for_relation(root, rel);

	/* Extract unique EC for query, if any, so we don't consider it again. */
	if (list_length(root->query_pathkeys) == 1)
	{
		PathKey    *query_pathkey = linitial(root->query_pathkeys);

		query_ec = query_pathkey->pk_eclass;
	}

	/*
	 * As a heuristic, the only pathkeys we consider here are those of length
	 * one.  It's surely possible to consider more, but since each one we
	 * choose to consider will generate a round-trip to the remote side, we
	 * need to be a bit cautious here.  It would sure be nice to have a local
	 * cache of information about remote index definitions...
	 */
	foreach(lc, useful_eclass_list)
	{
		EquivalenceClass *cur_ec = lfirst(lc);
		EquivalenceMember *em;
		Expr	   *em_expr;
		PathKey    *pathkey;

		/* If redundant with what we did above, skip it. */
		if (cur_ec == query_ec)
			continue;

		/* Can't push down the sort if the EC's opfamily is not shippable. */
		if (!mongo_is_builtin(linitial_oid(cur_ec->ec_opfamilies)))
			continue;

		/* If no pushable expression for this rel, skip it. */
		if (!(em = mongo_find_em_for_rel(root, cur_ec, rel)))
			continue;

		/* Ignore binary-compatible relabeling */
		em_expr = em->em_expr;
		while (em_expr && IsA(em_expr, RelabelType))
			em_expr = ((RelabelType *) em_expr)->arg;

		/* Only Vars are allowed per MongoDB. */
		if (!IsA(em_expr, Var))
			continue;

		/* Looks like we can generate a pathkey, so let's do it. */
		pathkey = make_canonical_pathkey(root, cur_ec,
										 linitial_oid(cur_ec->ec_opfamilies),
										 BTLessStrategyNumber,
										 false);
		if (!IS_PATHKEY_PUSHABLE(pathkey))
			continue;

		/* Check for sort operator pushability. */
		if (!mongo_is_default_sort_operator(em, pathkey))
			continue;

		useful_pathkeys_list = lappend(useful_pathkeys_list,
									   list_make1(pathkey));
	}

	return useful_pathkeys_list;
}

/*
 * mongo_add_paths_with_pathkeys
 *		 Add path with root->query_pathkeys if that's pushable.
 *
 * Pushing down query_pathkeys to the foreign server might let us avoid a
 * local sort.
 */
static void
mongo_add_paths_with_pathkeys(PlannerInfo *root, RelOptInfo *rel,
							  Path *epq_path, Cost base_startup_cost,
							  Cost base_total_cost)
{
	ListCell   *lc;
	List	   *useful_pathkeys_list = NIL; /* List of all pathkeys */

	/* If orderby pushdown is not enabled, honor it. */
	if (!enable_order_by_pushdown  ||
		!((MongoFdwRelationInfo *) rel->fdw_private)->is_order_by_pushable)
		return;

	/*
	 * Check the query pathkeys length.  Don't push when exceeding the limit
	 * set by MongoDB.
	 */
	if (list_length(root->query_pathkeys) > MAX_PATHKEYS)
		return;

	useful_pathkeys_list = mongo_get_useful_pathkeys_for_relation(root, rel);

	/* Create one path for each set of pathkeys we found above. */
	foreach(lc, useful_pathkeys_list)
	{
		Cost		startup_cost;
		Cost		total_cost;
		List	   *useful_pathkeys = lfirst(lc);
		Path	   *sorted_epq_path;

		/* TODO put accurate estimates. */
		startup_cost = base_startup_cost * DEFAULT_MONGO_SORT_MULTIPLIER;
		total_cost = base_total_cost * DEFAULT_MONGO_SORT_MULTIPLIER;

		/*
		 * The EPQ path must be at least as well sorted as the path itself, in
		 * case it gets used as input to a mergejoin.
		 */
		sorted_epq_path = epq_path;
		if (sorted_epq_path != NULL &&
			!pathkeys_contained_in(useful_pathkeys,
								   sorted_epq_path->pathkeys))
			sorted_epq_path = (Path *)
				create_sort_path(root,
								 rel,
								 sorted_epq_path,
								 useful_pathkeys,
								 -1.0);

#if PG_VERSION_NUM >= 120000
		if (IS_SIMPLE_REL(rel))
			add_path(rel, (Path *)
					 create_foreignscan_path(root, rel,
											 NULL,
											 rel->rows,
											 startup_cost,
											 total_cost,
											 useful_pathkeys,
											 rel->lateral_relids,
											 sorted_epq_path,
											 NIL));
		else
			add_path(rel, (Path *)
					 create_foreign_join_path(root, rel,
											  NULL,
											  rel->rows,
											  startup_cost,
											  total_cost,
											  useful_pathkeys,
											  rel->lateral_relids,
											  sorted_epq_path,
											  NIL));
#else
		add_path(rel, (Path *)
				 create_foreignscan_path(root, rel,
										 NULL,
										 rel->rows,
										 startup_cost,
										 total_cost,
										 useful_pathkeys,
										 rel->lateral_relids,
										 sorted_epq_path,
										 NIL));
#endif
	}
}

/*
 * mongo_find_em_for_rel
 * 		Find an equivalence class member expression, all of whose Vars, come
 * 		from the indicated relation.
 */
EquivalenceMember *
mongo_find_em_for_rel(PlannerInfo *root, EquivalenceClass *ec, RelOptInfo *rel)
{
	ListCell   *lc_em;

	foreach(lc_em, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc_em);

		/*
		 * Note we require !bms_is_empty, else we'd accept constant
		 * expressions which are not suitable for the purpose.
		 */
		if (bms_is_subset(em->em_relids, rel->relids) &&
			!bms_is_empty(em->em_relids) &&
			mongo_is_foreign_expr(root, rel, em->em_expr, false))
		{
			/*
			 * If there is more than one equivalence member whose Vars are
			 * taken entirely from this relation, we'll be content to choose
			 * any one of those.
			 */
			return em;
		}
	}

	/* We didn't find any suitable equivalence class expression */
	return NULL;
}

/*
 * mongo_add_foreign_ordered_paths
 *		Add foreign paths for performing the final sort remotely.
 *
 * Given input_rel contains the source-data Paths.  The paths are added to the
 * given ordered_rel.
 */
#if PG_VERSION_NUM >= 120000
static void
mongo_add_foreign_ordered_paths(PlannerInfo *root, RelOptInfo *input_rel,
								RelOptInfo *ordered_rel)
{
	Query	   *parse = root->parse;
	MongoFdwRelationInfo *ifpinfo = input_rel->fdw_private;
	MongoFdwRelationInfo *fpinfo = ordered_rel->fdw_private;
	double		rows;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *fdw_private;
	ForeignPath *ordered_path;
	ListCell   *lc;

	/* Set the flag is_order_by_pushable of the ordered relation */
	fpinfo->is_order_by_pushable =
		((MongoFdwRelationInfo *) input_rel->fdw_private)->is_order_by_pushable;

	/* If orderby pushdown is not enabled, honor it. */
	if (!enable_order_by_pushdown || !fpinfo->is_order_by_pushable)
		return;

	/* Shouldn't get here unless the query has ORDER BY */
	Assert(parse->sortClause);

	/* We don't support cases where there are any SRFs in the targetlist */
	if (parse->hasTargetSRFs)
		return;

	/*
	 * Check the query pathkeys length.  Don't push when exceeding the limit
	 * set by MongoDB.
	 */
	if (list_length(root->query_pathkeys) > MAX_PATHKEYS)
		return;

	/* Save the input_rel as outerrel in fpinfo */
	fpinfo->outerrel = input_rel;

	/*
	 * If the input_rel is a base or join relation, we would already have
	 * considered pushing down the final sort to the remote server when
	 * creating pre-sorted foreign paths for that relation, because the
	 * query_pathkeys is set to the root->sort_pathkeys in that case (see
	 * standard_qp_callback()).
	 */
	if (input_rel->reloptkind == RELOPT_BASEREL ||
		input_rel->reloptkind == RELOPT_JOINREL)
	{
		Assert(root->query_pathkeys == root->sort_pathkeys);

		/* Safe to push down  */
		fpinfo->pushdown_safe = ifpinfo->qp_is_pushdown_safe;

		return;
	}

	/* The input_rel should be a grouping relation */
	Assert(input_rel->reloptkind == RELOPT_UPPER_REL &&
		   ifpinfo->stage == UPPERREL_GROUP_AGG);

	/*
	 * We try to create a path below by extending a simple foreign path for
	 * the underlying grouping relation to perform the final sort remotely,
	 * which is stored into the fdw_private list of the resulting path.
	 */

	/* Assess if it is safe to push down the final sort */
	foreach(lc, root->sort_pathkeys)
	{
		PathKey    *pathkey = (PathKey *) lfirst(lc);
		EquivalenceClass *pathkey_ec = pathkey->pk_eclass;
		EquivalenceMember *em = NULL;
		Expr	   *sort_expr;

		/*
		 * mongo_is_foreign_expr would detect volatile expressions as well,
		 * but checking ec_has_volatile here saves some cycles.
		 */
		if (pathkey_ec->ec_has_volatile)
			return;

		if (!IS_PATHKEY_PUSHABLE(pathkey))
			return;

		/*
		 * Get the sort expression for the pathkey_ec.  The EC must contain a
		 * shippable EM that is computed in input_rel's reltarget, else we
		 * can't push down the sort.
		 */
		em = mongo_find_em_for_rel_target(root, pathkey_ec, input_rel);

		/* Check for sort operator pushability. */
		if (!mongo_is_default_sort_operator(em, pathkey))
			return;

		/* Ignore binary-compatible relabeling */
		sort_expr = em->em_expr;
		while (sort_expr && IsA(sort_expr, RelabelType))
			sort_expr = ((RelabelType *) sort_expr)->arg;

		/* Only Vars are allowed per MongoDB. */
		if (!IsA(sort_expr, Var))
			return;
	}

	/* Safe to push down */
	fpinfo->pushdown_safe = true;

	/* TODO: Put accurate estimates */
	startup_cost = 10;
	total_cost = 10 + startup_cost;
	rows = 10;

	/*
	 * Build the fdw_private list that will be used by mongoGetForeignPlan.
	 * Items in the list must match the order in the enum FdwPathPrivateIndex.
	 */
	fdw_private = list_make2(makeInteger(true), makeInteger(false));

	/* Create foreign ordering path */
	ordered_path = create_foreign_upper_path(root,
											 input_rel,
											 root->upper_targets[UPPERREL_ORDERED],
											 rows,
											 startup_cost,
											 total_cost,
											 root->sort_pathkeys,
											 NULL,	/* no extra plan */
											 fdw_private);

	/* and add it to the ordered_rel */
	add_path(ordered_rel, (Path *) ordered_path);
}

/*
 * mongo_add_foreign_final_paths
 *		Add foreign paths for performing the final processing remotely.
 *
 * Given input_rel contains the source-data Paths.  The paths are added to the
 * given final_rel.
 */
static void
mongo_add_foreign_final_paths(PlannerInfo *root, RelOptInfo *input_rel,
							  RelOptInfo *final_rel, FinalPathExtraData *extra)
{
	Query	   *parse = root->parse;
	MongoFdwRelationInfo *ifpinfo = (MongoFdwRelationInfo *) input_rel->fdw_private;
	MongoFdwRelationInfo *fpinfo = (MongoFdwRelationInfo *) final_rel->fdw_private;
	bool		has_final_sort = false;
	List	   *pathkeys = NIL;
	double		rows;
	Cost		startup_cost;
	Cost		total_cost;
	List	   *fdw_private;
	ForeignPath *final_path;

	/*
	 * Currently, we only support this for SELECT commands
	 */
	if (parse->commandType != CMD_SELECT)
		return;

	/*
	 * We do not support LIMIT with FOR UPDATE/SHARE.  Also, if there is no
	 * FOR UPDATE/SHARE clause and there is no LIMIT, don't need to add
	 * Foreign final path.
	 */
	if (parse->rowMarks || !extra->limit_needed)
		return;

	/* We don't support cases where there are any SRFs in the targetlist */
	if (parse->hasTargetSRFs)
		return;

	/* Save the input_rel as outerrel in fpinfo */
	fpinfo->outerrel = input_rel;

	/*
	 * If there is no need to add a LIMIT node, there might be a ForeignPath
	 * in the input_rel's pathlist that implements all behavior of the query.
	 * Note: we would already have accounted for the query's FOR UPDATE/SHARE
	 * (if any) before we get here.
	 */
	if (!extra->limit_needed)
	{
		ListCell   *lc;

		Assert(parse->rowMarks);

		/*
		 * Grouping and aggregation are not supported with FOR UPDATE/SHARE,
		 * so the input_rel should be a base, join, or ordered relation; and
		 * if it's an ordered relation, its input relation should be a base or
		 * join relation.
		 */
		Assert(input_rel->reloptkind == RELOPT_BASEREL ||
			   input_rel->reloptkind == RELOPT_JOINREL ||
			   (input_rel->reloptkind == RELOPT_UPPER_REL &&
				ifpinfo->stage == UPPERREL_ORDERED &&
				(ifpinfo->outerrel->reloptkind == RELOPT_BASEREL ||
				 ifpinfo->outerrel->reloptkind == RELOPT_JOINREL)));

		foreach(lc, input_rel->pathlist)
		{
			Path	   *path = (Path *) lfirst(lc);

			/*
			 * apply_scanjoin_target_to_paths() uses create_projection_path()
			 * to adjust each of its input paths if needed, whereas
			 * create_ordered_paths() uses apply_projection_to_path() to do
			 * that.  So the former might have put a ProjectionPath on top of
			 * the ForeignPath; look through ProjectionPath and see if the
			 * path underneath it is ForeignPath.
			 */
			if (IsA(path, ForeignPath) ||
				(IsA(path, ProjectionPath) &&
				 IsA(((ProjectionPath *) path)->subpath, ForeignPath)))
			{
				/*
				 * Create foreign final path; this gets rid of a
				 * no-longer-needed outer plan (if any), which makes the
				 * EXPLAIN output look cleaner
				 */
				final_path = create_foreign_upper_path(root,
													   path->parent,
													   path->pathtarget,
													   path->rows,
													   path->startup_cost,
													   path->total_cost,
													   path->pathkeys,
													   NULL,	/* no extra plan */
													   NULL);	/* no fdw_private */

				/* and add it to the final_rel */
				add_path(final_rel, (Path *) final_path);

				/* Safe to push down */
				fpinfo->pushdown_safe = true;

				return;
			}
		}

		/*
		 * If we get here it means no ForeignPaths; since we would already
		 * have considered pushing down all operations for the query to the
		 * remote server, give up on it.
		 */
		return;
	}

	Assert(extra->limit_needed);

	/*
	 * If the input_rel is an ordered relation, replace the input_rel with its
	 * input relation
	 */
	if (input_rel->reloptkind == RELOPT_UPPER_REL &&
		ifpinfo->stage == UPPERREL_ORDERED)
	{
		/* Do not push down LIMIT if ORDER BY push down is disabled */
		if (!enable_order_by_pushdown)
			return;

		input_rel = ifpinfo->outerrel;
		ifpinfo = (MongoFdwRelationInfo *) input_rel->fdw_private;
		has_final_sort = true;
		pathkeys = root->sort_pathkeys;
	}

	/* The input_rel should be a base, join, or grouping relation */
	Assert(input_rel->reloptkind == RELOPT_BASEREL ||
		   input_rel->reloptkind == RELOPT_JOINREL ||
		   (input_rel->reloptkind == RELOPT_UPPER_REL &&
			ifpinfo->stage == UPPERREL_GROUP_AGG));

	/*
	 * We try to create a path below by extending a simple foreign path for
	 * the underlying base, join, or grouping relation to perform the final
	 * sort (if has_final_sort) and the LIMIT restriction remotely, which is
	 * stored into the fdw_private list of the resulting path.  (We
	 * re-estimate the costs of sorting the underlying relation, if
	 * has_final_sort.)
	 */

	/*
	 * Assess if it is safe to push down the LIMIT and OFFSET to the remote
	 * server
	 */

	/*
	 * If the underlying relation has any local conditions, the LIMIT/OFFSET
	 * cannot be pushed down.
	 */
	if (ifpinfo->local_conds)
		return;

	/*
	 * Support only Const nodes as expressions are NOT supported on MongoDB.
	 * Also, MongoDB supports only positive 64-bit integer values, so don't
	 * pushdown in case of -ve values given for LIMIT/OFFSET clauses.
	 */
	if (parse->limitCount)
	{
		Node	   *node = parse->limitCount;

		if (nodeTag(node) != T_Const ||
			(((Const *) node)->consttype != INT8OID))
			return;

		if (!((Const *) node)->constisnull &&
			(DatumGetInt64(((Const *) node)->constvalue) < 0))
			return;
	}
	if (parse->limitOffset)
	{
		Node	   *node = parse->limitOffset;

		if (nodeTag(node) != T_Const ||
			(((Const *) node)->consttype != INT8OID))
			return;

		if (!((Const *) node)->constisnull &&
			(DatumGetInt64(((Const *) node)->constvalue) < 0))
			return;
	}

	/* Safe to push down */
	fpinfo->pushdown_safe = true;

	/* TODO: Put accurate estimates */
	startup_cost = 1;
	total_cost = 1 + startup_cost;
	rows = 1;

	/*
	 * Build the fdw_private list that will be used by mongoGetForeignPlan.
	 * Items in the list must match order in enum FdwPathPrivateIndex.
	 */
	fdw_private = list_make2(makeInteger(has_final_sort),
							 makeInteger(extra->limit_needed));

	/*
	 * Create foreign final path; this gets rid of a no-longer-needed outer
	 * plan (if any), which makes the EXPLAIN output look cleaner
	 */
	final_path = create_foreign_upper_path(root,
										   input_rel,
										   root->upper_targets[UPPERREL_FINAL],
										   rows,
										   startup_cost,
										   total_cost,
										   pathkeys,
										   NULL,	/* no extra plan */
										   fdw_private);

	/* and add it to the final_rel */
	add_path(final_rel, (Path *) final_path);
}
#endif							/* PG_VERSION_NUM >= 120000 */

/*
 * mongo_find_em_for_rel_target
 * 		Find an equivalence class member expression to be computed as a sort
 * 		column in the given target.
 */
static EquivalenceMember *
mongo_find_em_for_rel_target(PlannerInfo *root, EquivalenceClass *ec,
							 RelOptInfo *rel)
{
	PathTarget *target = rel->reltarget;
	ListCell   *lc1;
	int			i;

	i = 0;
	foreach(lc1, target->exprs)
	{
		Expr	   *expr = (Expr *) lfirst(lc1);
		Index		sgref = get_pathtarget_sortgroupref(target, i);
		ListCell   *lc2;

		/* Ignore non-sort expressions */
		if (sgref == 0 ||
			get_sortgroupref_clause_noerr(sgref,
										  root->parse->sortClause) == NULL)
		{
			i++;
			continue;
		}

		/* We ignore binary-compatible relabeling */
		while (expr && IsA(expr, RelabelType))
			expr = ((RelabelType *) expr)->arg;

		/* Locate an EquivalenceClass member matching this expr, if any */
		foreach(lc2, ec->ec_members)
		{
			EquivalenceMember *em = (EquivalenceMember *) lfirst(lc2);
			Expr	   *em_expr;

			/* Don't match constants */
			if (em->em_is_const)
				continue;

			/* Ignore child members */
			if (em->em_is_child)
				continue;

			/* Match if same expression (after stripping relabel) */
			em_expr = em->em_expr;
			while (em_expr && IsA(em_expr, RelabelType))
				em_expr = ((RelabelType *) em_expr)->arg;

			if (!equal(em_expr, expr))
				continue;

			/*
			 * Check that expression (including relabels!) is shippable.  If
			 * it's unsafe to remote, we cannot push down the final sort.
			 */
			if (mongo_is_foreign_expr(root, rel, em->em_expr, false))
				return em;
		}

		i++;
	}

	return NULL;				/* keep compiler quiet */
}

/*
 * mongo_is_default_sort_operator
 *		Returns true if default sort operator is provided.
 */
bool
mongo_is_default_sort_operator(EquivalenceMember *em, PathKey *pathkey)
{
	Oid			oprid;
	char	   *oprname;
	TypeCacheEntry *typentry;

	if (em == NULL)
		return false;

	/* Can't push down the sort if pathkey's opfamily is not shippable. */
	if (!mongo_is_builtin(pathkey->pk_opfamily))
		return NULL;

	oprid = get_opfamily_member(pathkey->pk_opfamily,
								em->em_datatype,
								em->em_datatype,
								pathkey->pk_strategy);
	if (!OidIsValid(oprid))
		elog(ERROR, "missing operator %d(%u,%u) in opfamily %u",
			 pathkey->pk_strategy, em->em_datatype, em->em_datatype,
			 pathkey->pk_opfamily);

	/* Can't push down the sort if the operator is not shippable. */
	oprname = get_opname(oprid);
	if (!((strncmp(oprname, "<", NAMEDATALEN) == 0) ||
		  (strncmp(oprname, ">", NAMEDATALEN) == 0)))
		return false;

	/*
	 * See whether the operator is default < or > for sort expr's datatype.
	 * Here we need to use the expression's actual type to discover whether
	 * the desired operator will be the default or not.
	 */
	typentry = lookup_type_cache(exprType((Node *) em->em_expr),
								 TYPECACHE_LT_OPR | TYPECACHE_GT_OPR);
	if (oprid == typentry->lt_opr || oprid == typentry->gt_opr)
		return true;

	return false;
}
