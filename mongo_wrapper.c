/*-------------------------------------------------------------------------
 *
 * mongo_wrapper.c
 * 		Wrapper functions for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2023, EnterpriseDB Corporation.
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		mongo_wrapper.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#ifdef META_DRIVER
#include "mongoc.h"
#else
#include "mongo.h"
#endif
#include "mongo_wrapper.h"

#define QUAL_STRING_LEN 512

MONGO_CONN *
mongoConnect(MongoFdwOptions *opt)
{
	MONGO_CONN *conn;

	conn = mongo_alloc();
	mongo_init(conn);

	if (mongo_connect(conn, opt->svr_address,
					  (int32) opt->svr_port) != MONGO_OK)
	{
		int			err = conn->err;

		mongo_destroy(conn);
		mongo_dealloc(conn);
		ereport(ERROR,
				(errmsg("could not connect to %s:%hu", opt->svr_address,
						opt->svr_port),
				 errhint("Mongo driver connection error: %d.", err)));
	}
	if (opt->svr_username && opt->svr_password)
	{
		if (mongo_cmd_authenticate(conn, opt->svr_database, opt->svr_username,
								   opt->svr_password) != MONGO_OK)
		{
			char	   *str = pstrdup(conn->errstr);

			mongo_destroy(conn);
			mongo_dealloc(conn);
			ereport(ERROR,
					(errmsg("could not connect to %s:%hu", opt->svr_address,
							opt->svr_port),
					 errhint("Mongo driver connection error: %s", str)));
		}
	}

	return conn;
}

void
mongoDisconnect(MONGO_CONN *conn)
{
	mongo_destroy(conn);
	mongo_dealloc(conn);
}

bool
mongoInsert(MONGO_CONN *conn, char *database, char *collection, bson *b)
{
	char		qual[QUAL_STRING_LEN];

	snprintf(qual, QUAL_STRING_LEN, "%s.%s", database, collection);
	if (mongo_insert(conn, qual, b, NULL) != MONGO_OK)
		ereport(ERROR,
				(errmsg("failed to insert row"),
				 errhint("Mongo driver insert error: %d.", conn->err)));

	return true;
}

bool
mongoUpdate(MONGO_CONN *conn, char *database, char *collection, BSON *b,
			BSON *op)
{
	char		qual[QUAL_STRING_LEN];

	snprintf(qual, QUAL_STRING_LEN, "%s.%s", database, collection);
	if (mongo_update(conn, qual, b, op, MONGO_UPDATE_BASIC, 0) != MONGO_OK)
		ereport(ERROR,
				(errmsg("failed to update row"),
				 errhint("Mongo driver update error: %d.", conn->err)));

	return true;
}

bool
mongoDelete(MONGO_CONN *conn, char *database, char *collection, BSON *b)
{
	char		qual[QUAL_STRING_LEN];

	snprintf(qual, QUAL_STRING_LEN, "%s.%s", database, collection);
	if (mongo_remove(conn, qual, b, NULL) != MONGO_OK)
		ereport(ERROR,
				(errmsg("failed to delete row"),
				 errhint("Mongo driver delete error: %d.", conn->err)));

	return true;
}

MONGO_CURSOR *
mongoCursorCreate(MONGO_CONN *conn, char *database, char *collection, BSON *q)
{
	MONGO_CURSOR *c;
	char		qual[QUAL_STRING_LEN];

	snprintf(qual, QUAL_STRING_LEN, "%s.%s", database, collection);
	c = mongo_cursor_alloc();
	mongo_cursor_init(c, conn, qual);
	mongo_cursor_set_query(c, q);

	return c;
}

const bson *
mongoCursorBson(MONGO_CURSOR *c)
{
	return mongo_cursor_bson(c);
}

bool
mongoCursorNext(MONGO_CURSOR *c, BSON *b)
{
	return (mongo_cursor_next(c) == MONGO_OK);
}

void
mongoCursorDestroy(MONGO_CURSOR *c)
{
	mongo_cursor_destroy(c);
	mongo_cursor_dealloc(c);
}

BSON *
bsonCreate()
{
	BSON	   *doc = NULL;

	doc = bson_alloc();
	bson_init(doc);

	return doc;
}

void
bsonDestroy(BSON *b)
{
	bson_destroy(b);
	bson_dealloc(b);
}

bool
bsonIterInit(BSON_ITERATOR *it, BSON *b)
{
	bson_iterator_init(it, b);

	return true;
}

bool
bsonIterSubObject(BSON_ITERATOR *it, BSON *b)
{
	bson_iterator_subobject_init(it, b, 0);

	return true;
}

int32_t
bsonIterInt32(BSON_ITERATOR *it)
{
	switch (bson_iterator_type(it))
	{
		case BSON_DOUBLE:
			{
				double		val = bson_iterator_double_raw(it);

				if (val < PG_INT32_MIN || val > PG_INT32_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("value \"%f\" is out of range for type integer",
									val)));

				return (int32) val;
			}
		case BSON_LONG:
			{
				int64		val = bson_iterator_long_raw(it);

				if (val < PG_INT32_MIN || val > PG_INT32_MAX)
					ereport(ERROR,
							(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
							 errmsg("value \"%ld\" is out of range for type integer",
									val)));

				return (int32) val;
			}
		case BSON_INT:
			return bson_iterator_int_raw(it);
		default:
			return 0;
	}
}

int64_t
bsonIterInt64(BSON_ITERATOR *it)
{
	return bson_iterator_long(it);
}

double
bsonIterDouble(BSON_ITERATOR *it)
{
	return bson_iterator_double(it);
}

bool
bsonIterBool(BSON_ITERATOR *it)
{
	return bson_iterator_bool(it);
}

const char *
bsonIterString(BSON_ITERATOR *it)
{
	return bson_iterator_string(it);
}

const char *
bsonIterBinData(BSON_ITERATOR *it)
{
	return bson_iterator_bin_data(it);
}

int
bsonIterBinLen(BSON_ITERATOR *it)
{
	return bson_iterator_bin_len(it);
}

bson_oid_t *
bsonIterOid(BSON_ITERATOR *it)
{
	return bson_iterator_oid(it);
}

time_t
bsonIterDate(BSON_ITERATOR *it)
{
	return bson_iterator_date(it);
}

int
bsonIterType(BSON_ITERATOR *it)
{
	return bson_iterator_type(it);
}

int
bsonIterNext(BSON_ITERATOR *it)
{
	return bson_iterator_next(it);
}

bool
bsonIterSubIter(BSON_ITERATOR *it, BSON_ITERATOR *sub)
{
	bson_iterator_subiterator(it, sub);

	return true;
}

void
bsonOidFromString(bson_oid_t *o, char *str)
{
	bson_oid_from_string(o, str);
}

bool
bsonAppendOid(BSON *b, const char *key, bson_oid_t *v)
{
	return (bson_append_oid(b, key, v) == MONGO_OK);
}

bool
bsonAppendBool(BSON *b, const char *key, bool v)
{
	return (bson_append_int(b, key, v) == MONGO_OK);
}

bool
bsonAppendNull(BSON *b, const char *key)
{
	return (bson_append_null(b, key) == MONGO_OK);
}

bool
bsonAppendInt32(BSON *b, const char *key, int v)
{
	return (bson_append_int(b, key, v) == MONGO_OK);
}

bool
bsonAppendInt64(BSON *b, const char *key, int64_t v)
{
	return (bson_append_long(b, key, v) == MONGO_OK);
}

bool
bsonAppendDouble(BSON *b, const char *key, double v)
{
	return (bson_append_double(b, key, v) == MONGO_OK);
}

bool
bsonAppendUTF8(BSON *b, const char *key, char *v)
{
	return (bson_append_string(b, key, v) == MONGO_OK);
}

bool
bsonAppendBinary(BSON *b, const char *key, char *v, size_t len)
{
	return (bson_append_binary(b, key, BSON_BIN_BINARY, v, len) == MONGO_OK);
}
bool
bsonAppendDate(BSON *b, const char *key, time_t v)
{
	return (bson_append_date(b, key, v) == MONGO_OK);
}

bool
bsonAppendStartArray(BSON *b, const char *key, BSON *c)
{
	return (bson_append_start_array(b, key) == MONGO_OK);
}

bool
bsonAppendFinishArray(BSON *b, BSON *c)
{
	return (bson_append_finish_array(b) == MONGO_OK);
}

bool
bsonAppendStartObject(BSON *b, char *key, BSON *r)
{
	return (bson_append_start_object(b, key) == MONGO_OK);
}

bool
bsonAppendFinishObject(BSON *b, BSON *r)
{
	return (bson_append_finish_object(b) == MONGO_OK);
}

bool
bsonAppendBson(BSON *b, char *key, BSON *c)
{
	return (bson_append_bson(b, key, c) == MONGO_OK);
}

bool
bsonFinish(BSON *b)
{
	return (bson_finish(b) == MONGO_OK);
}

json_object *
jsonTokenerPrase(char *s)
{
	return json_tokener_parse(s);
}

bool
jsonToBsonAppendElement(BSON *bb, const char *k, struct json_object *v)
{
	bool		status = true;

	if (!v)
	{
		bson_append_null(bb, k);
		return status;
	}

	switch (json_object_get_type(v))
	{
		case json_type_int:
			bson_append_int(bb, k, json_object_get_int(v));
			break;
		case json_type_boolean:
			bson_append_bool(bb, k, json_object_get_boolean(v));
			break;
		case json_type_double:
			bson_append_double(bb, k, json_object_get_double(v));
			break;
		case json_type_string:
			bson_append_string(bb, k, json_object_get_string(v));
			break;
		case json_type_object:
			{
				struct json_object *joj;

				joj = json_object_object_get(v, "$oid");

				if (joj != NULL)
				{
					bson_oid_t	bsonObjectId;

					memset(bsonObjectId.bytes, 0, sizeof(bsonObjectId.bytes));
					bsonOidFromString(&bsonObjectId,
									  (char *) json_object_get_string(joj));
					status = bsonAppendOid(bb, k, &bsonObjectId);
					break;
				}
				joj = json_object_object_get(v, "$date");
				if (joj != NULL)
				{
					status = bsonAppendDate(bb, k, json_object_get_int64(joj));
					break;
				}

				bson_append_start_object(bb, k);

				{
					json_object_object_foreach(v, kk, vv)
						jsonToBsonAppendElement(bb, kk, vv);
				}
				bson_append_finish_object(bb);
			}
			break;
		case json_type_array:
			{
				int			i;
				char		buf[10];

				bson_append_start_array(bb, k);
				for (i = 0; i < json_object_array_length(v); i++)
				{
					sprintf(buf, "%d", i);
					jsonToBsonAppendElement(bb, buf,
											json_object_array_get_idx(v, i));
				}
				bson_append_finish_object(bb);
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

double
mongoAggregateCount(MONGO_CONN *conn, const char *database,
					const char *collection, const BSON *b)
{
	return mongo_count(conn, database, collection, b);
}

void
bsonIteratorFromBuffer(BSON_ITERATOR *i, const char *buffer)
{
	bson_iterator_from_buffer(i, buffer);
}

void
bsonOidToString(const bson_oid_t *o, char str[25])
{
	bson_oid_to_string(o, str);
}

const char *
bsonIterCode(BSON_ITERATOR *i)
{
	return bson_iterator_code(i);
}

const char *
bsonIterRegex(BSON_ITERATOR *i)
{
	return bson_iterator_regex(i);
}

const char *
bsonIterKey(BSON_ITERATOR *i)
{
	return bson_iterator_key(i);
}

const char *
bsonIterValue(BSON_ITERATOR *i)
{
	return bson_iterator_value(i);
}

char *
bsonAsJson(const BSON *bsonDocument)
{
	ereport(ERROR,
			(errmsg("full document retrival only available in MongoC meta driver")));
}
