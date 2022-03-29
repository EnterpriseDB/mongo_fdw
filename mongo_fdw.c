/*-------------------------------------------------------------------------
 *
 * mongo_fdw.c
 * 		Foreign-data wrapper for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2022, EnterpriseDB Corporation.
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
#if PG_VERSION_NUM >= 140000
#include "optimizer/appendinfo.h"
#endif
#if PG_VERSION_NUM >= 120000
#include "optimizer/optimizer.h"
#endif
#include "optimizer/tlist.h"
#if PG_VERSION_NUM < 120000
#include "optimizer/var.h"
#endif
#include "parser/parsetree.h"
#include "storage/ipc.h"
#include "utils/jsonb.h"
#if PG_VERSION_NUM < 130000
#include "utils/jsonapi.h"
#else
#include "utils/jsonfuncs.h"
#endif
#include "utils/rel.h"
#include "utils/syscache.h"

/* Declarations for dynamic loading */
PG_MODULE_MAGIC;

/*
 * In PG 9.5.1 the number will be 90501,
 * our version is 5.3.0 so number will be 50300
 */
#define CODE_VERSION   50300

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
#if PG_VERSION_NUM >= 110000
static void mongoBeginForeignInsert(ModifyTableState *mtstate,
									ResultRelInfo *resultRelInfo);
static void mongoEndForeignInsert(EState *estate,
								  ResultRelInfo *resultRelInfo);
#endif
#ifdef META_DRIVER
static void mongoGetForeignJoinPaths(PlannerInfo *root, RelOptInfo *joinrel,
									 RelOptInfo *outerrel,
									 RelOptInfo *innerrel,
									 JoinType jointype,
									 JoinPathExtraData *extra);
#endif

/*
 * Helper functions
 */
static double foreign_table_document_count(Oid foreignTableId);
static HTAB *column_mapping_hash(Oid foreignTableId, List *columnList,
								 List *colNameList, List *colIsInnerList,
								 bool isJoin);
static void fill_tuple_slot(const BSON *bsonDocument,
							const char *bsonDocumentKey,
							HTAB *columnMappingHash,
							Datum *columnValues,
							bool *columnNulls,
							bool isJoin);
static bool column_types_compatible(BSON_TYPE bsonType, Oid columnTypeId);
static Datum column_value_array(BSON_ITERATOR *bsonIterator, Oid valueTypeId);
static Datum column_value(BSON_ITERATOR *bsonIterator,
						  Oid columnTypeId,
						  int32 columnTypeMod);
static void mongo_free_scan_state(MongoFdwModifyState *fmstate);
static int mongo_acquire_sample_rows(Relation relation,
									 int errorLevel,
									 HeapTuple *sampleRows,
									 int targetRowCount,
									 double *totalRowCount,
									 double *totalDeadRowCount);
static void mongo_fdw_exit(int code, Datum arg);
#ifdef META_DRIVER
static bool mongo_foreign_join_ok(PlannerInfo *root, RelOptInfo *joinrel,
								  JoinType jointype, RelOptInfo *outerrel,
								  RelOptInfo *innerrel,
								  JoinPathExtraData *extra);
static void mongo_prepare_qual_info(List *quals, MongoJoinQualInfo *jqinfo);
#endif
#ifndef META_DRIVER
static const char *escape_json_string(const char *string);
static void bson_to_json_string(StringInfo output, BSON_ITERATOR iter,
								bool isArray);
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
#ifdef META_DRIVER
	/* Initialize MongoDB C driver */
	mongoc_init();
#endif

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

#if PG_VERSION_NUM >= 110000
	/* Partition routing and/or COPY from */
	fdwRoutine->BeginForeignInsert = mongoBeginForeignInsert;
	fdwRoutine->EndForeignInsert = mongoEndForeignInsert;
#endif

#ifdef META_DRIVER
	/* Support function for join push-down */
	fdwRoutine->GetForeignJoinPaths = mongoGetForeignJoinPaths;
#endif

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
#ifdef META_DRIVER
	/* Release all memory and other resources allocated by the driver */
	mongoc_cleanup();
#endif
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
	char 	   *relname;
	char       *database;
	char       *refname;

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

		if (IsA(ri->clause, OpExpr) &&
			mongo_is_foreign_expr(root, baserel, ri->clause, false))
			fpinfo->remote_conds = lappend(fpinfo->remote_conds, ri);
		else
			fpinfo->local_conds = lappend(fpinfo->local_conds, ri);
	}

	/* Base foreign tables need to be pushed down always. */
	fpinfo->pushdown_safe = true;

	/* Fetch options */
	options = mongo_get_options(foreigntableid);

	/*
	 * Retrieve exact document count for remote collection if asked, otherwise,
	 * use default estimate in planning.
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
	database =  options->svr_database;
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
	Cost		startupCost = 0.0;
	Cost		totalCost = 0.0;

	/* Fetch options */
	options = mongo_get_options(foreigntableid);

	/*
	 * Retrieve exact document count for remote collection if asked, otherwise,
	 * use default estimate in planning.
	 */
	if (options->use_remote_estimate)
	{
		double 		documentCount = foreign_table_document_count(foreigntableid);

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
			 * We estimate disk costs assuming a sequential scan over the data.
			 * This is an inaccurate assumption as Mongo scatters the data over
			 * disk pages, and may rely on an index to retrieve the data.
			 * Still, this should at least give us a relative cost.
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
			totalCpuCost = (cpuCostPerDoc * documentCount) +(cpuCostPerRow * inputRowCount);

			connectionCost = MONGO_CONNECTION_COST_MULTIPLIER * seq_page_cost;
			startupCost = baserel->baserestrictcost.startup + connectionCost;
			totalCost = startupCost + totalDiskAccessCost + totalCpuCost;
		}
		else
			ereport(DEBUG1,
					(errmsg("could not retrieve document count for collection"),
					 errhint("Falling back to default estimates in planning.")));
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
#ifdef META_DRIVER
	MongoJoinQualInfo *jqinfo;
#endif

	/* Set scan relation id */
	if (foreignrel->reloptkind == RELOPT_BASEREL ||
		foreignrel->reloptkind == RELOPT_OTHER_MEMBER_REL)
		scan_relid = foreignrel->relid;
	else
	{
		 /* Join relation - set scan_relid to 0. */
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
	 * Separate the restrictionClauses into those that can be executed remotely
	 * and those that can't.  baserestrictinfo clauses that were previously
	 * determined to be safe or unsafe are shown in fpinfo->remote_conds and
	 * fpinfo->local_conds.  Anything else in the restrictionClauses list will
	 * be a join clause, which we have to check for remote-safety.  Only the
	 * OpExpr clauses are sent to the remote server.
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
	scan_var_list = list_concat_unique(scan_var_list,
									   pull_var_clause((Node *) local_exprs,
													   PVC_RECURSE_PLACEHOLDERS));

	if (IS_JOIN_REL(foreignrel))
	{
		/*
		 * For join relations, the planner needs a targetlist, which represents
		 * the output of the ForeignScan node.
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
			ListCell   *lc;

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
				 * plan can be part of the joinquals as well.  (They might also
				 * be in the mergequals or hashquals, but we can't touch those
				 * without breaking the plan.)
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

	/* Form column list required for query execution from scan_var_list. */
	columnList = mongo_get_column_list(root, foreignrel, scan_var_list,
									   &column_name_list,
									   &is_inner_column_list);

#ifdef META_DRIVER
	/*
	 * Prepare separate lists of column names, varno, varattno, and whether it
	 * is part of the outer relation or not.  This information would be useful
	 * at the time of execution to prepare the MongoDB query.
	 */
	if (IS_JOIN_REL(foreignrel))
	{
		/*
		 * We use MongoJoinQualInfo to pass various information related to
		 * joining quals to fdw_private which is used to form equivalent
		 * MongoDB query during the execution phase.
		 */
		jqinfo = (MongoJoinQualInfo *) palloc(sizeof(MongoJoinQualInfo));

		jqinfo->root = root;
		jqinfo->foreignRel = foreignrel;
		jqinfo->outerRelids = fpinfo->outerrel->relids;
		jqinfo->joinExprColHash = NULL;
		/* Initialize all lists */
		jqinfo->colNameList = NIL;
		jqinfo->colNumList = NIL;
		jqinfo->rtiList = NIL;
		jqinfo->isOuterList = NIL;

		/*
		 * Extract required data of columns involved in join clauses and append
		 * it into the various lists required to pass it to the executor.
		 * Also, save join type in the list.
		 */
		if (fpinfo->joinclauses)
			mongo_prepare_qual_info(fpinfo->joinclauses, jqinfo);

		/*
		 * Extract required data of columns involved in the WHERE clause and
		 * append it into the various lists required to pass it to the
		 * executor.
		 */
		if (fpinfo->remote_conds)
			mongo_prepare_qual_info(fpinfo->remote_conds, jqinfo);

		/* Destroy hash table used to get unique column info */
		hash_destroy(jqinfo->joinExprColHash);
	}
#endif

	/*
	 * Build the fdw_private list that will be available to the executor.
	 * Items in the list must match enum mongoFdwScanPrivateIndex.
	 */
	fdw_private = list_make2(columnList, remote_exprs);

#ifdef META_DRIVER
	/*
	 * Unlike postgres_fdw, remote query formation is done in the execution
	 * state.  There is NO way to get the correct information required to form
	 * a remote query during the execution state.  So, we are gathering
	 * information required to form a MongoDB query in the planning state and
	 * passing it to the execution state through fdw_private.
	 */
	if (IS_JOIN_REL(foreignrel))
	{
		fdw_private = lappend(fdw_private,
							  makeString(fpinfo->relation_name->data));
		fdw_private = lappend(fdw_private, column_name_list);
		fdw_private = lappend(fdw_private, is_inner_column_list);
		fdw_private = lappend(fdw_private, fpinfo->joinclauses);
		fdw_private = lappend(fdw_private, jqinfo->colNameList);
		fdw_private = lappend(fdw_private, jqinfo->colNumList);
		fdw_private = lappend(fdw_private, jqinfo->rtiList);
		fdw_private = lappend(fdw_private, jqinfo->isOuterList);
		fdw_private = lappend(fdw_private,
							  list_make2(makeString(fpinfo->inner_relname),
										 makeString(fpinfo->outer_relname)));
		fdw_private = lappend(fdw_private, makeInteger(fpinfo->jointype));
	}
#endif

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
		rtindex = bms_next_member(fsplan->fs_relids, -1);
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
	List 	   *colNameList = NIL;
	List 	   *colIsInnerList = NIL;

	/* If Explain with no Analyze, do nothing */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	fmstate = (MongoFdwModifyState *) palloc0(sizeof(MongoFdwModifyState));

	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckRTEPerms() does. In the case of a join, use the lowest-numbered
	 * member RTE as a representative; we would get the same result from any.
	 */
	if (fsplan->scan.scanrelid > 0)
	{
		rtindex = fsplan->scan.scanrelid;
		fmstate->isJoinRel = false;
	}
	else
	{
		rtindex = bms_next_member(fsplan->fs_relids, -1);
		fmstate->isJoinRel = true;
	}
	rte = rt_fetch(rtindex, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	fmstate->rel = node->ss.ss_currentRelation;
	table = GetForeignTable(rte->relid);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	options = mongo_get_options(rte->relid);

	/*
	 * Get connection to the foreign server.  Connection manager will establish
	 * new connection if necessary.
	 */
	mongoConnection = mongo_get_connection(server, user, options);

	columnList = list_nth(fdw_private, mongoFdwPrivateColumnList);

	if (fmstate->isJoinRel == true)
	{
		colNameList = list_nth(fdw_private, mongoFdwPrivateColNameList);
		colIsInnerList = list_nth(fdw_private, mongoFdwPrivateColIsInnerList);
	}

	columnMappingHash = column_mapping_hash(rte->relid, columnList,
											colNameList, colIsInnerList,
											fmstate->isJoinRel);

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
		 * We construct the query document to have MongoDB filter its rows.  We
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
		if (fmstate->isJoinRel)
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
						columnValues, columnNulls, fmstate->isJoinRel);

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
#if PG_VERSION_NUM < 110000
			Form_pg_attribute attr = tupdesc->attrs[attnum - 1];
#else
			Form_pg_attribute attr = TupleDescAttr(tupdesc, attnum - 1);
#endif

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}
	else if (operation == CMD_UPDATE)
	{
		Bitmapset  *tmpset = bms_copy(rte->updatedCols);
		AttrNumber	col;

		while ((col = bms_first_member(tmpset)) >= 0)
		{
			col += FirstLowInvalidHeapAttributeNumber;
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

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case.  resultRelInfo->ri_FdwState
	 * stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	foreignTableId = RelationGetRelid(rel);
	userid = GetUserId();

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
#if PG_VERSION_NUM < 110000
		Form_pg_attribute attr = RelationGetDescr(rel)->attrs[attnum - 1];
#else
		Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(rel),
											   attnum - 1);
#endif

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
#if PG_VERSION_NUM < 110000
			if (strcmp(slot->tts_tupleDescriptor->attrs[0]->attname.data, "_id") != 0)
#else
			if (strcmp(TupleDescAttr(slot->tts_tupleDescriptor, 0)->attname.data, "_id") != 0)
#endif
				elog(ERROR, "first column of MongoDB's foreign table must be \"_id\"");

			if (typoid != NAMEOID)
				elog(ERROR, "type of first column of MongoDB's foreign table must be \"NAME\"");
#if PG_VERSION_NUM < 110000
			if (strcmp(slot->tts_tupleDescriptor->attrs[0]->attname.data, "__doc") == 0)
#else
			if (strcmp(TupleDescAttr(slot->tts_tupleDescriptor, 0)->attname.data, "__doc") == 0)
#endif
				continue;

			/*
			 * Ignore the value of first column which is row identifier in
			 * MongoDb (_id) and let MongoDB to insert the unique value for
			 * that column.
			 */
			if (attnum == 1)
				continue;

#if PG_VERSION_NUM < 110000
			append_mongo_value(bsonDoc,
							   slot->tts_tupleDescriptor->attrs[attnum - 1]->attname.data,
							   value,
							   isnull,
							   slot->tts_tupleDescriptor->attrs[attnum - 1]->atttypid);
#else
			append_mongo_value(bsonDoc,
							   TupleDescAttr(slot->tts_tupleDescriptor, attnum - 1)->attname.data,
							   value,
							   isnull,
							   TupleDescAttr(slot->tts_tupleDescriptor, attnum - 1)->atttypid);
#endif
		}
	}
	bsonFinish(bsonDoc);

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
#if PG_VERSION_NUM < 110000
	Form_pg_attribute attr = RelationGetDescr(target_relation)->attrs[0];
#else
	Form_pg_attribute attr = TupleDescAttr(RelationGetDescr(target_relation),
										   0);
#endif

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

#if PG_VERSION_NUM < 110000
	columnName = get_relid_attribute_name(foreignTableId, 1);
#else
	columnName = get_attname(foreignTableId, 1, false);
#endif

	typoid = get_atttype(foreignTableId, 1);

	document = bsonCreate();
	bsonAppendStartObject(document, "$set", &set);

	/* Get following parameters from slot */
	if (slot != NULL && fmstate->target_attrs != NIL)
	{
		ListCell   *lc;

		foreach(lc, fmstate->target_attrs)
		{
			int			attnum = lfirst_int(lc);
#if PG_VERSION_NUM < 110000
			Form_pg_attribute attr = slot->tts_tupleDescriptor->attrs[attnum - 1];
#else
			Form_pg_attribute attr = TupleDescAttr(slot->tts_tupleDescriptor,
												   attnum - 1);
#endif
			Datum		value;
			bool		isnull;

			if (strcmp("_id", attr->attname.data) == 0)
				continue;

			if (strcmp("__doc", attr->attname.data) == 0)
				elog(ERROR, "system column '__doc' update is not supported");

			value = slot_getattr(slot, attnum, &isnull);
#ifdef META_DRIVER
			append_mongo_value(&set, attr->attname.data, value,
							   isnull ? true : false, attr->atttypid);
#else
			append_mongo_value(document, attr->attname.data, value,
							   isnull ? true : false, attr->atttypid);
#endif
		}
	}
	bsonAppendFinishObject(document, &set);
	bsonFinish(document);

	op = bsonCreate();
	if (!append_mongo_value(op, columnName, datum, false, typoid))
	{
		bsonDestroy(document);
		return NULL;
	}
	bsonFinish(op);

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

#if PG_VERSION_NUM < 110000
	columnName = get_relid_attribute_name(foreignTableId, 1);
#else
	columnName = get_attname(foreignTableId, 1, false);
#endif

	typoid = get_atttype(foreignTableId, 1);

	document = bsonCreate();
	if (!append_mongo_value(document, columnName, datum, false, typoid))
	{
		bsonDestroy(document);
		return NULL;
	}
	bsonFinish(document);

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
	double 		documentCount;
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
					List *colIsInnerList, bool isJoin)
{
	ListCell   *columnCell;
	HTAB	   *columnMappingHash;
	HASHCTL		hashInfo;
	uint32		attnum = 0;
	Index      	listIndex = 0;

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

		if (isJoin)
		{
			int			is_innerrel = list_nth_int(colIsInnerList, listIndex);

			columnName = strVal(list_nth(colNameList, listIndex++));

			/*
			 * In MongoDB, columns involved in join result-set from inner table
			 * prefixed with Join result name.  Uses hard-coded string
			 * "Join Result" to be prefixed to form the hash key to read the
			 * joined result set.  This same prefix needs to be given as joined
			 * result set name in the $lookup stage when building the remote
			 * query.
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
		else
		{
#if PG_VERSION_NUM < 110000
			columnName = get_relid_attribute_name(foreignTableId,
												  column->varattno);
#else
			columnName = get_attname(foreignTableId, column->varattno, false);
#endif
			hashKey = (void *) columnName;
		}

		columnMapping = (ColumnMapping *) hash_search(columnMappingHash,
													  hashKey,
													  HASH_ENTER,
													  &handleFound);
		Assert(columnMapping != NULL);

		/*
		 * Save attribute number of the current column in the resulting tuple.
		 * For join relation, it is continuously increasing integers starting
		 * from 0, and for simple relation, it's varattno.
		 */
		if(isJoin)
		{
			columnMapping->columnIndex = attnum;
			attnum++;
		}
		else
			columnMapping->columnIndex = column->varattno - 1;

		/* Save other information */
		columnMapping->columnTypeId = column->vartype;
		columnMapping->columnTypeMod = column->vartypmod;
		columnMapping->columnArrayTypeId = get_element_type(column->vartype);
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
				bool *columnNulls, bool isJoin)
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
		bool		handleFound = false;
		const char *bsonFullKey;
		void	   *hashKey;
		int32		attnum;

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
								columnValues, columnNulls, isJoin);
				continue;
			}
		}

		/* If no corresponding column or null BSON value, continue */
		if (columnMapping == NULL || bsonType == BSON_TYPE_NULL)
			continue;

		/* Check if columns have compatible types */
		if (OidIsValid(columnArrayTypeId) && bsonType == BSON_TYPE_ARRAY)
			compatibleTypes = true;
		else
			compatibleTypes = column_types_compatible(bsonType, columnTypeId);

		/* If types are incompatible, leave this column null */
		if (!compatibleTypes)
			continue;

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
#ifdef META_DRIVER
			if (bsonType == BSON_TYPE_BOOL)
				compatibleTypes = true;
#endif
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
#ifdef META_DRIVER
			if (bsonType == BSON_TYPE_OID)
				compatibleTypes = true;
#endif
			break;
		case NAMEOID:

			/*
			 * We currently overload the NAMEOID type to represent the BSON
			 * object identifier.  We can safely overload this 64-byte data
			 * type since it's reserved for internal use in PostgreSQL.
			 */
			if (bsonType == BSON_TYPE_OID ||
				bsonType == BSON_TYPE_UTF8)
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
				const char *name_val;
				BSON_TYPE	bsonType = bsonIterType(bsonIterator);
				Datum		valueDatum = 0;

				switch (bsonType)
				{
					case BSON_TYPE_OID:
						{
							char		value[NAMEDATALEN];
							bson_oid_t *bsonObjectId = (bson_oid_t *) bsonIterOid(bsonIterator);

							bson_oid_to_string(bsonObjectId, value);
							name_val = value;
						}
						break;
					case BSON_TYPE_UTF8:
						{
							name_val = bsonIterString(bsonIterator);
						}
						break;
					default:
						ereport(ERROR,
							(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							errmsg("cannot convert BSON type to column type"),
							errhint("Column type: %u", (uint32) columnTypeId)));
				}
				valueDatum = CStringGetDatum(name_val);
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
#ifdef META_DRIVER
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
#else
				value_len = bsonIterBinLen(bsonIterator);
				value = (char *) bsonIterBinData(bsonIterator);
#endif
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

#ifdef META_DRIVER
				/* Convert BSON to JSON value */
				bsonToJsonStringValue(buffer, bsonIterator,
									  BSON_TYPE_ARRAY == type);
#else
				/* Convert BSON to JSON value */
				bson_to_json_string(buffer, *bsonIterator,
									BSON_TYPE_ARRAY == type);
#endif
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

#ifndef META_DRIVER
static void
bson_to_json_string(StringInfo output, BSON_ITERATOR i, bool isArray)
{
	const char *key;
	bool		isFirstElement;
	char		beginSymbol = '{';
	char		endSymbol = '}';
	BSON_TYPE	bsonType;

	if (isArray)
	{
		beginSymbol = '[';
		endSymbol = ']';
	}

	appendStringInfoChar(output, beginSymbol);

	isFirstElement = true;
	while (bsonIterNext(&i))
	{
		if (!isFirstElement)
			appendStringInfoChar(output, ',');

		bsonType = bsonIterType(&i);
		if (bsonType == 0)
			break;

		key = bsonIterKey(&i);

		if (!isArray)
			appendStringInfo(output, "\"%s\":", key);

		switch (bsonType)
		{
			case BSON_TYPE_DOUBLE:
				appendStringInfo(output, "%f", bsonIterDouble(&i));
				break;
			case BSON_TYPE_UTF8:
				appendStringInfo(output, "\"%s\"",
								 escape_json_string(bsonIterString(&i)));
				break;
			case BSON_TYPE_SYMBOL:
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
						 errmsg("\"symbol\" BSON type is deprecated and unsupported"),
						 errhint("Symbol: %s", bsonIterString(&i))));
				break;
			case BSON_TYPE_OID:
				{
					char		oidhex[25];

					bsonOidToString(bsonIterOid(&i), oidhex);
					appendStringInfo(output, "{\"$oid\":\"%s\"}", oidhex);
					break;
				}
			case BSON_TYPE_BOOL:
				appendStringInfoString(output,
									   bsonIterBool(&i) ? "true" : "false");
				break;
			case BSON_TYPE_DATE_TIME:
				appendStringInfo(output, "{\"$date\":%ld}",
								 (long int) bsonIterDate(&i));
				break;
			case BSON_TYPE_BINDATA:
				/* It's possible to encode the data with base64 here. */
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
						 errmsg("support for \"binary data\" BSON type is not implemented")));
				break;
			case BSON_TYPE_UNDEFINED:
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
						 errmsg("\"undefined\" BSON type is deprecated and unsupported")));
				break;
			case BSON_TYPE_NULL:
				appendStringInfoString(output, "null");
				break;
			case BSON_TYPE_REGEX:
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
						 errmsg("support for \"regex\" BSON type is not implemented"),
						 errhint("Regex: %s", bsonIterRegex(&i))));
				break;
			case BSON_TYPE_CODE:
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
						 errmsg("support for \"code\" BSON type is not implemented"),
						 errhint("Code: %s", bsonIterCode(&i))));
				break;
			case BSON_TYPE_CODEWSCOPE:
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
						 errmsg("support for \"code\" with scope` BSON type is not implemented")));
				break;
			case BSON_TYPE_INT32:
				appendStringInfo(output, "%d", bsonIterInt32(&i));
				break;
			case BSON_TYPE_INT64:
				appendStringInfo(output, "%lu", (uint64_t) bsonIterInt64(&i));
				break;
			case BSON_TYPE_TIMESTAMP:
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
						 errmsg("internal `timestamp` BSON type is and unsupported")));
				break;
			case BSON_TYPE_DOCUMENT:
				bson_to_json_string(output, i, false);
				break;
			case BSON_TYPE_ARRAY:
				bson_to_json_string(output, i, true);
				break;
			default:
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
						 errmsg("unsupported BSON type: %x", bsonType)));
		}
		isFirstElement = false;
	}
	appendStringInfoChar(output, endSymbol);
}

/*
 * escape_json_string
 *		Escapes a string for safe inclusion in JSON.
 */
static const char *
escape_json_string(const char *string)
{
	StringInfo	buffer;
	const char *ptr;
	int			i;
	int			segmentStartIdx;
	int			len;
	bool		needsEscaping = false;

	for (ptr = string; *ptr; ++ptr)
	{
		if (*ptr == '"' || *ptr == '\r' || *ptr == '\n' || *ptr == '\t' ||
			*ptr == '\\')
		{
			needsEscaping = true;
			break;
		}
	}

	if (!needsEscaping)
		return string;

	buffer = makeStringInfo();
	len = strlen(string);
	segmentStartIdx = 0;
	for (i = 0; i < len; ++i)
	{
		if (string[i] == '"' || string[i] == '\r' || string[i] == '\n' ||
			string[i] == '\t' || string[i] == '\\')
		{
			if (segmentStartIdx < i)
				appendBinaryStringInfo(buffer, string + segmentStartIdx,
									   i - segmentStartIdx);

			appendStringInfoChar(buffer, '\\');
			if (string[i] == '"')
				appendStringInfoChar(buffer, '"');
			else if (string[i] == '\r')
				appendStringInfoChar(buffer, 'r');
			else if (string[i] == '\n')
				appendStringInfoChar(buffer, 'n');
			else if (string[i] == '\t')
				appendStringInfoChar(buffer, 't');
			else if (string[i] == '\\')
				appendStringInfoChar(buffer, '\\');

			segmentStartIdx = i + 1;
		}
	}
	if (segmentStartIdx < len)
		appendBinaryStringInfo(buffer, string + segmentStartIdx,
							   len - segmentStartIdx);
	return buffer->data;
}
#endif

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
	double 		documentCount;
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
#if PG_VERSION_NUM >= 110000
		Form_pg_attribute attr = TupleDescAttr(tupleDescriptor, columnId - 1);

		column->varattno = columnId;
		column->vartype = attr->atttypid;
		column->vartypmod = attr->atttypmod;
#else
		/* Only assign required fields for column mapping hash */
		column->varattno = columnId;
		column->vartype = tupleDescriptor->attrs[columnId - 1]->atttypid;
		column->vartypmod = tupleDescriptor->attrs[columnId - 1]->atttypmod;
#endif

		columnList = lappend(columnList, column);
	}

	foreignTableId = RelationGetRelid(relation);
	table = GetForeignTable(foreignTableId);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(GetUserId(), server->serverid);
	options = mongo_get_options(foreignTableId);

	/*
	 * Get connection to the foreign server.  Connection manager will establish
	 * new connection if necessary.
	 */
	mongoConnection = mongo_get_connection(server, user, options);

	if (!bsonFinish(queryDocument))
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

	/* Create cursor for collection name and set query */
	mongoCursor = mongoCursorCreate(mongoConnection, options->svr_database,
									options->collectionName, queryDocument);
	columnMappingHash = column_mapping_hash(foreignTableId, columnList, NIL,
											NIL, false);

	/*
	 * Use per-tuple memory context to prevent leak of memory used to read
	 * rows from the file with copy routines.
	 */
#if PG_VERSION_NUM < 110000
	tupleContext = AllocSetContextCreate(CurrentMemoryContext,
										 "mongo_fdw temporary context",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);
#else
	tupleContext = AllocSetContextCreate(CurrentMemoryContext,
										 "mongo_fdw temporary context",
										 ALLOCSET_DEFAULT_SIZES);
#endif

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
							columnValues, columnNulls, false);

			MemoryContextSwitchTo(oldContext);
		}
		else
		{
#ifdef META_DRIVER
			bson_error_t error;

			if (mongoc_cursor_error(mongoCursor, &error))
				ereport(ERROR,
						(errmsg("could not iterate over mongo collection"),
						 errhint("Mongo driver error: %s", error.message)));
#else
			mongo_cursor_error_t errorCode = mongoCursor->err;

			if (errorCode != MONGO_CURSOR_EXHAUSTED)
				ereport(ERROR,
						(errmsg("could not iterate over mongo collection"),
						 errhint("Mongo driver cursor error code: %d",
								 errorCode)));
#endif
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

#if PG_VERSION_NUM >= 110000
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
#endif

#ifdef META_DRIVER
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
	Path	   *epq_path = NULL; /* Path to create plan to be executed when
								  * EvalPlanQual gets triggered. */

	/*
	 * Skip if this join combination has been considered already.
	 */
	if (joinrel->fdw_private)
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
									   NIL,		/* no pathkeys */
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

	/* XXX Consider pathkeys for the join relation */

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

	/* If join pushdown is not enabled, honor it. */
	if ((!IS_JOIN_REL(outerrel) && !fpinfo_o->options->enable_join_pushdown) ||
		(!IS_JOIN_REL(innerrel) && !fpinfo_i->options->enable_join_pushdown))
		return false;

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
	 * required to be applied before joining the relations.  Hence the join can
	 * not be pushed down.
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
#if PG_VERSION_NUM >= 110000
		colname = get_attname(rte->relid, var->varattno, false);
#else
		colname = get_relid_attribute_name(rte->relid, var->varattno);
#endif
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
															 true);

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
				 * clauses separate to avoid INNER JOIN not getting pushdown if
				 * any of the WHERE clauses are not shippable as per join
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
 *		Gather information of columns involved in the join quals by extracting
 *		clause from each qual and process it further using mongo_check_qual().
 */
static void
mongo_prepare_qual_info(List *quals, MongoJoinQualInfo *jqinfo)
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

		mongo_check_qual(expr, jqinfo);
	}
}
#endif /* End of META_DRIVER */
