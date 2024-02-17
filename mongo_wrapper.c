/*-------------------------------------------------------------------------
 *
 * mongo_wrapper.c
 * 		Wrapper functions for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2024, EnterpriseDB Corporation.
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		mongo_wrapper.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <mongoc.h>
#include "mongo_wrapper.h"

#define ITER_TYPE(i) ((bson_type_t) * ((i)->raw + (i)->type))

/*
 * mongoConnect
 *		Connect to MongoDB server using Host/ip and Port number.
 */
MONGO_CONN *
mongoConnect(MongoFdwOptions *opt)
{
	MONGO_CONN *client;
	char	   *uri;

	if (opt->svr_username && opt->svr_password)
	{
		if (opt->authenticationDatabase)
		{
			if (opt->replicaSet)
			{
				if (opt->readPreference)
					uri = bson_strdup_printf("mongodb://%s:%s@%s:%hu/%s?readPreference=%s&ssl=%s&authSource=%s&replicaSet=%s",
											 opt->svr_username,
											 opt->svr_password,
											 opt->svr_address, opt->svr_port,
											 opt->svr_database,
											 opt->readPreference,
											 opt->ssl ? "true" : "false",
											 opt->authenticationDatabase,
											 opt->replicaSet);
				else
					uri = bson_strdup_printf("mongodb://%s:%s@%s:%hu/%s?ssl=%s&authSource=%s&replicaSet=%s",
											 opt->svr_username,
											 opt->svr_password,
											 opt->svr_address, opt->svr_port,
											 opt->svr_database,
											 opt->ssl ? "true" : "false",
											 opt->authenticationDatabase,
											 opt->replicaSet);
			}
			else if (opt->readPreference)
				uri = bson_strdup_printf("mongodb://%s:%s@%s:%hu/%s?readPreference=%s&ssl=%s&authSource=%s",
										 opt->svr_username, opt->svr_password,
										 opt->svr_address, opt->svr_port,
										 opt->svr_database,
										 opt->readPreference,
										 opt->ssl ? "true" : "false",
										 opt->authenticationDatabase);
			else
				uri = bson_strdup_printf("mongodb://%s:%s@%s:%hu/%s?ssl=%s&authSource=%s",
										 opt->svr_username, opt->svr_password,
										 opt->svr_address, opt->svr_port,
										 opt->svr_database,
										 opt->ssl ? "true" : "false",
										 opt->authenticationDatabase);
		}
		else if (opt->replicaSet)
		{
			if (opt->readPreference)
				uri = bson_strdup_printf("mongodb://%s:%s@%s:%hu/%s?readPreference=%s&ssl=%s&replicaSet=%s",
										 opt->svr_username, opt->svr_password,
										 opt->svr_address, opt->svr_port,
										 opt->svr_database,
										 opt->readPreference,
										 opt->ssl ? "true" : "false",
										 opt->replicaSet);
			else
				uri = bson_strdup_printf("mongodb://%s:%s@%s:%hu/%s?ssl=%s&replicaSet=%s",
										 opt->svr_username, opt->svr_password,
										 opt->svr_address, opt->svr_port,
										 opt->svr_database,
										 opt->ssl ? "true" : "false",
										 opt->replicaSet);
		}
		else if (opt->readPreference)
			uri = bson_strdup_printf("mongodb://%s:%s@%s:%hu/%s?readPreference=%s&ssl=%s",
									 opt->svr_username, opt->svr_password,
									 opt->svr_address, opt->svr_port,
									 opt->svr_database, opt->readPreference,
									 opt->ssl ? "true" : "false");
		else
			uri = bson_strdup_printf("mongodb://%s:%s@%s:%hu/%s?ssl=%s",
									 opt->svr_username, opt->svr_password,
									 opt->svr_address,
									 opt->svr_port, opt->svr_database,
									 opt->ssl ? "true" : "false");
	}
	else if (opt->replicaSet)
	{
		if (opt->readPreference)
			uri = bson_strdup_printf("mongodb://%s:%hu/%s?readPreference=%s&ssl=%s&replicaSet=%s",
									 opt->svr_address, opt->svr_port,
									 opt->svr_database, opt->readPreference,
									 opt->ssl ? "true" : "false",
									 opt->replicaSet);
		else
			uri = bson_strdup_printf("mongodb://%s:%hu/%s?ssl=%s&replicaSet=%s",
									 opt->svr_address, opt->svr_port,
									 opt->svr_database,
									 opt->ssl ? "true" : "false",
									 opt->replicaSet);
	}
	else if (opt->readPreference)
		uri = bson_strdup_printf("mongodb://%s:%hu/%s?readPreference=%s&ssl=%s",
								 opt->svr_address, opt->svr_port,
								 opt->svr_database, opt->readPreference,
								 opt->ssl ? "true" : "false");
	else
		uri = bson_strdup_printf("mongodb://%s:%hu/%s?ssl=%s",
								 opt->svr_address, opt->svr_port,
								 opt->svr_database,
								 opt->ssl ? "true" : "false");


	client = mongoc_client_new(uri);

	if (opt->ssl)
	{
		mongoc_ssl_opt_t *ssl_opts = (mongoc_ssl_opt_t *) malloc(sizeof(mongoc_ssl_opt_t));

		ssl_opts->pem_file = opt->pem_file;
		ssl_opts->pem_pwd = opt->pem_pwd;
		ssl_opts->ca_file = opt->ca_file;
		ssl_opts->ca_dir = opt->ca_dir;
		ssl_opts->crl_file = opt->crl_file;
		ssl_opts->weak_cert_validation = opt->weak_cert_validation;
		mongoc_client_set_ssl_opts(client, ssl_opts);
		free(ssl_opts);
	}

	bson_free(uri);

	if (client == NULL)
		ereport(ERROR,
				(errmsg("could not connect to %s:%d", opt->svr_address,
						opt->svr_port),
				 errhint("Mongo driver connection error.")));

	return client;
}

/*
 * mongoDisconnect
 *		Disconnect from MongoDB server.
 */
void
mongoDisconnect(MONGO_CONN *conn)
{
	if (conn)
		mongoc_client_destroy(conn);
}

/*
 * mongoInsert
 *		Insert a document 'b' into MongoDB.
 */
bool
mongoInsert(MONGO_CONN *conn, char *database, char *collection, BSON *b)
{
	mongoc_collection_t *c;
	bson_error_t error;
	bool		r = false;

	c = mongoc_client_get_collection(conn, database, collection);

	r = mongoc_collection_insert(c, MONGOC_INSERT_NONE, b, NULL, &error);
	mongoc_collection_destroy(c);
	if (!r)
		ereport(ERROR,
				(errmsg("failed to insert row"),
				 errhint("Mongo error: \"%s\"", error.message)));

	return true;
}

/*
 * mongoUpdate
 *		Update a document 'b' into MongoDB.
 */
bool
mongoUpdate(MONGO_CONN *conn, char *database, char *collection, BSON *b,
			BSON *op)
{
	mongoc_collection_t *c;
	bson_error_t error;
	bool		r = false;

	c = mongoc_client_get_collection(conn, database, collection);

	r = mongoc_collection_update(c, MONGOC_UPDATE_NONE, b, op, NULL, &error);
	mongoc_collection_destroy(c);
	if (!r)
		ereport(ERROR,
				(errmsg("failed to update row"),
				 errhint("Mongo error: \"%s\"", error.message)));

	return true;
}

/*
 * mongoDelete
 *		Delete MongoDB's document.
 */
bool
mongoDelete(MONGO_CONN *conn, char *database, char *collection, BSON *b)
{
	mongoc_collection_t *c;
	bson_error_t error;
	bool		r = false;

	c = mongoc_client_get_collection(conn, database, collection);

	r = mongoc_collection_remove(c, MONGOC_DELETE_SINGLE_REMOVE, b, NULL,
								 &error);
	mongoc_collection_destroy(c);
	if (!r)
		ereport(ERROR,
				(errmsg("failed to delete row"),
				 errhint("Mongo error: \"%s\"", error.message)));

	return true;
}

/*
 * mongoCursorCreate
 *		Performs a query against the configured MongoDB server and return
 *		cursor which can be destroyed by calling mongoc_cursor_current.
 */
MONGO_CURSOR *
mongoCursorCreate(MONGO_CONN *conn, char *database, char *collection, BSON *q)
{
	mongoc_collection_t *c;
	MONGO_CURSOR *cur;
	bson_error_t error;

	c = mongoc_client_get_collection(conn, database, collection);
	cur = mongoc_collection_aggregate(c, MONGOC_QUERY_NONE, q, NULL, NULL);
	mongoc_cursor_error(cur, &error);
	if (!cur)
		ereport(ERROR,
				(errmsg("failed to create cursor"),
				 errhint("Mongo error: \"%s\"", error.message)));

	mongoc_collection_destroy(c);

	return cur;
}

/*
 * mongoCursorDestroy
 *		Destroy cursor created by calling mongoCursorCreate function.
 */
void
mongoCursorDestroy(MONGO_CURSOR *c)
{
	mongoc_cursor_destroy(c);
}


/*
 * mongoCursorBson
 *		Get the current document from cursor.
 */
const BSON *
mongoCursorBson(MONGO_CURSOR *c)
{
	return mongoc_cursor_current(c);
}

/*
 * mongoCursorNext
 *		Get the next document from the cursor.
 */
bool
mongoCursorNext(MONGO_CURSOR *c, BSON *b)
{
	return mongoc_cursor_next(c, (const BSON **) &b);
}

/*
 * bsonCreate
 *		Allocates a new bson_t structure, and also initialize the bson object.
 *
 * After that point objects can be appended to that bson object and can be
 * iterated. A newly allocated bson_t that should be freed with bson_destroy().
 */
BSON *
bsonCreate(void)
{
	BSON	   *doc;

	doc = bson_new();
	bson_init(doc);

	return doc;
}

/*
 * bsonDestroy
 *		Destroy Bson object created by bsonCreate function.
 */
void
bsonDestroy(BSON *b)
{
	bson_destroy(b);
}

/*
 * bsonIterInit
 *		Initialize the bson Iterator.
 */
bool
bsonIterInit(BSON_ITERATOR *it, BSON *b)
{
	return bson_iter_init(it, b);
}

bool
bsonIterSubObject(BSON_ITERATOR *it, BSON *b)
{
	const uint8_t *buffer;
	uint32_t	len;

	bson_iter_document(it, &len, &buffer);
	bson_init_static(b, buffer, len);

	return true;
}

int32_t
bsonIterInt32(BSON_ITERATOR *it)
{
	BSON_ASSERT(it);
	switch ((int) ITER_TYPE(it))
	{
		case BSON_TYPE_BOOL:
			return (int32) bson_iter_bool(it);
		case BSON_TYPE_DOUBLE:
			{
				double		val = bson_iter_double(it);

				if (val < PG_INT32_MIN || val > PG_INT32_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("value \"%f\" is out of range for type integer",
									val)));

				return (int32) val;
			}
		case BSON_TYPE_INT64:
			{
				int64		val = bson_iter_int64(it);

				if (val < PG_INT32_MIN || val > PG_INT32_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("value \"%ld\" is out of range for type integer",
									val)));

				return (int32) val;
			}
		case BSON_TYPE_INT32:
			return bson_iter_int32(it);
		default:
			return 0;
	}
}

int64_t
bsonIterInt64(BSON_ITERATOR *it)
{
	return bson_iter_as_int64(it);
}

double
bsonIterDouble(BSON_ITERATOR *it)
{
	return bson_iter_as_double(it);
}

bool
bsonIterBool(BSON_ITERATOR *it)
{
	return bson_iter_as_bool(it);
}

const char *
bsonIterString(BSON_ITERATOR *it)
{
	uint32_t	len = 0;

	return bson_iter_utf8(it, &len);
}

const char *
bsonIterBinData(BSON_ITERATOR *it, uint32_t *len)
{
	const uint8_t *binary = NULL;
	bson_subtype_t subtype = BSON_SUBTYPE_BINARY;

	bson_iter_binary(it, &subtype, len, &binary);

	return (char *) binary;
}

const bson_oid_t *
bsonIterOid(BSON_ITERATOR *it)
{
	return bson_iter_oid(it);
}

time_t
bsonIterDate(BSON_ITERATOR *it)
{
	return bson_iter_date_time(it);
}

const char *
bsonIterKey(BSON_ITERATOR *it)
{
	return bson_iter_key(it);
}

int
bsonIterType(BSON_ITERATOR *it)
{
	return bson_iter_type(it);
}

int
bsonIterNext(BSON_ITERATOR *it)
{
	return bson_iter_next(it);
}

bool
bsonIterSubIter(BSON_ITERATOR *it, BSON_ITERATOR *sub)
{
	return bson_iter_recurse(it, sub);
}

void
bsonOidFromString(bson_oid_t *o, char *str)
{
	bson_oid_init_from_string(o, str);
}

bool
bsonAppendOid(BSON *b, const char *key, bson_oid_t *v)
{
	return bson_append_oid(b, key, strlen(key), v);
}

bool
bsonAppendBool(BSON *b, const char *key, bool v)
{
	return bson_append_bool(b, key, -1, v);
}

bool
bsonAppendStartObject(BSON *b, char *key, BSON *r)
{
	return bson_append_document_begin(b, key, strlen(key), r);
}

bool
bsonAppendFinishObject(BSON *b, BSON *r)
{
	return bson_append_document_end(b, r);
}

bool
bsonAppendNull(BSON *b, const char *key)
{
	return bson_append_null(b, key, strlen(key));
}

bool
bsonAppendInt32(BSON *b, const char *key, int v)
{
	return bson_append_int32(b, key, strlen(key), v);
}

bool
bsonAppendInt64(BSON *b, const char *key, int64_t v)
{
	return bson_append_int64(b, key, strlen(key), v);
}

bool
bsonAppendDouble(BSON *b, const char *key, double v)
{
	return bson_append_double(b, key, strlen(key), v);
}

bool
bsonAppendUTF8(BSON *b, const char *key, char *v)
{

	return bson_append_utf8(b, key, strlen(key), v, strlen(v));
}

bool
bsonAppendBinary(BSON *b, const char *key, char *v, size_t len)
{
	return bson_append_binary(b, key, (int) strlen(key), BSON_SUBTYPE_BINARY,
							  (const uint8_t *) v, len);
}

bool
bsonAppendDate(BSON *b, const char *key, time_t v)
{
	return bson_append_date_time(b, key, strlen(key), v);
}

bool
bsonAppendBson(BSON *b, char *key, BSON *c)
{
	return bson_append_document(b, key, strlen(key), c);
}

bool
bsonAppendStartArray(BSON *b, const char *key, BSON *c)
{
	return bson_append_array_begin(b, key, -1, c);
}

bool
bsonAppendFinishArray(BSON *b, BSON *c)
{
	return bson_append_array_end(b, c);
}

bool
jsonToBsonAppendElement(BSON *bb, const char *k, struct json_object *v)
{
	bool		status = true;

	if (!v)
	{
		bsonAppendNull(bb, k);
		return status;
	}

	switch (json_object_get_type(v))
	{
		case json_type_int:
			bsonAppendInt32(bb, k, json_object_get_int(v));
			break;
		case json_type_boolean:
			bsonAppendBool(bb, k, json_object_get_boolean(v));
			break;
		case json_type_double:
			bsonAppendDouble(bb, k, json_object_get_double(v));
			break;
		case json_type_string:
			bsonAppendUTF8(bb, k, (char *) json_object_get_string(v));
			break;
		case json_type_object:
			{
				BSON		t;
				struct json_object *joj;

				joj = json_object_object_get(v, "$oid");

				if (joj != NULL)
				{
					bson_oid_t	bsonObjectId;

					memset(bsonObjectId.bytes, 0, sizeof(bsonObjectId.bytes));
					bsonOidFromString(&bsonObjectId, (char *) json_object_get_string(joj));
					status = bsonAppendOid(bb, k, &bsonObjectId);
					break;
				}
				joj = json_object_object_get(v, "$date");
				if (joj != NULL)
				{
					status = bsonAppendDate(bb, k, json_object_get_int64(joj));
					break;
				}
				bsonAppendStartObject(bb, (char *) k, &t);

				{
					json_object_object_foreach(v, kk, vv)
						jsonToBsonAppendElement(&t, kk, vv);
				}
				bsonAppendFinishObject(bb, &t);
			}
			break;
		case json_type_array:
			{
				int			i;
				char		buf[10];
				BSON		t;

				bsonAppendStartArray(bb, k, &t);
				for (i = 0; i < json_object_array_length(v); i++)
				{
					sprintf(buf, "%d", i);
					jsonToBsonAppendElement(&t, buf, json_object_array_get_idx(v, i));
				}
				bsonAppendFinishObject(bb, &t);
			}
			break;
		default:
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_DATA_TYPE),
					 errmsg("can't handle type for : %s",
							json_object_to_json_string(v))));
	}

	return status;
}

json_object *
jsonTokenerPrase(char *s)
{
	return json_tokener_parse(s);
}

/*
 * mongoAggregateCount
 *		Count the number of documents.
 */
double
mongoAggregateCount(MONGO_CONN *conn, const char *database,
					const char *collection, const BSON *b)
{
	BSON	   *command;
	BSON	   *reply;
	double		count = 0;
	mongoc_cursor_t *cursor;

	command = bsonCreate();
	reply = bsonCreate();
	bsonAppendUTF8(command, "count", (char *) collection);
	if (b)						/* Not empty */
		bsonAppendBson(command, "query", (BSON *) b);

	cursor = mongoc_client_command(conn, database, MONGOC_QUERY_SLAVE_OK, 0, 1,
								   0, command, NULL, NULL);
	if (cursor)
	{
		BSON	   *doc;
		bool		ret;

		ret = mongoc_cursor_next(cursor, (const BSON **) &doc);
		if (ret)
		{
			bson_iter_t it;

			bson_copy_to(doc, reply);
			if (bson_iter_init_find(&it, reply, "n"))
				count = bsonIterDouble(&it);
		}
		mongoc_cursor_destroy(cursor);
	}
	bsonDestroy(reply);
	bsonDestroy(command);

	return count;
}

void
bsonOidToString(const bson_oid_t *o, char str[25])
{
	bson_oid_to_string(o, str);
}

const char *
bsonIterCode(BSON_ITERATOR *i)
{
	return bson_iter_code(i, NULL);
}

const char *
bsonIterRegex(BSON_ITERATOR *i)
{
	return bson_iter_regex(i, NULL);
}

const bson_value_t *
bsonIterValue(BSON_ITERATOR *i)
{
	return bson_iter_value(i);
}

void
bsonToJsonStringValue(StringInfo output, BSON_ITERATOR *iter, bool isArray)
{
	if (isArray)
		dumpJsonArray(output, iter);
	else
		dumpJsonObject(output, iter);
}

/*
 * dumpJsonObject
 *		Converts BSON document to a JSON string.
 *
 * isArray signifies if bsonData is contents of array or object.
 * [Some of] special BSON datatypes are converted to JSON using
 * "Strict MongoDB Extended JSON" [1].
 *
 * [1] http://docs.mongodb.org/manual/reference/mongodb-extended-json/
 */
void
dumpJsonObject(StringInfo output, BSON_ITERATOR *iter)
{
	uint32_t	len;
	const uint8_t *data;
	BSON 		bson;

	bson_iter_document(iter, &len, &data);
	if (bson_init_static(&bson, data, len))
	{
		char	   *json = bson_as_json(&bson, NULL);

		if (json != NULL)
		{
			appendStringInfoString(output, json);
			bson_free(json);
		}
	}
}

void
dumpJsonArray(StringInfo output, BSON_ITERATOR *iter)
{
	uint32_t	len;
	const uint8_t *data;
	BSON 		bson;

	bson_iter_array(iter, &len, &data);
	if (bson_init_static(&bson, data, len))
	{
		char	   *json;

		if ((json = bson_array_as_json(&bson, NULL)))
		{
			appendStringInfoString(output, json);
			bson_free(json);
		}
	}
}

char *
bsonAsJson(const BSON *bsonDocument)
{
	return bson_as_json(bsonDocument, NULL);
}
