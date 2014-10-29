/*-------------------------------------------------------------------------
 *
 * option.c
 * 		Foreign-data wrapper for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2004-2014, EnterpriseDB Corporation.
 *
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		option.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "mongo_wrapper.h"
#include "mongo_fdw.h"

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
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/memutils.h"
#include "miscadmin.h"

static char * mongo_get_option_value(Oid foreignTableId, const char *optionName);

/*
 * Validate the generic options given to a FOREIGN DATA WRAPPER, SERVER,
 * USER MAPPING or FOREIGN TABLE that uses postgres_fdw.
 *
 * Raise an ERROR if the option or its value is considered invalid.
 */
extern Datum mongo_fdw_validator(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(mongo_fdw_validator);

/*
 * mongo_fdw_validator validates options given to one of the following commands:
 * foreign data wrapper, server, user mapping, or foreign table. This function
 * errors out if the given option name or its value is considered invalid.
 */
Datum
mongo_fdw_validator(PG_FUNCTION_ARGS)
{
	Datum optionArray = PG_GETARG_DATUM(0);
	Oid optionContextId = PG_GETARG_OID(1);
	List *optionList = untransformRelOptions(optionArray);
	ListCell *optionCell = NULL;

	foreach(optionCell, optionList)
	{
		DefElem *optionDef = (DefElem *) lfirst(optionCell);
		char *optionName = optionDef->defname;
		bool optionValid = false;

		int32 optionIndex = 0;
		for (optionIndex = 0; optionIndex < ValidOptionCount; optionIndex++)
		{
			const MongoValidOption *validOption = &(ValidOptionArray[optionIndex]);

			if ((optionContextId == validOption->optionContextId) &&
				(strncmp(optionName, validOption->optionName, NAMEDATALEN) == 0))
			{
				optionValid = true;
				break;
			}
		}

		/* if invalid option, display an informative error message */
		if (!optionValid)
		{
			StringInfo optionNamesString = mongo_option_names_string(optionContextId);

			ereport(ERROR, (errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
							errmsg("invalid option \"%s\"", optionName),
							errhint("Valid options in this context are: %s",
									optionNamesString->data)));
		}

		/* if port option is given, error out if its value isn't an integer */
		if (strncmp(optionName, OPTION_NAME_PORT, NAMEDATALEN) == 0)
		{
			char *optionValue = defGetString(optionDef);
			int32 portNumber = pg_atoi(optionValue, sizeof(int32), 0);
			(void) portNumber;
		}
	}
	PG_RETURN_VOID();
}

/*
 * mongo_option_names_string finds all options that are valid for the current context,
 * and concatenates these option names in a comma separated string.
 */
StringInfo
mongo_option_names_string(Oid currentContextId)
{
	StringInfo optionNamesString = makeStringInfo();
	bool firstOptionPrinted = false;

	int32 optionIndex = 0;
	for (optionIndex = 0; optionIndex < ValidOptionCount; optionIndex++)
	{
		const MongoValidOption *validOption = &(ValidOptionArray[optionIndex]);

		/* if option belongs to current context, append option name */
		if (currentContextId == validOption->optionContextId)
		{
			if (firstOptionPrinted)
				appendStringInfoString(optionNamesString, ", ");

			appendStringInfoString(optionNamesString, validOption->optionName);
			firstOptionPrinted = true;
		}
	}
	return optionNamesString;
}


/*
 * mongo_get_options returns the option values to be used when connecting to and
 * querying MongoDB. To resolve these values, the function checks the foreign
 * table's options, and if not present, falls back to default values.
 */
MongoFdwOptions *
mongo_get_options(Oid foreignTableId)
{
	MongoFdwOptions *mongoFdwOptions = NULL;
	char *addressName = NULL;
	char *portName = NULL;
	int32 portNumber = 0;
	char *databaseName = NULL;
	char *collectionName = NULL;
	char *username= NULL;
	char *password= NULL;
#ifdef META_DRIVER
	char *readPreference = NULL;

	readPreference = mongo_get_option_value(foreignTableId, OPTION_NAME_READ_PREFERENCE);
#endif

	addressName = mongo_get_option_value(foreignTableId, OPTION_NAME_ADDRESS);
	if (addressName == NULL)
		addressName = pstrdup(DEFAULT_IP_ADDRESS);

	portName = mongo_get_option_value(foreignTableId, OPTION_NAME_PORT);
	if (portName == NULL)
		portNumber = DEFAULT_PORT_NUMBER;
	else
		portNumber = pg_atoi(portName, sizeof(int32), 0);

	databaseName = mongo_get_option_value(foreignTableId, OPTION_NAME_DATABASE);
	if (databaseName == NULL)
		databaseName = pstrdup(DEFAULT_DATABASE_NAME);

	collectionName = mongo_get_option_value(foreignTableId, OPTION_NAME_COLLECTION);
	if (collectionName == NULL)
		collectionName = get_rel_name(foreignTableId);

	username = mongo_get_option_value(foreignTableId, OPTION_NAME_USERNAME);
	password = mongo_get_option_value(foreignTableId, OPTION_NAME_PASSWORD);

	mongoFdwOptions = (MongoFdwOptions *) palloc0(sizeof(MongoFdwOptions));
	mongoFdwOptions->addressName = addressName;
	mongoFdwOptions->portNumber = portNumber;
	mongoFdwOptions->databaseName = databaseName;
	mongoFdwOptions->collectionName = collectionName;
	mongoFdwOptions->username = username;
	mongoFdwOptions->password = password;
#ifdef META_DRIVER
	mongoFdwOptions->readPreference = readPreference;
#endif

	return mongoFdwOptions;
}

void
mongo_free_options(MongoFdwOptions *mongoFdwOptions)
{
	if (mongoFdwOptions)
	{
		pfree(mongoFdwOptions->addressName);
		pfree(mongoFdwOptions->databaseName);
		pfree(mongoFdwOptions);
	}
}

/*
 * mongo_get_option_value walks over foreign table and foreign server options, and
 * looks for the option with the given name. If found, the function returns the
 * option's value.
 */
static char *
mongo_get_option_value(Oid foreignTableId, const char *optionName)
{
	ForeignTable *foreignTable = NULL;
	ForeignServer *foreignServer = NULL;
	List *optionList = NIL;
	ListCell *optionCell = NULL;
	UserMapping *mapping= NULL;
	char *optionValue = NULL;

	foreignTable = GetForeignTable(foreignTableId);
	foreignServer = GetForeignServer(foreignTable->serverid);
	mapping = GetUserMapping(GetUserId(), foreignTable->serverid);

	optionList = list_concat(optionList, foreignTable->options);
	optionList = list_concat(optionList, foreignServer->options);
	optionList = list_concat(optionList, mapping->options);

	foreach(optionCell, optionList)
	{
		DefElem *optionDef = (DefElem *) lfirst(optionCell);
		char *optionDefName = optionDef->defname;

		if (strncmp(optionDefName, optionName, NAMEDATALEN) == 0)
		{
			optionValue = defGetString(optionDef);
			break;
		}
	}
	return optionValue;
}

