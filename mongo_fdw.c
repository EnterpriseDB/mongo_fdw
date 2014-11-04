/*-------------------------------------------------------------------------
 *
 * mongo_fdw.c
 * 		Foreign-data wrapper for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2004-2014, EnterpriseDB Corporation.
 *
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		mongo_fdw.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "bson.h"
#include "mongo_wrapper.h"
#include "mongo_fdw.h"
#include "mongo_query.h"

#include "access/reloptions.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "nodes/makefuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/plancat.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "storage/ipc.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/memutils.h"
#include "access/sysattr.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/vacuum.h"
#include "foreign/fdwapi.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/cost.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planmain.h"
#include "optimizer/prep.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/jsonapi.h"
#if PG_VERSION_NUM >= 90300
	#include "access/htup_details.h"
#endif


/* Local functions forward declarations */
static void MongoGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel,
						Oid foreignTableId);
static void MongoGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel,
						Oid foreignTableId);
static ForeignScan * MongoGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel,
						Oid foreignTableId, ForeignPath *bestPath,
						List *targetList, List *restrictionClauses);
static void MongoExplainForeignScan(ForeignScanState *scanState,
						ExplainState *explainState);
static void MongoBeginForeignScan(ForeignScanState *scanState, int executorFlags);
static TupleTableSlot * MongoIterateForeignScan(ForeignScanState *scanState);
static void MongoEndForeignScan(ForeignScanState *scanState);
static void MongoReScanForeignScan(ForeignScanState *scanState);

static TupleTableSlot *MongoExecForeignUpdate(EState *estate,
						ResultRelInfo *resultRelInfo,
						TupleTableSlot *slot,
						TupleTableSlot *planSlot);
static TupleTableSlot *MongoExecForeignDelete(EState *estate,
						ResultRelInfo *resultRelInfo,
						TupleTableSlot *slot,
						TupleTableSlot *planSlot);
static void MongoEndForeignModify(EState *estate,
						ResultRelInfo *resultRelInfo);

static void MongoAddForeignUpdateTargets(Query *parsetree,
						RangeTblEntry *target_rte,
						Relation target_relation);

static void MongoBeginForeignModify(ModifyTableState *mtstate,
						ResultRelInfo *resultRelInfo,
						List *fdw_private,
						int subplan_index,
						int eflags);

static TupleTableSlot *MongoExecForeignInsert(EState *estate,
						ResultRelInfo *resultRelInfo,
						TupleTableSlot *slot,
						TupleTableSlot *planSlot);

static List *MongoPlanForeignModify(PlannerInfo *root,
						ModifyTable *plan,
						Index resultRelation,
						int subplan_index);

static void
MongoExplainForeignModify(ModifyTableState *mtstate,
						ResultRelInfo *rinfo, List *fdw_private,
						int subplan_index, ExplainState *es);

/* local functions */
static double ForeignTableDocumentCount(Oid foreignTableId);
static HTAB * ColumnMappingHash(Oid foreignTableId, List *columnList);
static void FillTupleSlot(const BSON *bsonDocument, const char *bsonDocumentKey,
						HTAB *columnMappingHash, Datum *columnValues,
						bool *columnNulls);
static bool ColumnTypesCompatible(BSON_TYPE bsonType, Oid columnTypeId);
static Datum ColumnValueArray(BSON_ITERATOR *bsonIterator, Oid valueTypeId);
static Datum ColumnValue(BSON_ITERATOR *bsonIterator, Oid columnTypeId,
						 int32 columnTypeMod);
static void MongoFreeScanState(MongoFdwModifyState *fmstate);
static bool MongoAnalyzeForeignTable(Relation relation,
						AcquireSampleRowsFunc *acquireSampleRowsFunc,
						BlockNumber *totalPageCount);
static int MongoAcquireSampleRows(Relation relation, int errorLevel,
						HeapTuple *sampleRows, int targetRowCount,
						double *totalRowCount, double *totalDeadRowCount);
static void mongo_fdw_exit(int code, Datum arg);

extern PGDLLEXPORT void _PG_init(void);

const char * EscapeJsonString(const char *string);
void DumpJson(StringInfo buffer, const char *bsonData, bool isArray);

/* the null action object used for pure validation */
static JsonSemAction nullSemAction =
{
	NULL, NULL, NULL, NULL, NULL,
	NULL, NULL, NULL, NULL, NULL
};

/* declarations for dynamic loading */
PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(mongo_fdw_handler);

/*
 * Library load-time initalization, sets on_proc_exit() callback for
 * backend shutdown.
 */
void
_PG_init(void)
{
	on_proc_exit(&mongo_fdw_exit, PointerGetDatum(NULL));
}

/*
 * mongo_fdw_handler creates and returns a struct with pointers to foreign table
 * callback functions.
 */
Datum
mongo_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *fdwRoutine = makeNode(FdwRoutine);
	fdwRoutine->GetForeignRelSize = MongoGetForeignRelSize;
	fdwRoutine->GetForeignPaths = MongoGetForeignPaths;
	fdwRoutine->GetForeignPlan = MongoGetForeignPlan;
	fdwRoutine->BeginForeignScan = MongoBeginForeignScan;
	fdwRoutine->IterateForeignScan = MongoIterateForeignScan;
	fdwRoutine->ReScanForeignScan = MongoReScanForeignScan;
	fdwRoutine->EndForeignScan = MongoEndForeignScan;
	fdwRoutine->AnalyzeForeignTable = MongoAnalyzeForeignTable;

	/* support for insert / update / delete */
	fdwRoutine->ExecForeignInsert = MongoExecForeignInsert;
	fdwRoutine->BeginForeignModify = MongoBeginForeignModify;
	fdwRoutine->PlanForeignModify = MongoPlanForeignModify;
	fdwRoutine->AddForeignUpdateTargets = MongoAddForeignUpdateTargets;
	fdwRoutine->ExecForeignUpdate = MongoExecForeignUpdate;
	fdwRoutine->ExecForeignDelete = MongoExecForeignDelete;
	fdwRoutine->EndForeignModify = MongoEndForeignModify;

	/* support for EXPLAIN */
	fdwRoutine->ExplainForeignScan = MongoExplainForeignScan;
	fdwRoutine->ExplainForeignModify = MongoExplainForeignModify;

	/* support for ANALYSE */
	fdwRoutine->AnalyzeForeignTable = MongoAnalyzeForeignTable;

	PG_RETURN_POINTER(fdwRoutine);
}

/*
 * Exit callback function.
 */
static void
mongo_fdw_exit(int code, Datum arg)
{
	mongo_cleanup_connection();
}


/*
 * MongoGetForeignRelSize obtains relation size estimates for mongo foreign table.
 */
static void
MongoGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel, Oid foreignTableId)
{
	double documentCount = ForeignTableDocumentCount(foreignTableId);
	if (documentCount > 0.0)
	{
		/*
		 * We estimate the number of rows returned after restriction qualifiers
		 * are applied. This will be more accurate if analyze is run on this
		 * relation.
		 */
		List *rowClauseList = baserel->baserestrictinfo;
		double rowSelectivity = clauselist_selectivity(root, rowClauseList,
													   0, JOIN_INNER, NULL);

		double outputRowCount = clamp_row_est(documentCount * rowSelectivity);
		baserel->rows = outputRowCount;
	}
	else
	{
		ereport(DEBUG1, (errmsg("could not retrieve document count for collection"),
						 errhint("Falling back to default estimates in planning")));
	}
}


/*
 * MongoGetForeignPaths creates the only scan path used to execute the query.
 * Note that MongoDB may decide to use an underlying index for this scan, but
 * that decision isn't deterministic or visible to us. We therefore create a
 * single table scan path.
 */
static void
MongoGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel, Oid foreignTableId)
{
	double           tupleFilterCost = baserel->baserestrictcost.per_tuple;
	double           inputRowCount = 0.0;
	double           documentSelectivity = 0.0;
	double           foreignTableSize = 0;
	int32            documentWidth = 0;
	BlockNumber      pageCount = 0;
	double           totalDiskAccessCost = 0.0;
	double           cpuCostPerDoc = 0.0;
	double           cpuCostPerRow = 0.0;
	double           totalCpuCost = 0.0;
	double           connectionCost = 0.0;
	double           documentCount = 0.0;
	List             *opExpressionList = NIL;
	Cost             startupCost = 0.0;
	Cost             totalCost = 0.0;
	Path             *foreignPath = NULL;

	documentCount = ForeignTableDocumentCount(foreignTableId);
	if (documentCount > 0.0)
	{
		/*
		 * We estimate the number of rows returned after restriction qualifiers
		 * are applied by MongoDB.
		 */
		opExpressionList = ApplicableOpExpressionList(baserel);
		documentSelectivity = clauselist_selectivity(root, opExpressionList,
													 0, JOIN_INNER, NULL);
		inputRowCount = clamp_row_est(documentCount * documentSelectivity);
		
		/*
		 * We estimate disk costs assuming a sequential scan over the data. This is
		 * an inaccurate assumption as Mongo scatters the data over disk pages, and
		 * may rely on an index to retrieve the data. Still, this should at least
		 * give us a relative cost.
		 */
		documentWidth = get_relation_data_width(foreignTableId, baserel->attr_widths);
		foreignTableSize = documentCount * documentWidth;

		pageCount = (BlockNumber) rint(foreignTableSize / BLCKSZ);
		totalDiskAccessCost = seq_page_cost * pageCount;

		/*
		 * The cost of processing a document returned by Mongo (input row) is 5x the
		 * cost of processing a regular row.
		 */
		cpuCostPerDoc = cpu_tuple_cost;
		cpuCostPerRow = (cpu_tuple_cost * MONGO_TUPLE_COST_MULTIPLIER) + tupleFilterCost;
		totalCpuCost = (cpuCostPerDoc * documentCount) + (cpuCostPerRow * inputRowCount);

		connectionCost = MONGO_CONNECTION_COST_MULTIPLIER * seq_page_cost;
		startupCost = baserel->baserestrictcost.startup + connectionCost;
		totalCost = startupCost + totalDiskAccessCost + totalCpuCost;
	}
	else
	{
		ereport(DEBUG1, (errmsg("could not retrieve document count for collection"),
						 errhint("Falling back to default estimates in planning")));
	}

	/* create a foreign path node */
	foreignPath = (Path *) create_foreignscan_path(root, baserel, baserel->rows,
												   startupCost, totalCost,
												   NIL,	 /* no pathkeys */
												   NULL, /* no outer rel either */
												   NIL); /* no fdw_private data */

	/* add foreign path as the only possible path */
	add_path(baserel, foreignPath);	
}


/*
 * MongoGetForeignPlan creates a foreign scan plan node for scanning the MongoDB
 * collection. Note that MongoDB may decide to use an underlying index for this
 * scan, but that decision isn't deterministic or visible to us.
 */
static ForeignScan *
MongoGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel,	Oid foreignTableId,
					ForeignPath *bestPath, List *targetList, List *restrictionClauses)
{
	Index                scanRangeTableIndex = baserel->relid;
	ForeignScan          *foreignScan = NULL;
	List                 *foreignPrivateList = NIL;
	List                 *opExpressionList = NIL;
	BSON                 *queryDocument = NULL;
	List                 *columnList = NIL;

	/*
	 * We push down applicable restriction clauses to MongoDB, but for simplicity
	 * we currently put all the restrictionClauses into the plan node's qual
	 * list for the executor to re-check. So all we have to do here is strip
	 * RestrictInfo nodes from the clauses and ignore pseudoconstants (which
	 * will be handled elsewhere).
	 */
	restrictionClauses = extract_actual_clauses(restrictionClauses, false);

	/*
	 * We construct the query document to have MongoDB filter its rows. We could
	 * also construct a column name document here to retrieve only the needed
	 * columns. However, we found this optimization to degrade performance on
	 * the MongoDB server-side, so we instead filter out columns on our side.
	 */
	opExpressionList = ApplicableOpExpressionList(baserel);
	queryDocument = QueryDocument(foreignTableId, opExpressionList);

	/* we don't need to serialize column list as lists are copiable */
	columnList = ColumnList(baserel);

	/* construct foreign plan with query document and column list */
	foreignPrivateList = list_make2(columnList, opExpressionList);

	/* only clean up the query struct */
	BsonDestroy(queryDocument);

	/* create the foreign scan node */
	foreignScan =  make_foreignscan(targetList, restrictionClauses,
									scanRangeTableIndex,
									NIL, /* no expressions to evaluate */
									foreignPrivateList);

	return foreignScan;
}


/*
 * MongoExplainForeignScan produces extra output for the Explain command.
 */
static void
MongoExplainForeignScan(ForeignScanState *scanState, ExplainState *explainState)
{
	MongoFdwOptions        *options = NULL;
	StringInfo             namespaceName = NULL;
	Oid                    foreignTableId = InvalidOid;

	foreignTableId = RelationGetRelid(scanState->ss.ss_currentRelation);
	options = mongo_get_options(foreignTableId);

	/* construct fully qualified collection name */
	namespaceName = makeStringInfo();
	appendStringInfo(namespaceName, "%s.%s", options->svr_database,
					 options->collectionName);

	mongo_free_options(options);

	ExplainPropertyText("Foreign Namespace", namespaceName->data, explainState);
}

static void
MongoExplainForeignModify(ModifyTableState *mtstate,
							ResultRelInfo *rinfo,
							List *fdw_private,
							int subplan_index,
							ExplainState *es)
{
	MongoFdwOptions       *options = NULL;
	StringInfo            namespaceName = NULL;
	Oid                   foreignTableId = InvalidOid;

	foreignTableId = RelationGetRelid(rinfo->ri_RelationDesc);
	options = mongo_get_options(foreignTableId);

	/* construct fully qualified collection name */
	namespaceName = makeStringInfo();
	appendStringInfo(namespaceName, "%s.%s", options->svr_database,
					 options->collectionName);

	mongo_free_options(options);
	ExplainPropertyText("Foreign Namespace", namespaceName->data, es);
}


/*
 * MongoBeginForeignScan connects to the MongoDB server, and opens a cursor that
 * uses the database name, collection name, and the remote query to send to the
 * server. The function also creates a hash table that maps referenced column
 * names to column index and type information.
 */
static void
MongoBeginForeignScan(ForeignScanState *scanState, int executorFlags)
{
	MONGO_CONN               *mongoConnection = NULL;
	MONGO_CURSOR             *mongoCursor = NULL;
	Oid                      foreignTableId = InvalidOid;
	List                     *columnList = NIL;
	HTAB                     *columnMappingHash = NULL;
	ForeignScan              *foreignScan = NULL;
	List                     *foreignPrivateList = NIL;
	BSON                     *queryDocument = NULL;
	MongoFdwOptions          *options = NULL;
	MongoFdwModifyState      *fmstate = NULL;
	List                     *opExpressionList = NIL;
	RangeTblEntry            *rte;
	EState                   *estate = scanState->ss.ps.state;
	ForeignScan              *fsplan = (ForeignScan *) scanState->ss.ps.plan;
	Oid                      userid;
	ForeignServer            *server;
	UserMapping              *user;
	ForeignTable             *table;


	/* if Explain with no Analyze, do nothing */
	if (executorFlags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	foreignTableId = RelationGetRelid(scanState->ss.ss_currentRelation);
	options = mongo_get_options(foreignTableId);

	fmstate = (MongoFdwModifyState *) palloc0(sizeof(MongoFdwModifyState));
	/*
	 * Identify which user to do the remote access as.  This should match what
	 * ExecCheckRTEPerms() does.
	 */
	rte = rt_fetch(fsplan->scan.scanrelid, estate->es_range_table);
	userid = rte->checkAsUser ? rte->checkAsUser : GetUserId();

	/* Get info about foreign table. */
	fmstate->rel = scanState->ss.ss_currentRelation;
	table = GetForeignTable(RelationGetRelid(fmstate->rel));
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/*
	 * Get connection to the foreign server. Connection manager will
	 * establish new connection if necessary.
	 */
	mongoConnection = mongo_get_connection(server, user, options);

	foreignScan = (ForeignScan *) scanState->ss.ps.plan;
	foreignPrivateList = foreignScan->fdw_private;
	Assert(list_length(foreignPrivateList) == 2);

	columnList = list_nth(foreignPrivateList, 0);
	opExpressionList = list_nth(foreignPrivateList, 1);

	queryDocument = QueryDocument(foreignTableId, opExpressionList);

	columnMappingHash = ColumnMappingHash(foreignTableId, columnList);

	/* create cursor for collection name and set query */
	mongoCursor = MongoCursorCreate(mongoConnection, options->svr_database, options->collectionName, queryDocument);

	/* create and set foreign execution state */
	fmstate->columnMappingHash = columnMappingHash;
	fmstate->mongoConnection = mongoConnection;
	fmstate->mongoCursor = mongoCursor;
	fmstate->queryDocument = queryDocument;
	fmstate->options = options;

	scanState->fdw_state = (void *) fmstate;
}


/*
 * MongoIterateForeignScan reads the next document from MongoDB, converts it to
 * a PostgreSQL tuple, and stores the converted tuple into the ScanTupleSlot as
 * a virtual tuple.
 */
static TupleTableSlot *
MongoIterateForeignScan(ForeignScanState *scanState)
{
	MongoFdwModifyState *fmstate = (MongoFdwModifyState *) scanState->fdw_state;
	TupleTableSlot      *tupleSlot = scanState->ss.ss_ScanTupleSlot;
	MONGO_CURSOR        *mongoCursor = fmstate->mongoCursor;
	HTAB                *columnMappingHash = fmstate->columnMappingHash;
	TupleDesc           tupleDescriptor = tupleSlot->tts_tupleDescriptor;
	Datum               *columnValues = tupleSlot->tts_values;
	bool                *columnNulls = tupleSlot->tts_isnull;
	int32               columnCount = tupleDescriptor->natts;

	/*
	 * We execute the protocol to load a virtual tuple into a slot. We first
	 * call ExecClearTuple, then fill in values / isnull arrays, and last call
	 * ExecStoreVirtualTuple. If we are done fetching documents from Mongo, we
	 * just return an empty slot as required.
	 */
	ExecClearTuple(tupleSlot);

	/* initialize all values for this row to null */
	memset(columnValues, 0, columnCount * sizeof(Datum));
	memset(columnNulls, true, columnCount * sizeof(bool));

	if (MongoCursorNext(mongoCursor, NULL))
	{
		const BSON *bsonDocument = MongoCursorBson(mongoCursor);
		const char *bsonDocumentKey = NULL; /* top level document */

		FillTupleSlot(bsonDocument, bsonDocumentKey,
					  columnMappingHash, columnValues, columnNulls);

		ExecStoreVirtualTuple(tupleSlot);
	}
	else
	{
		#ifdef META_DRIVER
		bson_error_t error;
		if (mongoc_cursor_error (mongoCursor, &error))
		{
			MongoFreeScanState(fmstate);
			ereport(ERROR, (errmsg("could not iterate over mongo collection"),
					errhint("Mongo driver error: %s", error.message)));
		}
		#else
		mongo_cursor_error_t errorCode = mongoCursor->err;
		if (errorCode != MONGO_CURSOR_EXHAUSTED)
		{
			MongoFreeScanState(fmstate);
			ereport(ERROR, (errmsg("could not iterate over mongo collection"),
									errhint("Mongo driver cursor error code: %d", errorCode)));
		}
		#endif
	}

	return tupleSlot;
}


/*
 * MongoEndForeignScan finishes scanning the foreign table, closes the cursor
 * and the connection to MongoDB, and reclaims scan related resources.
 */
static void
MongoEndForeignScan(ForeignScanState *scanState)
{
	MongoFdwModifyState *fmstate = (MongoFdwModifyState *) scanState->fdw_state;

	/* if we executed a query, reclaim mongo related resources */
	if (fmstate != NULL)
	{
		if (fmstate->options)
		{
			mongo_free_options(fmstate->options);
			fmstate->options = NULL;
		}
		MongoFreeScanState(fmstate);
	}
}


/*
 * MongoReScanForeignScan rescans the foreign table. Note that rescans in Mongo
 * end up being notably more expensive than what the planner expects them to be,
 * since MongoDB cursors don't provide reset/rewind functionality.
 */
static void
MongoReScanForeignScan(ForeignScanState *scanState)
{
	MongoFdwModifyState      *fmstate = (MongoFdwModifyState *) scanState->fdw_state;
	MONGO_CONN               *mongoConnection = fmstate->mongoConnection;
	MongoFdwOptions          *options = NULL;
	Oid                      foreignTableId = InvalidOid;

	/* close down the old cursor */
	MongoCursorDestroy(fmstate->mongoCursor);

	/* reconstruct full collection name */
	foreignTableId = RelationGetRelid(scanState->ss.ss_currentRelation);
	options = mongo_get_options(foreignTableId);

	/* reconstruct cursor for collection name and set query */
	fmstate->mongoCursor = MongoCursorCreate(mongoConnection,
													fmstate->options->svr_database,
													fmstate->options->collectionName,
													fmstate->queryDocument);
	mongo_free_options(options);
}

static List *
MongoPlanForeignModify(PlannerInfo *root,
				ModifyTable *plan,
				Index resultRelation,
				int subplan_index)
{
	CmdType         operation = plan->operation;
	RangeTblEntry   *rte = planner_rt_fetch(resultRelation, root);
	Relation        rel;
	List            *targetAttrs = NIL;

	/*
	 * Core code already has some lock on each rel being planned, so we can
	 * use NoLock here.
	 */
	rel = heap_open(rte->relid, NoLock);

	if (operation == CMD_INSERT)
	{
		TupleDesc tupdesc = RelationGetDescr(rel);
		int attnum;

		for (attnum = 1; attnum <= tupdesc->natts; attnum++)
		{
			Form_pg_attribute attr = tupdesc->attrs[attnum - 1];

			if (!attr->attisdropped)
				targetAttrs = lappend_int(targetAttrs, attnum);
		}
	}
	else if (operation == CMD_UPDATE)
	{
		Bitmapset  *tmpset = bms_copy(rte->modifiedCols);
		AttrNumber	col;

		while ((col = bms_first_member(tmpset)) >= 0)
		{
			col += FirstLowInvalidHeapAttributeNumber;
			if (col <= InvalidAttrNumber)		/* shouldn't happen */
				elog(ERROR, "system-column update is not supported");
			/*
			 * We also disallow updates to the first column which
			 * happens to be the row identifier in MongoDb (_id)
			 */
			if (col == 1)	/* shouldn't happen */
				elog(ERROR, "row identifier column update is not supported");

			targetAttrs = lappend_int(targetAttrs, col);
		}
		/* We also want the rowid column to be available for the update */
		targetAttrs = lcons_int(1, targetAttrs);
	}
	else
	{
		targetAttrs = lcons_int(1, targetAttrs);
	}
	/*
	 * RETURNING list not supported
	 */
	if (plan->returningLists)
		elog(ERROR, "RETURNING is not supported by this FDW");

	heap_close(rel, NoLock);

	return list_make1(targetAttrs);
}


/*
 * Begin an insert/update/delete operation on a foreign table
 */
static void
MongoBeginForeignModify(ModifyTableState *mtstate,
					ResultRelInfo *resultRelInfo,
					List *fdw_private,
					int subplan_index,
					int eflags)
{
	MongoFdwModifyState       *fmstate = NULL;
	Relation                  rel = resultRelInfo->ri_RelationDesc;
	AttrNumber                n_params = 0;
	Oid                       typefnoid = InvalidOid;
	bool                      isvarlena = false;
	ListCell                  *lc = NULL;
	Oid                       foreignTableId = InvalidOid;

	/*
	 * Do nothing in EXPLAIN (no ANALYZE) case. resultRelInfo->ri_FdwState
	 * stays NULL.
	 */
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	foreignTableId = RelationGetRelid(rel);

	/* Begin constructing MongoFdwModifyState. */
	fmstate = (MongoFdwModifyState *) palloc0(sizeof(MongoFdwModifyState));

	fmstate->rel = rel;
	fmstate->options = mongo_get_options(foreignTableId);

	fmstate->target_attrs = (List *) list_nth(fdw_private, 0);

	n_params = list_length(fmstate->target_attrs) + 1;
	fmstate->p_flinfo = (FmgrInfo *) palloc0(sizeof(FmgrInfo) * n_params);
	fmstate->p_nums = 0;

	/* Set up for remaining transmittable parameters */
	foreach(lc, fmstate->target_attrs)
	{
		int attnum = lfirst_int(lc);
		Form_pg_attribute attr = RelationGetDescr(rel)->attrs[attnum - 1];

		Assert(!attr->attisdropped);

		getTypeOutputInfo(attr->atttypid, &typefnoid, &isvarlena);
		fmgr_info(typefnoid, &fmstate->p_flinfo[fmstate->p_nums]);
		fmstate->p_nums++;
	}
	Assert(fmstate->p_nums <= n_params);

	resultRelInfo->ri_FdwState = fmstate;
}


/*
 * Insert one row into a foreign table.
 */
static TupleTableSlot *
MongoExecForeignInsert(EState *estate,
					ResultRelInfo *resultRelInfo,
					TupleTableSlot *slot,
					TupleTableSlot *planSlot)
{
	MongoFdwOptions           *options = NULL;
	MONGO_CONN                *mongoConnection = NULL;
	Oid                       foreignTableId = InvalidOid;
	BSON                      *b = NULL;
	Oid                       typoid;
	Datum                     value;
	bool                      isnull = false;
	Oid                       userid;
	ForeignServer             *server;
	UserMapping               *user;
	ForeignTable              *table;

	MongoFdwModifyState *fmstate = (MongoFdwModifyState *) resultRelInfo->ri_FdwState;

	foreignTableId = RelationGetRelid(resultRelInfo->ri_RelationDesc);

	userid = GetUserId();

	/* Get info about foreign table. */
	table = GetForeignTable(RelationGetRelid(fmstate->rel));
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/*
	 * Get connection to the foreign server. Connection manager will
	 * establish new connection if necessary.
	 */
	options = fmstate->options;
	mongoConnection = mongo_get_connection(server, user, options);

	b = BsonCreate();

	typoid = get_atttype(foreignTableId, 1);

	/* get following parameters from slot */
	if (slot != NULL && fmstate->target_attrs != NIL)
	{
		ListCell *lc;

		foreach(lc, fmstate->target_attrs)
		{
			int attnum = lfirst_int(lc);
			value = slot_getattr(slot, attnum, &isnull);

			/* first column of MongoDB's foreign table must be _id */
			if (strcmp(slot->tts_tupleDescriptor->attrs[0]->attname.data, "_id") != 0)
				elog(ERROR, "first column of MongoDB's foreign table must be \"_id\"");

			if (typoid != NAMEOID)
				elog(ERROR, "type of first column of MongoDB's foreign table must be \"NAME\"");

			if (attnum == 1)
			{
				/*
				 * Ignore the value of first column which is row identifier in MongoDb (_id)
				 * and let MongoDB to insert the unique value for that column.
				 */
			}
			else
			{
				AppenMongoValue(b, slot->tts_tupleDescriptor->attrs[attnum - 1]->attname.data, value,
						isnull, slot->tts_tupleDescriptor->attrs[attnum -1]->atttypid);
			}
		}
	}
	BsonFinish(b);

	/* Now we are ready to insert tuple / document into MongoDB */
	MongoInsert(mongoConnection, options->svr_database, options->collectionName, b);

	BsonDestroy(b);

	return slot;
}


/*
 * Add column(s) needed for update/delete on a foreign table, we are using
 * first column as row identification column, so we are adding that into target
 * list.
 */
static void
MongoAddForeignUpdateTargets(Query *parsetree,
						RangeTblEntry *target_rte,
						Relation target_relation)
{
	Var                 *var = NULL;
	const char          *attrname = NULL;
	TargetEntry         *tle = NULL;

	/*
	 * What we need is the rowid which is the first column
	 */
	Form_pg_attribute attr =
				RelationGetDescr(target_relation)->attrs[0];

	/* Make a Var representing the desired value */
	var = makeVar(parsetree->resultRelation,
				1,
				attr->atttypid,
				attr->atttypmod,
				InvalidOid,
				0);

	/* Wrap it in a TLE with the right name ... */
	attrname = NameStr(attr->attname);

	tle = makeTargetEntry((Expr *) var,
						list_length(parsetree->targetList) + 1,
						pstrdup(attrname),
						true);

	/* ... and add it to the query's targetlist */
	parsetree->targetList = lappend(parsetree->targetList, tle);
}


static TupleTableSlot *
MongoExecForeignUpdate(EState *estate,
					ResultRelInfo *resultRelInfo,
					TupleTableSlot *slot,
					TupleTableSlot *planSlot)
{
	MongoFdwOptions            *options = NULL;
	MONGO_CONN                 *mongoConnection = NULL;
	Datum                      datum = 0;
	bool                       isNull = false;
	Oid                        foreignTableId = InvalidOid;
	char                       *columnName = NULL;
	Oid                        typoid = InvalidOid;
	BSON                       *b = NULL;
	BSON                       *op = NULL;
	BSON                       set;
	Oid                        userid = GetUserId();
	ForeignServer              *server;
	UserMapping                *user;
	ForeignTable               *table;


	MongoFdwModifyState *fmstate = (MongoFdwModifyState *) resultRelInfo->ri_FdwState;

	foreignTableId = RelationGetRelid(resultRelInfo->ri_RelationDesc);

	/* resolve foreign table options; and connect to mongo server */
	options = fmstate->options;

	/* Get info about foreign table. */
	table = GetForeignTable(foreignTableId);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/*
	 * Get connection to the foreign server. Connection manager will
	 * establish new connection if necessary.
	 */
	mongoConnection = mongo_get_connection(server, user, options);

	/* Get the id that was passed up as a resjunk column */
	datum = ExecGetJunkAttribute(planSlot, 1, &isNull);

	columnName = get_relid_attribute_name(foreignTableId, 1);

	typoid = get_atttype(foreignTableId, 1);

	b = BsonCreate();
	BsonAppendStartObject(b, "$set", &set);

	/* get following parameters from slot */
	if (slot != NULL && fmstate->target_attrs != NIL)
	{
		ListCell *lc;

		foreach(lc, fmstate->target_attrs)
		{
			int attnum = lfirst_int(lc);
			Datum value;
			bool isnull;
 
		if (strcmp("_id", slot->tts_tupleDescriptor->attrs[attnum - 1]->attname.data) == 0)
			continue;

			value = slot_getattr(slot, attnum, &isnull);
#ifdef META_DRIVER
			AppenMongoValue(&set, slot->tts_tupleDescriptor->attrs[attnum - 1]->attname.data, value,
							isnull ? true : false, slot->tts_tupleDescriptor->attrs[attnum - 1]->atttypid);
#else
			AppenMongoValue(b, slot->tts_tupleDescriptor->attrs[attnum - 1]->attname.data, value,
							isnull ? true : false, slot->tts_tupleDescriptor->attrs[attnum - 1]->atttypid);
#endif
		}
	}
	BsonAppendFinishObject(b, &set);
	BsonFinish(b);

	op = BsonCreate();
	if (!AppenMongoValue(op, columnName, datum, false, typoid))
	{
		BsonDestroy(b);
		return NULL;
	}
	BsonFinish(op);

	/* We are ready to update the row into MongoDB */
	MongoUpdate(mongoConnection, options->svr_database, options->collectionName, op, b);

	BsonDestroy(op);
	BsonDestroy(b);

	/* Return NULL if nothing was updated on the remote end */
	return slot;
}

/*
 * MongoExecForeignDelete
 *		Delete one row from a foreign table
 */
static TupleTableSlot *
MongoExecForeignDelete(EState *estate,
					ResultRelInfo *resultRelInfo,
					TupleTableSlot *slot,
					TupleTableSlot *planSlot)
{
	MongoFdwOptions           *options = NULL;
	MONGO_CONN                *mongoConnection = NULL;
	Datum                     datum = 0;
	bool                      isNull = false;
	Oid                       foreignTableId = InvalidOid;
	char                      *columnName = NULL;
	Oid                       typoid = InvalidOid;
	BSON                      *b = NULL;
	Oid                       userid = GetUserId();
	ForeignServer             *server;
	UserMapping               *user;
	ForeignTable              *table;


	MongoFdwModifyState *fmstate = (MongoFdwModifyState *) resultRelInfo->ri_FdwState;

	foreignTableId = RelationGetRelid(resultRelInfo->ri_RelationDesc);

	/* resolve foreign table options; and connect to mongo server */
	options = fmstate->options;

	/* Get info about foreign table. */
	table = GetForeignTable(foreignTableId);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/*
	 * Get connection to the foreign server. Connection manager will
	 * establish new connection if necessary.
	 */
	mongoConnection = mongo_get_connection(server, user, options);

	/* Get the id that was passed up as a resjunk column */
	datum = ExecGetJunkAttribute(planSlot, 1, &isNull);

	columnName = get_relid_attribute_name(foreignTableId, 1);

	typoid = get_atttype(foreignTableId, 1);

	b = BsonCreate();
	if (!AppenMongoValue(b,columnName, datum, false, typoid))
	{
		BsonDestroy(b);
		return NULL;
	}
	BsonFinish(b);

	/* Now we are ready to delete a single document from MongoDB */
	MongoDelete(mongoConnection, options->svr_database, options->collectionName, b);

	BsonDestroy(b);

	/* Return NULL if nothing was updated on the remote end */
	return slot;
}

/*
 * MongoEndForeignModify
 *		Finish an insert/update/delete operation on a foreign table
 */
static void
MongoEndForeignModify(EState *estate, ResultRelInfo *resultRelInfo)
{
	MongoFdwModifyState *fmstate = (MongoFdwModifyState *) resultRelInfo->ri_FdwState;
	if (fmstate)
	{
		if (fmstate->options)
		{
			mongo_free_options(fmstate->options);
			fmstate->options = NULL;
		}
		MongoFreeScanState(fmstate);
		pfree(fmstate);
	}
}

/*
 * ForeignTableDocumentCount connects to the MongoDB server, and queries it for
 * the number of documents in the foreign collection. On success, the function
 * returns the document count. On failure, the function returns -1.0.
 */
static double
ForeignTableDocumentCount(Oid foreignTableId)
{
	MongoFdwOptions         *options = NULL;
	MONGO_CONN              *mongoConnection = NULL;
	const BSON              *emptyQuery = NULL;
	double                  documentCount = 0.0;
	Oid		        userid = GetUserId();
	ForeignServer           *server;
	UserMapping             *user;
	ForeignTable            *table;


	/* Get info about foreign table. */
	table = GetForeignTable(foreignTableId);
	server = GetForeignServer(table->serverid);
	user = GetUserMapping(userid, server->serverid);

	/* resolve foreign table options; and connect to mongo server */
	options = mongo_get_options(foreignTableId);

#ifdef META_DRIVER
	mongoConnection = mongo_get_connection(options->addressName, options->portNumber, options->databaseName, options->username, options->password, options->readPreference);
#else
	mongoConnection = mongo_get_connection(options->addressName, options->portNumber, options->databaseName, options->username, options->password);
#endif
	/*
	 * Get connection to the foreign server. Connection manager will
	 * establish new connection if necessary.
	 */
	mongoConnection = mongo_get_connection(server, user, options);


	documentCount = MongoAggregateCount(mongoConnection, options->svr_database, options->collectionName, emptyQuery);

	mongo_free_options(options);

	return documentCount;
}


/*
 * ColumnMappingHash creates a hash table that maps column names to column index
 * and types. This table helps us quickly translate BSON document key/values to
 * the corresponding PostgreSQL columns.
 */
static HTAB *
ColumnMappingHash(Oid foreignTableId, List *columnList)
{
	ListCell              *columnCell = NULL;
	const long            hashTableSize = 2048;
	HTAB                  *columnMappingHash = NULL;

	/* create hash table */
	HASHCTL hashInfo;
	memset(&hashInfo, 0, sizeof(hashInfo));
	hashInfo.keysize = NAMEDATALEN;
	hashInfo.entrysize = sizeof(ColumnMapping);
	hashInfo.hash = string_hash;
	hashInfo.hcxt = CurrentMemoryContext;

	columnMappingHash = hash_create("Column Mapping Hash", hashTableSize, &hashInfo,
									(HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT));
	Assert(columnMappingHash != NULL);

	foreach(columnCell, columnList)
	{
		Var *column = (Var *) lfirst(columnCell);
		AttrNumber columnId = column->varattno;

		ColumnMapping *columnMapping = NULL;
		char *columnName = NULL;
		bool handleFound = false;
		void *hashKey = NULL;

		columnName = get_relid_attribute_name(foreignTableId, columnId);
		hashKey = (void *) columnName;

		columnMapping = (ColumnMapping *) hash_search(columnMappingHash, hashKey,
													  HASH_ENTER, &handleFound);
		Assert(columnMapping != NULL);

		columnMapping->columnIndex = columnId - 1;
		columnMapping->columnTypeId = column->vartype;
		columnMapping->columnTypeMod = column->vartypmod;
		columnMapping->columnArrayTypeId = get_element_type(column->vartype);
	}

	return columnMappingHash;
}


/*
 * FillTupleSlot walks over all key/value pairs in the given document. For each
 * pair, the function checks if the key appears in the column mapping hash, and
 * if the value type is compatible with the one specified for the column. If so,
 * the function converts the value and fills the corresponding tuple position.
 * The bsonDocumentKey parameter is used for recursion, and should always be 
 * passed as NULL.
 */
static void
FillTupleSlot(const BSON *bsonDocument, const char *bsonDocumentKey,
			  HTAB *columnMappingHash, Datum *columnValues, bool *columnNulls)
{
	BSON_ITERATOR bsonIterator = { NULL, 0 };
	BsonIterInit(&bsonIterator, (BSON*)bsonDocument);

	while (BsonIterNext(&bsonIterator))
	{
		const char *bsonKey = BsonIterKey(&bsonIterator);
		BSON_TYPE bsonType = BsonIterType(&bsonIterator);

		ColumnMapping *columnMapping = NULL;
		Oid columnTypeId = InvalidOid;
		Oid columnArrayTypeId = InvalidOid;
		bool compatibleTypes = false;
		bool handleFound = false;
		const char *bsonFullKey = NULL;
		void *hashKey = NULL;

		if (bsonDocumentKey != NULL)
		{
			/*
			 * For fields in nested BSON objects, we use fully qualified field
			 * name to check the column mapping.
			 */
			StringInfo bsonFullKeyString = makeStringInfo();
			appendStringInfo(bsonFullKeyString, "%s.%s", bsonDocumentKey, bsonKey);
			bsonFullKey = bsonFullKeyString->data;
		}
		else
		{
			bsonFullKey = bsonKey;
		}

		/* look up the corresponding column for this bson key */
		hashKey = (void *) bsonFullKey;
		columnMapping = (ColumnMapping *) hash_search(columnMappingHash, hashKey,
													  HASH_FIND, &handleFound);
		if (columnMapping != NULL) {
			columnTypeId = columnMapping->columnTypeId;
			columnArrayTypeId = columnMapping->columnArrayTypeId;
		}

		/* recurse into nested objects */
		if (bsonType == BSON_TYPE_DOCUMENT && columnTypeId != JSONOID)
		{
			BSON subObject;
			BsonIterSubObject(&bsonIterator, &subObject);
			FillTupleSlot(&subObject, bsonFullKey,
						  columnMappingHash, columnValues, columnNulls);
			continue;
		}

		/* if no corresponding column or null BSON value, continue */
		if (columnMapping == NULL || bsonType == BSON_TYPE_NULL)
		{
			continue;
		}

		/* check if columns have compatible types */

		if (OidIsValid(columnArrayTypeId) && bsonType == BSON_TYPE_ARRAY)
		{
			compatibleTypes = true;
		}
		else
		{
			compatibleTypes = ColumnTypesCompatible(bsonType, columnTypeId);
		}

		/* if types are incompatible, leave this column null */
		if (!compatibleTypes)
		{
			continue;
		}

		/* fill in corresponding column value and null flag */
		if (OidIsValid(columnArrayTypeId))
		{
			int32 columnIndex = columnMapping->columnIndex;

			columnValues[columnIndex] = ColumnValueArray(&bsonIterator,
														 columnArrayTypeId);
			columnNulls[columnIndex] = false;
		}
		else
		{
			int32 columnIndex = columnMapping->columnIndex;
			Oid columnTypeMod = columnMapping->columnTypeMod;

			columnValues[columnIndex] = ColumnValue(&bsonIterator,
													columnTypeId, columnTypeMod);
			columnNulls[columnIndex] = false;
		}
	}
}


/*
 * ColumnTypesCompatible checks if the given BSON type can be converted to the
 * given PostgreSQL type. In this check, the function also uses its knowledge of
 * internal conversions applied by BSON APIs.
 */
static bool
ColumnTypesCompatible(BSON_TYPE bsonType, Oid columnTypeId)
{
	bool compatibleTypes = false;

	/* we consider the PostgreSQL column type as authoritative */
	switch(columnTypeId)
	{
		case INT2OID: case INT4OID:
		case INT8OID: case FLOAT4OID:
		case FLOAT8OID: case NUMERICOID:
		{
			if (bsonType == BSON_TYPE_INT32 || bsonType == BSON_TYPE_INT64 ||
				bsonType == BSON_TYPE_DOUBLE)
			{
				compatibleTypes = true;
			}
			break;
		}
		case BOOLOID:
		{
			if (bsonType == BSON_TYPE_INT32 || bsonType == BSON_TYPE_INT64 ||
				bsonType == BSON_TYPE_DOUBLE || bsonType == BSON_TYPE_BOOL)
			{
				compatibleTypes = true;
			}
			break;
		}
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
		{
			if (bsonType == BSON_TYPE_UTF8)
			{
				compatibleTypes = true;
			}
			break;
		}
		case BYTEAOID:
		{
			if (bsonType == BSON_TYPE_BINDATA)
			{
				compatibleTypes = true;
			}
#ifdef META_DRIVER
			if (bsonType == BSON_TYPE_OID)
			{
				compatibleTypes = true;
			}
#endif
			break;
		}
		case NAMEOID:
		{
			/*
			 * We currently overload the NAMEOID type to represent the BSON
			 * object identifier. We can safely overload this 64-byte data type
			 * since it's reserved for internal use in PostgreSQL.
			 */
			if (bsonType == BSON_TYPE_OID)
			{
				compatibleTypes = true;
			}
			break;
		}
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		{
			if (bsonType == BSON_TYPE_DATE_TIME)
			{
				compatibleTypes = true;
			}
			break;
		}
		case NUMERICARRAY_OID:
		{
			if (bsonType == BSON_TYPE_ARRAY)
				compatibleTypes = true;
			break;
		}
		case JSONOID:
		{
			if (bsonType == BSON_OBJECT || bsonType == BSON_ARRAY)
			{
				compatibleTypes = true;
			}
			break;
		}
		default:
		{
			/*
			 * We currently error out on other data types. Some types such as
			 * byte arrays are easy to add, but they need testing. Other types
			 * such as money or inet, do not have equivalents in MongoDB.
			 */
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							errmsg("cannot convert BSON type to column type"),
							errhint("Column type: %u", (uint32) columnTypeId)));
			break;
		}
	}

	return compatibleTypes;
}


/*
 * ColumnValueArray uses array element type id to read the current array pointed
 * to by the BSON iterator, and converts each array element (with matching type)
 * to the corresponding PostgreSQL datum. Then, the function constructs an array
 * datum from element datums, and returns the array datum.
 */
static Datum
ColumnValueArray(BSON_ITERATOR *bsonIterator, Oid valueTypeId)
{
	Datum          *columnValueArray = palloc0(INITIAL_ARRAY_CAPACITY * sizeof(Datum));
	uint32         arrayCapacity = INITIAL_ARRAY_CAPACITY;
	uint32         arrayGrowthFactor = 2;
	uint32         arrayIndex = 0;

	ArrayType      *columnValueObject = NULL;
	Datum          columnValueDatum = 0;
	bool           typeByValue = false;
	char           typeAlignment = 0;
	int16          typeLength = 0;

	BSON_ITERATOR bsonSubIterator = { NULL, 0 };
	BsonIterSubIter(bsonIterator, &bsonSubIterator);
	while (BsonIterNext(&bsonSubIterator))
	{
		BSON_TYPE bsonType = BsonIterType(&bsonSubIterator);
		bool compatibleTypes = false;

		compatibleTypes = ColumnTypesCompatible(bsonType, valueTypeId);
		if (bsonType == BSON_TYPE_NULL || !compatibleTypes)
		{
			continue;
		}

		if (arrayIndex >= arrayCapacity)
		{
			arrayCapacity *= arrayGrowthFactor;
			columnValueArray = repalloc(columnValueArray, arrayCapacity * sizeof(Datum));
		}

		/* use default type modifier (0) to convert column value */
		columnValueArray[arrayIndex] = ColumnValue(&bsonSubIterator, valueTypeId, 0);
		arrayIndex++;
	}

	get_typlenbyvalalign(valueTypeId, &typeLength, &typeByValue, &typeAlignment);
	columnValueObject = construct_array(columnValueArray, arrayIndex, valueTypeId,
										typeLength, typeByValue, typeAlignment);

	columnValueDatum = PointerGetDatum(columnValueObject);
	return columnValueDatum;
}


/*
 * ColumnValue uses column type information to read the current value pointed to
 * by the BSON iterator, and converts this value to the corresponding PostgreSQL
 * datum. The function then returns this datum.
 */
static Datum
ColumnValue(BSON_ITERATOR *bsonIterator, Oid columnTypeId, int32 columnTypeMod)
{
	Datum columnValue = 0;

	switch(columnTypeId)
	{
		case INT2OID:
		{
			int16 value = (int16) BsonIterInt32(bsonIterator);
			columnValue = Int16GetDatum(value);
			break;
		}
		case INT4OID:
		{
			int32 value = BsonIterInt32(bsonIterator);
			columnValue = Int32GetDatum(value);
			break;
		}
		case INT8OID:
		{
			int64 value = BsonIterInt64(bsonIterator);
			columnValue = Int64GetDatum(value);
			break;
		}
		case FLOAT4OID:
		{
			float4 value = (float4) BsonIterDouble(bsonIterator);
			columnValue = Float4GetDatum(value);
			break;
		}
		case FLOAT8OID:
		{
			float8 value = BsonIterDouble(bsonIterator);
			columnValue = Float8GetDatum(value);
			break;
		}
		case NUMERICOID:
		{
			float8 value = BsonIterDouble(bsonIterator);
			Datum valueDatum = Float8GetDatum(value);

			/* overlook type modifiers for numeric */
			columnValue = DirectFunctionCall1(float8_numeric, valueDatum);
			break;
		}
		case BOOLOID:
		{
			bool value = BsonIterBool(bsonIterator);
			columnValue = BoolGetDatum(value);
			break;
		}
		case BPCHAROID:
		{
			const char *value = BsonIterString(bsonIterator);
			Datum valueDatum = CStringGetDatum(value);

			columnValue = DirectFunctionCall3(bpcharin, valueDatum,
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(columnTypeMod));
			break;
		}
		case VARCHAROID:
		{
			const char *value = BsonIterString(bsonIterator);
			Datum valueDatum = CStringGetDatum(value);

			columnValue = DirectFunctionCall3(varcharin, valueDatum,
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(columnTypeMod));
			break;
		}
		case TEXTOID:
		{
			const char *value = BsonIterString(bsonIterator);
			columnValue = CStringGetTextDatum(value);
			break;
		}
		case NAMEOID:
		{
			char value[NAMEDATALEN];
			Datum valueDatum = 0;

			bson_oid_t *bsonObjectId = (bson_oid_t*) BsonIterOid(bsonIterator);
			bson_oid_to_string(bsonObjectId, value);

			valueDatum = CStringGetDatum(value);
			columnValue = DirectFunctionCall3(namein, valueDatum,
											  ObjectIdGetDatum(InvalidOid),
											  Int32GetDatum(columnTypeMod));
			break;
		}
		case BYTEAOID:
		{
			int value_len;
			const char *value;
#ifdef META_DRIVER
			switch (BsonIterType(bsonIterator))
			{
				case BSON_TYPE_OID:
					value = BsonIterOid(bsonIterator);
					value_len = 12;
					break;
				default: 
					value = BsonIterBinData(bsonIterator, &value_len);
					break;
			}
#else
			value_len = BsonIterBinLen(bsonIterator);
			value = BsonIterBinData(bsonIterator);
#endif
			bytea *result = (bytea *)palloc(value_len + VARHDRSZ);
			memcpy(VARDATA(result), value, value_len);
			SET_VARSIZE(result, value_len + VARHDRSZ);
			columnValue = PointerGetDatum(result);
			break;
		}
		case DATEOID:
		{
			int64 valueMillis = BsonIterDate(bsonIterator);
			int64 timestamp = (valueMillis * 1000L) - POSTGRES_TO_UNIX_EPOCH_USECS;
			Datum timestampDatum = TimestampGetDatum(timestamp);

			columnValue = DirectFunctionCall1(timestamp_date, timestampDatum);
			break;
		}
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		{
			int64 valueMillis = BsonIterDate(bsonIterator);
			int64 timestamp = (valueMillis * 1000L) - POSTGRES_TO_UNIX_EPOCH_USECS;

			/* overlook type modifiers for timestamp */
			columnValue = TimestampGetDatum(timestamp);
			break;
		}
		case JSONOID:
		{
			JsonLexContext *lex;
			text *result;
			bool is_array;
			StringInfo buffer = makeStringInfo();

			bson_type type = bson_iterator_type(bsonIterator);
			if (type != BSON_ARRAY && type != BSON_OBJECT)
			{
				ereport(ERROR, (errmsg("cannot convert scolar to json")));
			}
			is_array = (BSON_ARRAY == type);

#ifdef META_DRIVER
			if (is_array)
			{
				DumpJsonArray(buffer, bsonIterator);
			}
			else
			{
				DumpJsonObject(buffer, bsonIterator);
			}
#else
			DumpJson(buffer, bson_iterator_value(bsonIterator), is_array);
#endif

			result = cstring_to_text_with_len(buffer->data, buffer->len);

			/* validate it */
			lex = makeJsonLexContext(result, false);
			pg_parse_json(lex, &nullSemAction);
			columnValue = PointerGetDatum(result);
			break;
		}
		default:
		{
			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
							errmsg("cannot convert BSON type to column type"),
							errhint("Column type: %u", (uint32) columnTypeId)));
			break;
		}
	}

	return columnValue;
}


/*
 * DumpJson converts BSON document to a JSON string.
 * isArray signifies if bsonData is contents of array or object.
 * [Some of] special BSON datatypes are converted to JSON using
 * "Strict MongoDB Extended JSON" [1].
 *
 * [1] http://docs.mongodb.org/manual/reference/mongodb-extended-json/
 */
#ifdef META_DRIVER
void
DumpJsonObject(StringInfo output, BSON_ITERATOR *iter) {
	char *json;
	uint32_t len;
	const uint8_t *data;
	BSON bson;
	
	bson_iter_document(iter, &len, &data);

	if (bson_init_static(&bson, data, len))
	{
	  if((json = bson_as_json (&bson, NULL)))
	  {
		appendStringInfoString(output, json);
		bson_free(json);
	  }
	}
}

void
DumpJsonArray(StringInfo output, BSON_ITERATOR *iter) {
	char *json;
	uint32_t len;
	const uint8_t *data;
	BSON bson;

	bson_iter_array(iter, &len, &data);

	if (bson_init_static(&bson, data, len))
	{
	  if((json = bson_array_as_json (&bson, NULL)))
	  {
		appendStringInfoString(output, json);
		bson_free(json);
	  }
	}
}
#endif
void
DumpJson(StringInfo output, const char *bsonData, bool isArray)
{
	bson_iterator    i;
	const char       *key;
	bool             isFirstElement;
	char             beginSymbol, endSymbol;
	bson_type        t;


	bson_iterator_from_buffer(&i, bsonData);

	if (isArray)
	{
		beginSymbol = '[';
		endSymbol = ']';
	}
	else
	{
		beginSymbol = '{';
		endSymbol = '}';
	}

	appendStringInfoChar(output, beginSymbol);

	isFirstElement = true;
	while (bson_iterator_next(&i))
	{
		if (!isFirstElement)
		{
			appendStringInfoChar(output, ',');
		}

		t = bson_iterator_type(&i);
		if (t == 0) break;
		key = bson_iterator_key(&i);

		if (!isArray)
		{
			appendStringInfo(output, "\"%s\":", key);
		}

		switch (t)
		{
			case BSON_DOUBLE:
				appendStringInfo(output, "%f", bson_iterator_double(&i));
				break;
			case BSON_STRING:
				appendStringInfo(output, "\"%s\"",
						EscapeJsonString(bson_iterator_string(&i)));
				break;
			case BSON_SYMBOL:
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
								errmsg("`symbol` BSON type is deprecated and "
									   "unsupported"),
								errhint("Symbol: %s", bson_iterator_string(&i))));
				break;
			case BSON_OID:
			{
				char oidhex[25];
				bson_oid_to_string(bson_iterator_oid(&i), oidhex);
				appendStringInfo(output, "\"%s\"", oidhex);
				break;
			}
			case BSON_BOOL:
				appendStringInfoString(
						output, bson_iterator_bool(&i) ? "true" : "false");
				break;
			case BSON_DATE:
				appendStringInfo(output, "{\"$date\":%ld}",
						(long int)bson_iterator_date(&i));
				break;
			case BSON_BINDATA:
				/* It's possible to encode the data with base64 here. */
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
								errmsg("support for `binary data` BSON type "
									   "is not implemented")));
				break;
			case BSON_UNDEFINED:
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
								errmsg("`undefined` BSON type is deprecated "
									   "and unsupported")));
				break;
			case BSON_NULL:
				appendStringInfoString(output, "null");
				break;
			case BSON_REGEX:
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
								errmsg("support for `regex` BSON type is "
									   "not implemented"),
								errhint("Regex: %s", bson_iterator_regex(&i))));
				break;
			case BSON_CODE:
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
								errmsg("support for `code` BSON type is "
									   "not implemented"),
								errhint("Code: %s", bson_iterator_code(&i))));
				break;
			case BSON_CODEWSCOPE:
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
								errmsg("support for `code with scope` BSON "
									   "type is not implemented")));
				break;
			case BSON_INT:
				appendStringInfo(output, "%d", bson_iterator_int(&i));
				break;
			case BSON_LONG:
				appendStringInfo(output, "%lld", (uint64_t)bson_iterator_long(&i));
				break;
			case BSON_TIMESTAMP:
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
								errmsg("internal `timestamp` BSON type is "
									   "and unsupported")));
				break;
			case BSON_OBJECT:
				DumpJson(output, bson_iterator_value(&i), false);
				break;
			case BSON_ARRAY:
				DumpJson(output, bson_iterator_value(&i), true);
				break;
			default:
				ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
								errmsg("unsupported BSON type: %d", t)));
		}
		isFirstElement = false;
	}
	appendStringInfoChar(output, endSymbol);
}


/*
 * EscapeJsonString escapes a string for safe inclusion in JSON.
 */
const char *
EscapeJsonString(const char *string)
{
	StringInfo   buffer;
	const char   *ptr;
	int          i, segmentStartIdx, len;

	bool needsEscaping = false;
	for (ptr = string; *ptr; ++ptr) {
		if (*ptr == '"' || *ptr == '\r' || *ptr == '\n' || *ptr == '\t'\
				|| *ptr == '\\') {
			needsEscaping = true;
		}
	}

	if (!needsEscaping) return string;

	buffer = makeStringInfo();
	len = strlen(string);
	segmentStartIdx = 0;
	for (i = 0; i < len; ++i) {
		if (string[i] == '"' || string[i] == '\r' || string[i] == '\n'
				|| string[i] == '\t' || string[i] == '\\') {
			if (segmentStartIdx < i) {
				appendBinaryStringInfo(buffer, string + segmentStartIdx,
						i - segmentStartIdx);
			}

			appendStringInfoChar(buffer, '\\');
			if (string[i] == '"') appendStringInfoChar(buffer, '"');
			else if (string[i] == '\r') appendStringInfoChar(buffer, 'r');
			else if (string[i] == '\n') appendStringInfoChar(buffer, 'n');
			else if (string[i] == '\t') appendStringInfoChar(buffer, 't');
			else if (string[i] == '\\') appendStringInfoChar(buffer, '\\');

			segmentStartIdx = i + 1;
		}
	}
	if (segmentStartIdx < len) {
		appendBinaryStringInfo(buffer, string + segmentStartIdx,
				len - segmentStartIdx);
	}
	return buffer->data;
}


/*
 * MongoFreeScanState closes the cursor and connection to MongoDB, and reclaims
 * all Mongo related resources allocated for the foreign scan.
 */
static void
MongoFreeScanState(MongoFdwModifyState *fmstate)
{
	if (fmstate == NULL)
		return;

	if (fmstate->queryDocument)
	{
		BsonDestroy(fmstate->queryDocument);
		fmstate->queryDocument = NULL;
	}

	if (fmstate->mongoCursor)
	{
		MongoCursorDestroy(fmstate->mongoCursor);
		fmstate->mongoCursor = NULL;
	}

	/* Release remote connection */
	mongo_release_connection(fmstate->mongoConnection);
}


/*
 * MongoAnalyzeForeignTable collects statistics for the given foreign table.
 */
static bool
MongoAnalyzeForeignTable(Relation relation,
						 AcquireSampleRowsFunc *acquireSampleRowsFunc,
						 BlockNumber *totalPageCount)
{
	BlockNumber        pageCount = 0;
	int                attributeCount = 0;
	int32              *attributeWidths = NULL;
	Oid                foreignTableId = InvalidOid;
	int32              documentWidth = 0;
	double             documentCount = 0.0;
	double             foreignTableSize = 0;

	foreignTableId = RelationGetRelid(relation);

	documentCount = ForeignTableDocumentCount(foreignTableId);

	if (documentCount > 0.0)
	{
		attributeCount = RelationGetNumberOfAttributes(relation);
		attributeWidths = (int32 *)	palloc0((attributeCount + 1) * sizeof(int32));

		/*
		 * We estimate disk costs assuming a sequential scan over the data. This is
		 * an inaccurate assumption as Mongo scatters the data over disk pages, and
		 * may rely on an index to retrieve the data. Still, this should at least
		 * give us a relative cost.
		 */
		documentWidth = get_relation_data_width(foreignTableId, attributeWidths);
		foreignTableSize = documentCount * documentWidth;

		pageCount = (BlockNumber) rint(foreignTableSize / BLCKSZ);
	}
	else
	{
		ereport(ERROR, (errmsg("could not retrieve document count for collection"),
						 errhint("could not	collect statistics about foreign table")));
	}

	(*totalPageCount) = pageCount;
	(*acquireSampleRowsFunc) = MongoAcquireSampleRows;

	return true;
}


/*
 * MongoAcquireSampleRows acquires a random sample of rows from the foreign
 * table. Selected rows are returned in the caller allocated sampleRows array,
 * which must have at least target row count entries. The actual number of rows
 * selected is returned as the function result. We also count the number of rows
 * in the collection and return it in total row count. We also always set dead
 * row count to zero.
 *
 * Note that the returned list of rows is not always in order by physical
 * position in the MongoDB collection. Therefore, correlation estimates
 * derived later may be meaningless, but it's OK because we don't use the
 * estimates currently (the planner only pays attention to correlation for
 * index scans).
 */
static int
MongoAcquireSampleRows(Relation relation, int errorLevel,
					   HeapTuple *sampleRows, int targetRowCount,
					   double *totalRowCount, double *totalDeadRowCount)
{
	int                      sampleRowCount = 0;
	double                   rowCount = 0;
	double                   rowCountToSkip = -1; /* -1 means not set yet */
	double                   randomState = 0;
	Datum                    *columnValues = NULL;
	bool                     *columnNulls = NULL;
	Oid                      foreignTableId = InvalidOid;
	TupleDesc                tupleDescriptor = NULL;
	Form_pg_attribute        *attributesPtr = NULL;
	AttrNumber               columnCount = 0;
	AttrNumber               columnId = 0;
	HTAB                     *columnMappingHash = NULL;
	MONGO_CURSOR             *mongoCursor = NULL;
	BSON                     *queryDocument = NULL;
	List                     *columnList = NIL;
	ForeignScanState         *scanState = NULL;
	List                     *foreignPrivateList = NIL;
	ForeignScan              *foreignScan = NULL;
	MongoFdwModifyState      *fmstate = NULL;
	char                     *relationName = NULL;
	int                      executorFlags = 0;
	MemoryContext            oldContext = CurrentMemoryContext;
	MemoryContext            tupleContext = NULL;

	/* create list of columns in the relation */
	tupleDescriptor = RelationGetDescr(relation);
	columnCount = tupleDescriptor->natts;
	attributesPtr = tupleDescriptor->attrs;

	for (columnId = 1; columnId <= columnCount; columnId++)
	{
		Var *column = (Var *) palloc0(sizeof(Var));

		/* only assign required fields for column mapping hash */
		column->varattno = columnId;
		column->vartype = attributesPtr[columnId-1]->atttypid;
		column->vartypmod = attributesPtr[columnId-1]->atttypmod;

		columnList = lappend(columnList, column);
	}

	/* create state structure */
	scanState = makeNode(ForeignScanState);
	scanState->ss.ss_currentRelation = relation;

	foreignTableId = RelationGetRelid(relation);
	queryDocument = QueryDocument(foreignTableId, NIL);
	foreignPrivateList = list_make2(columnList, NULL);

	/* only clean up the query struct, but not its data */
	BsonDestroy(queryDocument);

	foreignScan = makeNode(ForeignScan);
	foreignScan->fdw_private = foreignPrivateList;

	scanState->ss.ps.plan = (Plan *) foreignScan;

	MongoBeginForeignScan(scanState, executorFlags);

	fmstate = (MongoFdwModifyState *) scanState->fdw_state;
	mongoCursor = fmstate->mongoCursor;
	columnMappingHash = fmstate->columnMappingHash;

	/*
	 * Use per-tuple memory context to prevent leak of memory used to read
	 * rows from the file with copy routines.
	 */
	tupleContext = AllocSetContextCreate(CurrentMemoryContext,
										 "mongo_fdw temporary context",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);

	/* prepare for sampling rows */
	randomState = anl_init_selection_state(targetRowCount);

	columnValues = (Datum *) palloc0(columnCount * sizeof(Datum));
	columnNulls = (bool *) palloc0(columnCount * sizeof(bool));	

	for (;;)
	{
		/* check for user-requested abort or sleep */
		vacuum_delay_point();

		/* initialize all values for this row to null */
		memset(columnValues, 0, columnCount * sizeof(Datum));
		memset(columnNulls, true, columnCount * sizeof(bool));

		if(MongoCursorNext(mongoCursor, NULL))
		{
			const BSON *bsonDocument = MongoCursorBson(mongoCursor);
			const char *bsonDocumentKey = NULL; /* top level document */

			/* fetch next tuple */
			MemoryContextReset(tupleContext);
			MemoryContextSwitchTo(tupleContext);

			FillTupleSlot(bsonDocument, bsonDocumentKey,
						  columnMappingHash, columnValues, columnNulls);

			MemoryContextSwitchTo(oldContext);
		}
		else
		{
			#ifdef META_DRIVER
			bson_error_t error;
			if (mongoc_cursor_error (mongoCursor, &error))
			{
				MongoFreeScanState(fmstate);
				ereport(ERROR, (errmsg("could not iterate over mongo collection"),
						errhint("Mongo driver error: %s", error.message)));
			}
			#else
				mongo_cursor_error_t errorCode = mongoCursor->err;
				if (errorCode != MONGO_CURSOR_EXHAUSTED)
				{
					MongoFreeScanState(fmstate);
					ereport(ERROR, (errmsg("could not iterate over mongo collection"),
							errhint("Mongo driver cursor error code: %d", errorCode)));
				}
			#endif
			break;
		}

		/*
		 * The first targetRowCount sample rows are simply copied into the
		 * reservoir. Then we start replacing tuples in the sample until we
		 * reach the end of the relation. This algorithm is from Jeff Vitter's
		 * paper (see more info in commands/analyze.c).
		 */
		if (sampleRowCount < targetRowCount)
		{
			sampleRows[sampleRowCount++] = heap_form_tuple(tupleDescriptor,
														   columnValues,
														   columnNulls);
		}
		else
		{
			/*
			 * t in Vitter's paper is the number of records already processed.
			 * If we need to compute a new S value, we must use the "not yet
			 * incremented" value of rowCount as t.
			 */
			if (rowCountToSkip < 0)
			{
				rowCountToSkip = anl_get_next_S(rowCount, targetRowCount,
												&randomState);
			}

			if (rowCountToSkip <= 0)
			{
				/*
				 * Found a suitable tuple, so save it, replacing one old tuple
				 * at random.
				 */
				int rowIndex = (int) (targetRowCount * anl_random_fract());
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

	/* clean up */
	MemoryContextDelete(tupleContext);
	MongoFreeScanState(fmstate);
	
	pfree(columnValues);
	pfree(columnNulls);

	/* emit some interesting relation info */
	relationName = RelationGetRelationName(relation);
	ereport(errorLevel, (errmsg("\"%s\": collection contains %.0f rows; %d rows in sample",
								relationName, rowCount, sampleRowCount)));

	(*totalRowCount) = rowCount;
	(*totalDeadRowCount) = 0;

	return sampleRowCount;
}
