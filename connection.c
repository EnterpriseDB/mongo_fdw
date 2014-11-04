/*-------------------------------------------------------------------------
 *
 * connection.c
 * 		Foreign-data wrapper for remote MongoDB servers
 *
 * Portions Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 * Portions Copyright (c) 2004-2014, EnterpriseDB Corporation.
 *
 * Portions Copyright (c) 2012–2014 Citus Data, Inc.
 *
 * IDENTIFICATION
 * 		connection.c
 *
 *-------------------------------------------------------------------------
 */
#include <sys/types.h>


#include "postgres.h"
#include "mongo_wrapper.h"
#include "mongo_fdw.h"

#include "access/xact.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

/* Length of host */
#define HOST_LEN 256

/*
 * Connection cache hash table entry
 *
 * The lookup key in this hash table is the foreign server OID plus the user
 * mapping OID.  (We use just one connection per user per foreign server,
 * so that we can ensure all scans use the same snapshot during a query.)
 */
typedef struct ConnCacheKey
{
	Oid			serverid;		/* OID of foreign server */
	Oid			userid;			/* OID of local user whose mapping we use */
} ConnCacheKey;

typedef struct ConnCacheEntry
{
	ConnCacheKey key;		/* hash key (must be first) */
	MONGO_CONN *conn;		/* connection to foreign server, or NULL */
} ConnCacheEntry;

/*
 * Connection cache (initialized on first use)
 */
static HTAB *ConnectionHash = NULL;

/*
 * mongo_get_connection:
 * 			Get a mong connection which can be used to execute queries on
 * the remote Mongo server with the user's authorization. A new connection
 * is established if we don't already have a suitable one.
 */
MONGO_CONN*
mongo_get_connection(ForeignServer *server, UserMapping *user, MongoFdwOptions *opt)
{
	bool            found;
	ConnCacheEntry  *entry;
	ConnCacheKey    key;

	/* First time through, initialize connection cache hashtable */
	if (ConnectionHash == NULL)
	{
		HASHCTL	ctl;
		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		ctl.hash = tag_hash;
		/* allocate ConnectionHash in the cache context */
		ctl.hcxt = CacheMemoryContext;
		ConnectionHash = hash_create("mongo_fdw connections", 8,
							&ctl,
							HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);
	}

	/* Create hash key for the entry.  Assume no pad bytes in key struct */
	key.serverid = server->serverid;
	key.userid = user->userid;

	/*
	 * Find or create cached entry for requested connection.
	 */
	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);
	if (!found)
	{
		/* initialize new hashtable entry (key is already filled in) */
		entry->conn = NULL;
	}
	if (entry->conn == NULL)
	{
#ifdef META_DRIVER
		entry->conn = MongoConnect(opt->svr_address, opt->svr_port, opt->svr_database, opt->svr_username, opt->svr_password, opt->readPreference);
#else
		entry->conn = MongoConnect(opt->svr_address, opt->svr_port, opt->svr_database, opt->svr_username, opt->svr_password);
#endif
		elog(DEBUG3, "new mongo_fdw connection %p for server \"%s:%d\"",
			 entry->conn, opt->svr_address, opt->svr_port);
	}

	return entry->conn;
}

/*
 * mongo_cleanup_connection:
 * Delete all the cache entries on backend exists.
 */
void
mongo_cleanup_connection()
{
	HASH_SEQ_STATUS	scan;
	ConnCacheEntry *entry;

	if (ConnectionHash == NULL)
		return;

	hash_seq_init(&scan, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&scan)))
	{
		if (entry->conn == NULL)
			continue;

		elog(DEBUG3, "disconnecting mongo_fdw connection %p", entry->conn);
		MongoDisconnect(entry->conn);
		entry->conn = NULL;
	}
}

/*
 * Release connection created by calling mongo_get_connection.
 */
void
mongo_release_connection(MONGO_CONN *conn)
{
	/*
	 * We don't close the connection indvisually  here, will do all connection
	 * cleanup on the backend exit.
	 */
}

