/*-------------------------------------------------------------------------
 *
 * mongo_query.c
 * 		Foreign-data wrapper for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2004-2014, EnterpriseDB Corporation.
 *
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		mongo_query.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mongo_wrapper.h"

#ifdef META_DRIVER
	#include "mongoc.h"
#else
	#include "mongo.h"
#endif
#include "mongo_fdw.h"
#include "mongo_query.h"

#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/relation.h"
#include "optimizer/var.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/timestamp.h"


/* Local functions forward declarations */
static Expr * FindArgumentOfType(List *argumentList, NodeTag argumentType);
static char * MongoOperatorName(const char *operatorName);
static List * EqualityOperatorList(List *operatorList);
static List * UniqueColumnList(List *operatorList);
static List * ColumnOperatorList(Var *column, List *operatorList);
static void AppendConstantValue(BSON *queryDocument, const char *keyName,
								Const *constant);


/*
 * ApplicableOpExpressionList walks over all filter clauses that relate to this
 * foreign table, and chooses applicable clauses that we know we can translate
 * into Mongo queries. Currently, these clauses include comparison expressions
 * that have a column and a constant as arguments. For example, "o_orderdate >=
 * date '1994-01-01' + interval '1' year" is an applicable expression.
 */
List *
ApplicableOpExpressionList(RelOptInfo *baserel)
{
	List *opExpressionList = NIL;
	List *restrictInfoList = baserel->baserestrictinfo;
	ListCell *restrictInfoCell = NULL;

	foreach(restrictInfoCell, restrictInfoList)
	{
		RestrictInfo *restrictInfo = (RestrictInfo *) lfirst(restrictInfoCell);
		Expr *expression = restrictInfo->clause;
		NodeTag expressionType = 0;

		OpExpr *opExpression = NULL;
		char *operatorName = NULL;
		char *mongoOperatorName = NULL;
		List *argumentList = NIL;
		Var *column = NULL;
		Const *constant = NULL;
		bool equalsOperator = false;
		bool constantIsArray = false;

		/* we only support operator expressions */
		expressionType = nodeTag(expression);
		if (expressionType != T_OpExpr)
		{
			continue;
		}

		opExpression = (OpExpr *) expression;
		operatorName = get_opname(opExpression->opno);

		/* we only support =, <, >, <=, >=, and <> operators */
		if (strncmp(operatorName, EQUALITY_OPERATOR_NAME, NAMEDATALEN) == 0)
		{
			equalsOperator = true;
		}

		mongoOperatorName = MongoOperatorName(operatorName);
		if (!equalsOperator && mongoOperatorName == NULL)
		{
			continue;
		}

		/*
		 * We only support simple binary operators that compare a column against
		 * a constant. If the expression is a tree, we don't recurse into it.
		 */
		argumentList = opExpression->args;
		column = (Var *) FindArgumentOfType(argumentList, T_Var);
		constant = (Const *) FindArgumentOfType(argumentList, T_Const);

		/*
		 * We don't push down operators where the constant is an array, since
		 * conditional operators for arrays in MongoDB aren't properly defined.
		 * For example, {similar_products : [ "B0009S4IJW", "6301964144" ]}
		 * finds results that are equal to the array, but {similar_products:
		 * {$gte: [ "B0009S4IJW", "6301964144" ]}} returns an empty set.
		 */
		if (constant != NULL)
		{
			Oid constantArrayTypeId = get_element_type(constant->consttype);
			if (constantArrayTypeId != InvalidOid)
			{
				constantIsArray = true;
			}
		}

		if (column != NULL && constant != NULL && !constantIsArray)
		{
			opExpressionList = lappend(opExpressionList, opExpression);
		}
	}

	return opExpressionList;
}


/*
 * FindArgumentOfType walks over the given argument list, looks for an argument
 * with the given type, and returns the argument if it is found.
 */
static Expr *
FindArgumentOfType(List *argumentList, NodeTag argumentType)
{
	Expr *foundArgument = NULL;
	ListCell *argumentCell = NULL;

	foreach(argumentCell, argumentList)
	{
		Expr *argument = (Expr *) lfirst(argumentCell);
		if (nodeTag(argument) == argumentType)
		{
			foundArgument = argument;
			break;
		}
	}

	return foundArgument;
}


/*
 * QueryDocument takes in the applicable operator expressions for a relation and
 * converts these expressions into equivalent queries in MongoDB. For now, this
 * function can only transform simple comparison expressions, and returns these
 * transformed expressions in a BSON document. For example, simple expressions
 * "l_shipdate >= date '1994-01-01' AND l_shipdate < date '1995-01-01'" become
 * "l_shipdate: { $gte: new Date(757382400000), $lt: new Date(788918400000) }".
 */
BSON *
QueryDocument(Oid relationId, List *opExpressionList)
{
	List *equalityOperatorList = NIL;
	List *comparisonOperatorList = NIL;
	List *columnList = NIL;
	ListCell *equalityOperatorCell = NULL;
	ListCell *columnCell = NULL;
	BSON *queryDocument = NULL;

	queryDocument = BsonCreate();
	/*
	 * We distinguish between equality expressions and others since we need to
	 * insert the latter (<, >, <=, >=, <>) as separate sub-documents into the
	 * BSON query object.
	 */
	equalityOperatorList = EqualityOperatorList(opExpressionList);
	comparisonOperatorList = list_difference(opExpressionList, equalityOperatorList);

	/* append equality expressions to the query */
	foreach(equalityOperatorCell, equalityOperatorList)
	{
		OpExpr *equalityOperator = (OpExpr *) lfirst(equalityOperatorCell);
		Oid columnId = InvalidOid;
		char *columnName = NULL;

		List *argumentList = equalityOperator->args;
		Var *column = (Var *) FindArgumentOfType(argumentList, T_Var);
		Const *constant = (Const *) FindArgumentOfType(argumentList, T_Const);

		columnId = column->varattno;
		columnName = get_relid_attribute_name(relationId, columnId);

		AppendConstantValue(queryDocument, columnName, constant);
	}

	/*
	 * For comparison expressions, we need to group them by their columns and
	 * append all expressions that correspond to a column as one sub-document.
	 * Otherwise, even when we have two expressions to define the upper- and
	 * lower-bound of a range, Mongo uses only one of these expressions during
	 * an index search.
	 */
	columnList = UniqueColumnList(comparisonOperatorList);

	/* append comparison expressions, grouped by columns, to the query */
	foreach(columnCell, columnList)
	{
		Var *column = (Var *) lfirst(columnCell);
		Oid columnId = InvalidOid;
		char *columnName = NULL;
		List *columnOperatorList = NIL;
		ListCell *columnOperatorCell = NULL;
		BSON r;

		columnId = column->varattno;
		columnName = get_relid_attribute_name(relationId, columnId);

		/* find all expressions that correspond to the column */
		columnOperatorList = ColumnOperatorList(column, comparisonOperatorList);

		/* for comparison expressions, start a sub-document */
		BsonAppendStartObject(queryDocument, columnName, &r);

		foreach(columnOperatorCell, columnOperatorList)
		{
			OpExpr *columnOperator = (OpExpr *) lfirst(columnOperatorCell);
			char *operatorName = NULL;
			char *mongoOperatorName = NULL;

			List *argumentList = columnOperator->args;
			Const *constant = (Const *) FindArgumentOfType(argumentList, T_Const);

			operatorName = get_opname(columnOperator->opno);
			mongoOperatorName = MongoOperatorName(operatorName);
#ifdef META_DRIVER
			AppendConstantValue(&r, mongoOperatorName, constant);
#else
			AppendConstantValue(queryDocument, mongoOperatorName, constant);
#endif
		}
		BsonAppendFinishObject(queryDocument, &r);
	}

	if (!BsonFinish(queryDocument))
	{
#ifdef META_DRIVER
		ereport(ERROR, (errmsg("could not create document for query"),
						errhint("BSON flags: %d", queryDocument->flags)));
#else
		ereport(ERROR, (errmsg("could not create document for query"),
						errhint("BSON error: %d", queryDocument->err)));
#endif
	}

	return queryDocument;
}


/*
 * MongoOperatorName takes in the given PostgreSQL comparison operator name, and
 * returns its equivalent in MongoDB.
 */
static char *
MongoOperatorName(const char *operatorName)
{
	const char *mongoOperatorName = NULL;
	const int32 nameCount = 5;
	static const char *nameMappings[][2] = { { "<", "$lt" },
											 { ">", "$gt" },
											 { "<=", "$lte" },
											 { ">=", "$gte" },
											 { "<>", "$ne" } };

	int32 nameIndex = 0;
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


/*
 * EqualityOperatorList finds the equality (=) operators in the given list, and
 * returns these operators in a new list.
 */
static List *
EqualityOperatorList(List *operatorList)
{
	List *equalityOperatorList = NIL;
	ListCell *operatorCell = NULL;

	foreach(operatorCell, operatorList)
	{
		OpExpr *operator = (OpExpr *) lfirst(operatorCell);
		char *operatorName = NULL;

		operatorName = get_opname(operator->opno);
		if (strncmp(operatorName, EQUALITY_OPERATOR_NAME, NAMEDATALEN) == 0)
		{
			equalityOperatorList = lappend(equalityOperatorList, operator);
		}
	}

	return equalityOperatorList;
}


/*
 * UniqueColumnList walks over the given operator list, and extracts the column
 * argument in each operator. The function then de-duplicates extracted columns,
 * and returns them in a new list.
 */
static List *
UniqueColumnList(List *operatorList)
{
	List *uniqueColumnList = NIL;
	ListCell *operatorCell = NULL;

	foreach(operatorCell, operatorList)
	{
		OpExpr *operator = (OpExpr *) lfirst(operatorCell);
		List *argumentList = operator->args;
		Var *column = (Var *) FindArgumentOfType(argumentList, T_Var);

		/* list membership is determined via column's equal() function */
		uniqueColumnList = list_append_unique(uniqueColumnList, column);
	}

	return uniqueColumnList;
}


/*
 * ColumnOperatorList finds all expressions that correspond to the given column,
 * and returns them in a new list.
 */
static List *
ColumnOperatorList(Var *column, List *operatorList)
{
	List *columnOperatorList = NIL;
	ListCell *operatorCell = NULL;

	foreach(operatorCell, operatorList)
	{
		OpExpr *operator = (OpExpr *) lfirst(operatorCell);
		List *argumentList = operator->args;

		Var *foundColumn = (Var *) FindArgumentOfType(argumentList, T_Var);
		if (equal(column, foundColumn))
		{
			columnOperatorList = lappend(columnOperatorList, operator);
		}
	}

	return columnOperatorList;
}


/*
 * AppendConstantValue appends to the query document the key name and constant
 * value. The function translates the constant value from its PostgreSQL type to
 * its MongoDB equivalent.
 */
static void
AppendConstantValue(BSON *queryDocument, const char *keyName, Const *constant)
{
	if (constant->constisnull)
	{
		BsonAppendNull(queryDocument, keyName);
		return;
	}
	AppenMongoValue(queryDocument, keyName, constant->constvalue, false, constant->consttype);
}

bool
AppenMongoValue(BSON *queryDocument, const char *keyName, Datum value, bool isnull, Oid id)
{
	bool status;
	if (isnull)
	{
		status = BsonAppendNull(queryDocument, keyName);
		return status;
	}

	switch(id)
	{
		case INT2OID:
		{
			int16 valueInt = DatumGetInt16(value);
			status = BsonAppendInt32(queryDocument, keyName, (int) valueInt);
			break;
		}
		case INT4OID:
		{
			int32 valueInt = DatumGetInt32(value);
			status = BsonAppendInt32(queryDocument, keyName, valueInt);
			break;
		}
		case INT8OID:
		{
			int64 valueLong = DatumGetInt64(value);
			status = BsonAppendInt64(queryDocument, keyName, valueLong);
			break;
		}
		case FLOAT4OID:
		{
			float4 valueFloat = DatumGetFloat4(value);
			status = BsonAppendDouble(queryDocument, keyName, (double) valueFloat);
			break;
		}
		case FLOAT8OID:
		{
			float8 valueFloat = DatumGetFloat8(value);
			status = BsonAppendDouble(queryDocument, keyName, valueFloat);
			break;
		}
		case NUMERICOID:
		{
			Datum valueDatum = DirectFunctionCall1(numeric_float8, value);
			float8 valueFloat = DatumGetFloat8(valueDatum);
			status = BsonAppendDouble(queryDocument, keyName, valueFloat);
			break;
		}
		case BOOLOID:
		{
			bool valueBool = DatumGetBool(value);
			status = BsonAppendBool(queryDocument, keyName, (int) valueBool);
			break;
		}
		case BPCHAROID:
		case VARCHAROID:
		case TEXTOID:
		{
			char *outputString = NULL;
			Oid outputFunctionId = InvalidOid;
			bool typeVarLength = false;
			getTypeOutputInfo(id, &outputFunctionId, &typeVarLength);
			outputString = OidOutputFunctionCall(outputFunctionId, value);
			status = BsonAppendUTF8(queryDocument, keyName, outputString);
			break;
		}
		case BYTEAOID:
		{
			int len;
			char *data;
			char *result = DatumGetPointer(value);
			if (VARATT_IS_1B(result)) {
				len = VARSIZE_1B(result) - VARHDRSZ_SHORT;
				data = VARDATA_1B(result);
			} else {
				len = VARSIZE_4B(result) - VARHDRSZ;
				data = VARDATA_4B(result);
			}
			status = BsonAppendBinary(queryDocument, keyName, data, len);
			break;
		}
		case NAMEOID:
		{
			char *outputString = NULL;
			Oid outputFunctionId = InvalidOid;
			bool typeVarLength = false;
			bson_oid_t bsonObjectId;
			memset(bsonObjectId.bytes, 0, sizeof(bsonObjectId.bytes));
			getTypeOutputInfo(id, &outputFunctionId, &typeVarLength);
			outputString = OidOutputFunctionCall(outputFunctionId, value);
			BsonOidFromString(&bsonObjectId, outputString);
			status = BsonAppendOid(queryDocument, keyName, &bsonObjectId);
			break;
		}
		case DATEOID:
		{
			Datum valueDatum = DirectFunctionCall1(date_timestamp, value);
			Timestamp valueTimestamp = DatumGetTimestamp(valueDatum);
			int64 valueMicroSecs = valueTimestamp + POSTGRES_TO_UNIX_EPOCH_USECS;
			int64 valueMilliSecs = valueMicroSecs / 1000;

			status = BsonAppendDate(queryDocument, keyName, valueMilliSecs);
			break;
		}
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		{
			Timestamp valueTimestamp = DatumGetTimestamp(value);
			int64 valueMicroSecs = valueTimestamp + POSTGRES_TO_UNIX_EPOCH_USECS;
			int64 valueMilliSecs = valueMicroSecs / 1000;

			status = BsonAppendDate(queryDocument, keyName, valueMilliSecs);
			break;
		}
		case NUMERICARRAY_OID:
		{
			ArrayType *array;
			Oid elmtype;
			int16 elmlen;
			bool elmbyval;
			char elmalign;
			int num_elems;
			Datum *elem_values;
			bool *elem_nulls;
			int i;
			BSON t;

			array = DatumGetArrayTypeP(value);
			elmtype = ARR_ELEMTYPE(array);
			get_typlenbyvalalign(elmtype, &elmlen, &elmbyval, &elmalign);

			deconstruct_array(array, elmtype, elmlen, elmbyval, elmalign, &elem_values, &elem_nulls, &num_elems);

			BsonAppendStartArray(queryDocument, keyName, &t);
			for (i = 0; i < num_elems; i++)
			{
				Datum valueDatum;
				float8 valueFloat;
				if (elem_nulls[i])
					continue;

				valueDatum = DirectFunctionCall1(numeric_float8, elem_values[i]);
				valueFloat = DatumGetFloat8(valueDatum);
#ifdef META_DRIVER
				status = BsonAppendDouble(&t, keyName, valueFloat);
#else
				status = BsonAppendDouble(queryDocument, keyName, valueFloat);
#endif
			}
			BsonAppendFinishArray(queryDocument, &t);
			pfree(elem_values);
			pfree(elem_nulls);
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
					errmsg("cannot convert constant value to BSON value"),
						errhint("Constant value data type: %u", id)));
			break;
		}
	}
	return status;
}


/*
 * ColumnList takes in the planner's information about this foreign table. The
 * function then finds all columns needed for query execution, including those
 * used in projections, joins, and filter clauses, de-duplicates these columns,
 * and returns them in a new list.
 */
List *
ColumnList(RelOptInfo *baserel)
{
	List *columnList = NIL;
	List *neededColumnList = NIL;
	AttrNumber columnIndex = 1;
	AttrNumber columnCount = baserel->max_attr;
	List *targetColumnList = baserel->reltargetlist;
	List *restrictInfoList = baserel->baserestrictinfo;
	ListCell *restrictInfoCell = NULL;

	/* first add the columns used in joins and projections */
	neededColumnList = list_copy(targetColumnList);

	/* then walk over all restriction clauses, and pull up any used columns */
	foreach(restrictInfoCell, restrictInfoList)
	{
		RestrictInfo *restrictInfo = (RestrictInfo *) lfirst(restrictInfoCell);
		Node *restrictClause = (Node *) restrictInfo->clause;
		List *clauseColumnList = NIL;

		/* recursively pull up any columns used in the restriction clause */
		clauseColumnList = pull_var_clause(restrictClause,
										   PVC_RECURSE_AGGREGATES,
										   PVC_RECURSE_PLACEHOLDERS);

		neededColumnList = list_union(neededColumnList, clauseColumnList);
	}

	/* walk over all column definitions, and de-duplicate column list */
	for (columnIndex = 1; columnIndex <= columnCount; columnIndex++)
	{
		ListCell *neededColumnCell = NULL;
		Var *column = NULL;

		/* look for this column in the needed column list */
		foreach(neededColumnCell, neededColumnList)
		{
			Var *neededColumn = (Var *) lfirst(neededColumnCell);
			if (neededColumn->varattno == columnIndex)
			{
				column = neededColumn;
				break;
			}
		}

		if (column != NULL)
		{
			columnList = lappend(columnList, column);
		}
	}

	return columnList;
}
