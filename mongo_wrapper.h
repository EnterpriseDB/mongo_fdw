/*-------------------------------------------------------------------------
 *
 * mongo_wrapper.h
 * 		Wrapper functions for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 2004-2023, EnterpriseDB Corporation.
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		mongo_wrapper.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef MONGO_WRAPPER_H
#define MONGO_WRAPPER_H

#include "mongo_fdw.h"

#ifdef META_DRIVER
#include "mongoc.h"
#else
#include "mongo.h"
#endif
#define json_object json_object_tmp

#include <json.h>

#ifdef META_DRIVER
MONGO_CONN *mongoConnect(MongoFdwOptions *opt);
#else
MONGO_CONN *mongoConnect(MongoFdwOptions *opt);
#endif
void		mongoDisconnect(MONGO_CONN *conn);
bool		mongoInsert(MONGO_CONN *conn, char *database, char *collection,
						BSON *b);
bool		mongoUpdate(MONGO_CONN *conn, char *database, char *collection,
						BSON *b, BSON *op);
bool		mongoDelete(MONGO_CONN *conn, char *database, char *collection,
						BSON *b);
MONGO_CURSOR *mongoCursorCreate(MONGO_CONN *conn, char *database,
								char *collection, BSON *q);
const BSON *mongoCursorBson(MONGO_CURSOR *c);
bool		mongoCursorNext(MONGO_CURSOR *c, BSON *b);
void		mongoCursorDestroy(MONGO_CURSOR *c);
double		mongoAggregateCount(MONGO_CONN *conn, const char *database,
								const char *collection, const BSON *b);

BSON	   *bsonCreate(void);
void		bsonDestroy(BSON *b);

bool		bsonIterInit(BSON_ITERATOR *it, BSON *b);
bool		bsonIterSubObject(BSON_ITERATOR *it, BSON *b);
int32_t		bsonIterInt32(BSON_ITERATOR *it);
int64_t		bsonIterInt64(BSON_ITERATOR *it);
double		bsonIterDouble(BSON_ITERATOR *it);
bool		bsonIterBool(BSON_ITERATOR *it);
const char *bsonIterString(BSON_ITERATOR *it);
#ifdef META_DRIVER
const char *bsonIterBinData(BSON_ITERATOR *it, uint32_t *len);
#else
const char *bsonIterBinData(BSON_ITERATOR *it);
int			bsonIterBinLen(BSON_ITERATOR *it);
#endif
#ifdef META_DRIVER
const bson_oid_t *bsonIterOid(BSON_ITERATOR *it);
#else
bson_oid_t *bsonIterOid(BSON_ITERATOR *it);
#endif
time_t		bsonIterDate(BSON_ITERATOR *it);
int			bsonIterType(BSON_ITERATOR *it);
int			bsonIterNext(BSON_ITERATOR *it);
bool		bsonIterSubIter(BSON_ITERATOR *it, BSON_ITERATOR *sub);
void		bsonOidFromString(bson_oid_t *o, char *str);
void		bsonOidToString(const bson_oid_t *o, char str[25]);
const char *bsonIterCode(BSON_ITERATOR *i);
const char *bsonIterRegex(BSON_ITERATOR *i);
const char *bsonIterKey(BSON_ITERATOR *i);
#ifdef META_DRIVER
const bson_value_t *bsonIterValue(BSON_ITERATOR *i);
#else
const char *bsonIterValue(BSON_ITERATOR *i);
#endif

void		bsonIteratorFromBuffer(BSON_ITERATOR *i, const char *buffer);

BSON	   *bsonCreate();
bool		bsonAppendOid(BSON *b, const char *key, bson_oid_t *v);
bool		bsonAppendBool(BSON *b, const char *key, bool v);
bool		bsonAppendNull(BSON *b, const char *key);
bool		bsonAppendInt32(BSON *b, const char *key, int v);
bool		bsonAppendInt64(BSON *b, const char *key, int64_t v);
bool		bsonAppendDouble(BSON *b, const char *key, double v);
bool		bsonAppendUTF8(BSON *b, const char *key, char *v);
bool		bsonAppendBinary(BSON *b, const char *key, char *v, size_t len);
bool		bsonAppendDate(BSON *b, const char *key, time_t v);
bool		bsonAppendStartArray(BSON *b, const char *key, BSON *c);
bool		bsonAppendFinishArray(BSON *b, BSON *c);
bool		bsonAppendStartObject(BSON *b, char *key, BSON *r);
bool		bsonAppendFinishObject(BSON *b, BSON *r);
bool		bsonAppendBson(BSON *b, char *key, BSON *c);
bool		bsonFinish(BSON *b);
bool		jsonToBsonAppendElement(BSON *bb, const char *k,
									struct json_object *v);
json_object *jsonTokenerPrase(char *s);

char	   *bsonAsJson(const BSON *bsonDocument);

void		bsonToJsonStringValue(StringInfo output, BSON_ITERATOR *iter,
								  bool isArray);
void		dumpJsonObject(StringInfo output, BSON_ITERATOR *iter);
void		dumpJsonArray(StringInfo output, BSON_ITERATOR *iter);


#endif							/* MONGO_QUERY_H */
