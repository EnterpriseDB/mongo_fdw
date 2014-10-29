/*-------------------------------------------------------------------------
 *
 * mongo_wrapper_meta.c
 * 		Foreign-data wrapper for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2004-2014, EnterpriseDB Corporation.
 *
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		mongo_wrapper_meta.c
 *
 *-------------------------------------------------------------------------
 */


#include "postgres.h"
#include <mongoc.h>

#include "mongo_wrapper.h"

/*
 * Connect to MongoDB server using Host/ip and Port number.
 */
MONGO_CONN*
MongoConnect(const char* host, const unsigned short port, char* databaseName, char *user, char *password, char *readPreference)
{
	MONGO_CONN *client = NULL;
	char* uri = NULL;

	if (user && password && readPreference)
		uri = bson_strdup_printf ("mongodb://%s:%s@%s:%hu/?readPreference=%s", user, password, host, port, readPreference);
	else if (user && password)
		uri = bson_strdup_printf ("mongodb://%s:%s@%s:%hu/", user, password, host, port);
	else if (readPreference)
		uri = bson_strdup_printf ("mongodb://%s:%hu/?readPreference=%s", host, port, readPreference);
	else
		uri = bson_strdup_printf ("mongodb://%s:%hu/", host, port);

	client = mongoc_client_new(uri);

	bson_free(uri);

	if (client == NULL)
		ereport(ERROR, (errmsg("could not connect to %s:%d", host, port),
						errhint("Mongo driver connection error")));
	return client;
}

/*
 * Disconnect from MongoDB server.
 */
void
MongoDisconnect(MONGO_CONN* conn)
{
	if (conn)
		mongoc_client_destroy(conn);
}


/*
 * Insert a document 'b' into MongoDB.
 */
bool
MongoInsert(MONGO_CONN* conn, char *database, char* collection, BSON* b)
{
	mongoc_collection_t *c = NULL;
	bson_error_t error;
	bool r = false;

	c = mongoc_client_get_collection(conn, database, collection);

	r = mongoc_collection_insert(c, MONGOC_INSERT_NONE, b, NULL, &error);
	mongoc_collection_destroy(c);
	if (!r)
		ereport(ERROR, (errmsg("failed to insert row"),
						errhint("Mongo error: \"%s\"", error.message)));
	return true;
}


/*
 * Update a document 'b' into MongoDB.
 */
bool
MongoUpdate(MONGO_CONN* conn, char* database, char *collection, BSON* b, BSON* op)
{
	mongoc_collection_t *c = NULL;
	bson_error_t error;
	bool r = false;

	c = mongoc_client_get_collection (conn, database, collection);

	r = mongoc_collection_update(c, MONGOC_UPDATE_NONE, b, op, NULL, &error);
	mongoc_collection_destroy(c);
	if (!r)
		ereport(ERROR, (errmsg("failed to update row"),
						errhint("Mongo error: \"%s\"", error.message)));
	return true;
}


/*
 * Delete MongoDB's document.
 */
bool
MongoDelete(MONGO_CONN* conn, char* database, char *collection, BSON* b)
{
	mongoc_collection_t *c = NULL;
	bson_error_t error;
	bool r = false;

	c = mongoc_client_get_collection (conn, database, collection);

	r = mongoc_collection_delete(c, MONGOC_DELETE_SINGLE_REMOVE, b, NULL, &error);
	mongoc_collection_destroy(c);
	if (!r)
		ereport(ERROR, (errmsg("failed to delete row"),
						errhint("Mongo error: \"%s\"", error.message)));
	return true;
}

/*
 * Performs a query against the configured MongoDB server and return
 * cursor which can be destroyed by calling mongoc_cursor_current.
 */
MONGO_CURSOR*
MongoCursorCreate(MONGO_CONN* conn, char* database, char *collection, BSON* q)
{
	mongoc_collection_t *c = NULL;
	MONGO_CURSOR *cur = NULL;
	bson_error_t error;

	c = mongoc_client_get_collection (conn, database, collection);
	cur = mongoc_collection_find(c, MONGOC_QUERY_SLAVE_OK, 0, 0, 0, q, NULL, NULL);
	mongoc_cursor_error(cur, &error);
	if (!cur)
		ereport(ERROR, (errmsg("failed to create cursor"),
						errhint("Mongo error: \"%s\"", error.message)));

	mongoc_collection_destroy(c);
	return cur;
}


/*
 * Destroy cursor created by calling MongoCursorCreate function.
 */
void
MongoCursorDestroy(MONGO_CURSOR* c)
{
	mongoc_cursor_destroy(c);
}


/*
 * Get the current document from cursor.
 */
const BSON*
MongoCursorBson(MONGO_CURSOR* c)
{
	return mongoc_cursor_current(c);
}

/*
 * Get the next document from the cursor.
 */
bool
MongoCursorNext(MONGO_CURSOR* c, BSON *b)
{
	return mongoc_cursor_next(c, (const BSON**) &b);
}


/*
 * Allocates a new bson_t structure, and also initialize the bson
 * object. After that point objects can be appended to that bson
 * object and can be iterated. A newly allocated bson_t that should
 * be freed with bson_destroy().
 */
BSON*
BsonCreate(void)
{
	BSON *b = NULL;
	b = bson_new();
	bson_init(b);
	return b;
}

/*
 * Destroy Bson objected created by BsonCreate function.
 */
void
BsonDestroy(BSON *b)
{
	bson_destroy(b);
}


/*
 * Initialize the bson Iterator.
 */
bool
BsonIterInit(BSON_ITERATOR *it, BSON *b)
{
	return bson_iter_init(it, b);
}


bool
BsonIterSubObject(BSON_ITERATOR *it, BSON *b)
{
	/* TODO: Need to see the Meta Driver equalient for "bson_iterator_subobject" */
	return true;
}

int32_t
BsonIterInt32(BSON_ITERATOR *it)
{
	return bson_iter_int32(it);
}


int64_t
BsonIterInt64(BSON_ITERATOR *it)
{
	return bson_iter_int64(it);
}


double
BsonIterDouble(BSON_ITERATOR *it)
{
	return bson_iter_double(it);
}


bool
BsonIterBool(BSON_ITERATOR *it)
{
	return bson_iter_bool(it);
}


const char*
BsonIterString(BSON_ITERATOR *it)
{
	uint32_t len = 0;
	return bson_iter_utf8(it, &len);
}

const char*
BsonIterBinData(BSON_ITERATOR *it, uint32_t *len)
{
	const uint8_t *binary = NULL;
	bson_subtype_t subtype = BSON_SUBTYPE_BINARY;
	bson_iter_binary (it, &subtype, &len, &binary);
	return (char*)binary;
}

const bson_oid_t *
BsonIterOid(BSON_ITERATOR *it)
{
	return bson_iter_oid(it);
}


time_t
BsonIterDate(BSON_ITERATOR *it)
{
	return bson_iter_date_time(it);
}


const char*
BsonIterKey(BSON_ITERATOR *it)
{
	return bson_iter_key(it);
}

int
BsonIterType(BSON_ITERATOR *it)
{
	return bson_iter_type(it);
}

int
BsonIterNext(BSON_ITERATOR *it)
{
	return bson_iter_next(it);
}


bool
BsonIterSubIter(BSON_ITERATOR *it, BSON_ITERATOR* sub)
{
	return bson_iter_recurse(it, sub);
}


void
BsonOidFromString(bson_oid_t *o, char* str)
{
	bson_oid_init_from_string(o, str);
}


bool
BsonAppendOid(BSON *b, const char* key, bson_oid_t *v)
{
	return bson_append_oid(b, key, strlen(key), v);
}

bool
BsonAppendBool(BSON *b, const char* key, bool v)
{
	return bson_append_bool(b, key, -1, v);
}

bool
BsonAppendStartObject(BSON* b, char *key, BSON* r)
{
	return bson_append_document_begin(b, key, strlen(key), r);
}


bool
BsonAppendFinishObject(BSON* b, BSON* r)
{
	return bson_append_document_end(b, r);
}


bool
BsonAppendNull(BSON *b, const char* key)
{
	return bson_append_null(b, key, strlen(key));
}


bool
BsonAppendInt32(BSON *b, const char* key, int v)
{
	return bson_append_int32(b, key, strlen(key), v);
}


bool
BsonAppendInt64(BSON *b, const char* key, int64_t v)
{
	return bson_append_int64(b, key, strlen(key), v);
}

bool
BsonAppendDouble(BSON *b, const char* key, double v)
{
	return bson_append_double(b, key, strlen(key), v);
}

bool
BsonAppendUTF8(BSON *b, const char* key, char *v)
{
  
	return bson_append_utf8(b, key, strlen(key), v, strlen(v));
}

bool
BsonAppendBinary(BSON *b, const char* key, char *v, size_t len)
{
	return bson_append_binary(b, key, (int)strlen(key), BSON_SUBTYPE_BINARY, v, len);
}

bool
BsonAppendDate(BSON *b, const char* key, time_t v)
{
	return bson_append_date_time(b, key, strlen(key), v);
}


bool
BsonAppendBson(BSON* b, char *key, BSON* c)
{
	return bson_append_document(b, key, strlen(key), c);
}

bool BsonAppendStartArray(BSON *b, const char* key, BSON* c)
{
    return bson_append_array_begin(b, key, -1, c);
}


bool BsonAppendFinishArray(BSON *b, BSON* c)
{
    return bson_append_array_end(b, c);
}


bool
BsonFinish(BSON* b)
{
	/*
	 * There is no need for bson_finish in Meta Driver.
	 * We are doing nothing, just because of compatiblity with legacy
	 * driver.
	 */
	return true;
}

/*
 * Count the number of documents.
 */
double
MongoAggregateCount(MONGO_CONN* conn, const char* database, const char* collection, const BSON* b)
{
	const BSON *command;
	BSON *reply;
	const BSON *doc;
	double count;
	mongoc_cursor_t *cursor;
	bool ret;

	command = BsonCreate();
	reply = BsonCreate();
	BsonAppendUTF8(command, "count", (char*)collection);
	if (b) /* not empty */
		BsonAppendBson(command, "query", (BSON*)b);

	BsonFinish(command);

	cursor = mongoc_client_command(conn, database, MONGOC_QUERY_SLAVE_OK, 0, 1, 0, command, NULL, NULL);

	ret = mongoc_cursor_next(cursor, &doc);

	if (ret) {
		bson_copy_to(doc, reply);
		bson_iter_t it;
		if (bson_iter_init_find(&it, reply, "n"))
			count = BsonIterDouble(&it);
	} else {
		count = -1;
	}

	mongoc_cursor_destroy(cursor);

	BsonDestroy(reply);
	BsonDestroy(doc);
	BsonDestroy(command);

	return count;
}
