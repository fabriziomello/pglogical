/*-------------------------------------------------------------------------
 *
 * pglogical_functions.c
 *		pglogical SQL visible interfaces
 *
 * Copyright (c) 2015, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		pglogical_functions.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/xact.h"
#include "access/xlog.h"

#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"

#include "commands/dbcommands.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"

#include "executor/spi.h"

#include "funcapi.h"

#include "miscadmin.h"

#include "nodes/makefuncs.h"

#include "replication/reorderbuffer.h"

#include "storage/latch.h"
#include "storage/proc.h"

#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/catcache.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/json.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "pglogical_node.h"
#include "pglogical_queue.h"
#include "pglogical_repset.h"
#include "pglogical_rpc.h"
#include "pglogical_sync.h"
#include "pglogical_worker.h"

#include "pglogical.h"

/* Node management. */
PG_FUNCTION_INFO_V1(pglogical_create_node);
PG_FUNCTION_INFO_V1(pglogical_drop_node);
PG_FUNCTION_INFO_V1(pglogical_alter_node_disable);
PG_FUNCTION_INFO_V1(pglogical_alter_node_enable);

/* Subscription management. */
PG_FUNCTION_INFO_V1(pglogical_create_subscription);
PG_FUNCTION_INFO_V1(pglogical_drop_subscription);

PG_FUNCTION_INFO_V1(pglogical_alter_subscription_disable);
PG_FUNCTION_INFO_V1(pglogical_alter_subscription_enable);

PG_FUNCTION_INFO_V1(pglogical_alter_subscriber_add_replication_set);
PG_FUNCTION_INFO_V1(pglogical_alter_subscriber_remove_replication_set);

PG_FUNCTION_INFO_V1(pglogical_alter_subscription_synchronize);
PG_FUNCTION_INFO_V1(pglogical_alter_subscription_resynchronize_table);

PG_FUNCTION_INFO_V1(pglogical_show_subscription_table);

/* Replication set manipulation. */
PG_FUNCTION_INFO_V1(pglogical_create_replication_set);
PG_FUNCTION_INFO_V1(pglogical_alter_replication_set);
PG_FUNCTION_INFO_V1(pglogical_drop_replication_set);
PG_FUNCTION_INFO_V1(pglogical_replication_set_add_table);
PG_FUNCTION_INFO_V1(pglogical_replication_set_remove_table);

/* DDL */
PG_FUNCTION_INFO_V1(pglogical_replicate_ddl_command);
PG_FUNCTION_INFO_V1(pglogical_queue_truncate);
PG_FUNCTION_INFO_V1(pglogical_truncate_trigger_add);
PG_FUNCTION_INFO_V1(pglogical_dependency_check_trigger);

/* Internal utils */
PG_FUNCTION_INFO_V1(pglogical_gen_slot_name);
PG_FUNCTION_INFO_V1(pglogical_node_info);

/*
 * Create new node
 */
Datum
pglogical_create_node(PG_FUNCTION_ARGS)
{
	PGLogicalNode	node;
	PGlogicalInterface	nodeif;
	PGLogicalRepSet		repset;

	node.id = InvalidOid;
	node.name = NameStr(*PG_GETARG_NAME(0));
	create_node(&node);

	nodeif.id = InvalidOid;
	nodeif.name = node.name;
	nodeif.nodeid = node.id;
	nodeif.dsn = text_to_cstring(PG_GETARG_TEXT_PP(1));
	create_node_interface(&nodeif);

	repset.id = InvalidOid;
	repset.nodeid = node.id;
	repset.name = DEFAULT_REPSET_NAME;
	repset.replicate_insert = true;
	repset.replicate_update = true;
	repset.replicate_delete = true;
	repset.replicate_truncate = true;
	create_replication_set(&repset);

	create_local_node(node.id, nodeif.id);

	PG_RETURN_OID(node.id);
}

/*
 * Drop the named node.
 *
 * TODO: drop subscriptions
 */
Datum
pglogical_drop_node(PG_FUNCTION_ARGS)
{
	char	   *node_name = NameStr(*PG_GETARG_NAME(0));
	bool		ifexists = PG_GETARG_BOOL(1);
	PGLogicalNode  *node;

	node = get_node_by_name(node_name, !ifexists);

	if (node != NULL)
	{
		PGLogicalLocalNode *local_node;

		/* Drop all the interfaces. */
		drop_node_interfaces(node->id);

		/* If the node is local node, drop the record as well. */
		local_node = get_local_node(true);
		if (local_node && local_node->node->id == node->id)
			drop_local_node();

		/* Drop the node itself. */
		drop_node(node->id);

		pglogical_connections_changed();
	}

	PG_RETURN_BOOL(node != NULL);
}

/*
 * Connect two existing nodes.
 */
Datum
pglogical_create_subscription(PG_FUNCTION_ARGS)
{

	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	char				   *provider_dsn = text_to_cstring(PG_GETARG_TEXT_PP(1));
	ArrayType			   *rep_set_names = PG_GETARG_ARRAYTYPE_P(2);
	bool					sync_structure = PG_GETARG_BOOL(3);
	bool					sync_data = PG_GETARG_BOOL(4);
	ArrayType			   *forward_origin_names = PG_GETARG_ARRAYTYPE_P(5);
	PGconn				   *conn;
	PGLogicalSubscription	sub;
	PGLogicalSyncStatus		sync;
	PGLogicalNode			origin;
	PGlogicalInterface		originif;
	PGLogicalLocalNode     *localnode;
	PGlogicalInterface		targetif;

	/* Check that this is actually a node. */
	localnode = get_local_node(true);

	/* Now, fetch info about remote node. */
	conn = pglogical_connect(provider_dsn, "create_subscription");
	pglogical_remote_node_info(conn, &origin.id, &origin.name, NULL, NULL, NULL);
	PQfinish(conn);

	/* Next, create local representation of remote node and interface. */
	create_node(&origin);

	originif.id = InvalidOid;
	originif.name = origin.name;
	originif.nodeid = origin.id;
	originif.dsn = provider_dsn;
	create_node_interface(&originif);

	/*
	 * Next, create subscription.
	 *
	 * Note for now we don't care much about the target interface so we fake
	 * it here to be invalid.
	 */
	targetif.id = localnode->interface->id;
	targetif.nodeid = localnode->node->id;
	sub.id = InvalidOid;
	sub.name = sub_name;
	sub.origin_if = &originif;
	sub.target_if = &targetif;
	sub.replication_sets = textarray_to_list(rep_set_names);
	sub.forward_origins = textarray_to_list(forward_origin_names);
	sub.enabled = true;
	create_subscription(&sub);

	/* Create synchronization status for the subscription. */
	if (sync_structure && sync_data)
		sync.kind = 'f';
	else if (sync_structure)
		sync.kind = 's';
	else if (sync_data)
		sync.kind = 'd';
	else
		sync.kind = 'i';

	sync.subid = sub.id;
	sync.nspname = NULL;
	sync.relname = NULL;
	sync.status = SYNC_STATUS_INIT;
	create_local_sync_status(&sync);

	pglogical_connections_changed();

	PG_RETURN_OID(sub.id);
}

/*
 * Remove subscribption.
 */
Datum
pglogical_drop_subscription(PG_FUNCTION_ARGS)
{
	char	   *sub_name = NameStr(*PG_GETARG_NAME(0));
	bool		ifexists = PG_GETARG_BOOL(1);
	PGLogicalSubscription  *sub;

	sub = get_subscription_by_name(sub_name, !ifexists);

	if (sub != NULL)
	{
		/* First drop the status. */
		drop_subscription_sync_status(sub->id);

		/* Drop the actual subscription. */
		drop_subscription(sub->id);

		/* This will make manager kill the apply worker on commit. */
		pglogical_connections_changed();
	}

	PG_RETURN_BOOL(sub != NULL);
}

/*
 * Disable subscription.
 */
Datum
pglogical_alter_subscription_disable(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	bool					immediate = PG_GETARG_BOOL(1);
	PGLogicalSubscription  *sub = get_subscription_by_name(sub_name, false);

	sub->enabled = false;

	alter_subscription(sub);

	if (immediate)
	{
		PGLogicalWorker		   *apply;

		LWLockAcquire(PGLogicalCtx->lock, LW_EXCLUSIVE);
		apply = pglogical_apply_find(MyDatabaseId, sub->id);
		if (pglogical_worker_running(apply))
			kill(apply->proc->pid, SIGTERM);
		LWLockRelease(PGLogicalCtx->lock);
	}

	PG_RETURN_BOOL(true);
}

/*
 * Enable subscriber.
 */
Datum
pglogical_alter_subscription_enable(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	bool					immediate = PG_GETARG_BOOL(1);
	PGLogicalSubscription  *sub = get_subscription_by_name(sub_name, false);

	sub->enabled = true;

	alter_subscription(sub);

	if (immediate)
	{
		PGLogicalWorker		   *manager;

		LWLockAcquire(PGLogicalCtx->lock, LW_EXCLUSIVE);
		manager = pglogical_manager_find(MyDatabaseId);
		if (pglogical_worker_running(manager))
			SetLatch(&manager->proc->procLatch);
		LWLockRelease(PGLogicalCtx->lock);
	}

	PG_RETURN_BOOL(true);
}

/*
 * Add replication set to subscriber.
 */
Datum
pglogical_alter_subscriber_add_replication_set(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	char				   *repset_name = NameStr(*PG_GETARG_NAME(1));
	PGLogicalSubscription  *sub = get_subscription_by_name(sub_name, false);
	ListCell			   *lc;
	PGLogicalWorker		   *apply;

	foreach (lc, sub->replication_sets)
	{
		char	   *rs = (char *) lfirst(lc);

		if (strcmp(rs, repset_name) == 0)
			PG_RETURN_BOOL(false);
	}

	sub->replication_sets = lappend(sub->replication_sets, repset_name);
	alter_subscription(sub);

	/* Apply as to reconnect to be able to receive new repset. */
	LWLockAcquire(PGLogicalCtx->lock, LW_EXCLUSIVE);
	apply = pglogical_apply_find(MyDatabaseId, sub->id);
	if (pglogical_worker_running(apply))
		kill(apply->proc->pid, SIGTERM);
	LWLockRelease(PGLogicalCtx->lock);

	pglogical_connections_changed();

	PG_RETURN_BOOL(true);
}

/*
 * Remove replication set to subscriber.
 */
Datum
pglogical_alter_subscriber_remove_replication_set(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	char				   *repset_name = NameStr(*PG_GETARG_NAME(1));
	PGLogicalSubscription  *sub = get_subscription_by_name(sub_name, false);
	ListCell			   *lc,
						   *next,
						   *prev;
	PGLogicalWorker		   *apply;

	prev = NULL;
	for (lc = list_head(sub->replication_sets); lc; lc = next)
	{
		char	   *rs = (char *) lfirst(lc);

		/* We might delete the cell so advance it now. */
		next = lnext(lc);

		if (strcmp(rs, repset_name) == 0)
		{
			sub->replication_sets = list_delete_cell(sub->replication_sets,
													 lc, prev);
			alter_subscription(sub);

			/* Apply as to reconnect to be able to receive new repset. */
			LWLockAcquire(PGLogicalCtx->lock, LW_EXCLUSIVE);
			apply = pglogical_apply_find(MyDatabaseId, sub->id);
			if (pglogical_worker_running(apply))
				kill(apply->proc->pid, SIGTERM);
			LWLockRelease(PGLogicalCtx->lock);

			pglogical_connections_changed();

			PG_RETURN_BOOL(true);
		}

		prev = lc;
	}

	PG_RETURN_BOOL(false);
}

/*
 * Synchronize all the missing tables.
 */
Datum
pglogical_alter_subscription_synchronize(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	bool					truncate = PG_GETARG_BOOL(1);
	PGLogicalSubscription  *sub = get_subscription_by_name(sub_name, false);
	PGconn				   *conn;
	List				   *tables;
	ListCell			   *lc;
	PGLogicalWorker		   *apply;

	/* Read table list from provider. */
	conn = pglogical_connect(sub->origin_if->dsn, "synchronize_subscription");
	tables = pg_logical_get_remote_repset_tables(conn, sub->replication_sets);
	PQfinish(conn);

	/* Compare with sync status on subscriber. And add missing ones. */
	foreach (lc, tables)
	{
		RangeVar	   *rv = (RangeVar *) lfirst(lc);
		PGLogicalSyncStatus	   *oldsync;

		oldsync = get_table_sync_status(sub->id, rv->schemaname, rv->relname,
										true);
		if (!oldsync)
		{
			PGLogicalSyncStatus	   newsync;

			newsync.kind = SYNC_KIND_DATA;
			newsync.subid = sub->id;
			newsync.nspname = rv->schemaname;
			newsync.relname = rv->relname;
			newsync.status = SYNC_STATUS_INIT;
			create_local_sync_status(&newsync);

			if (truncate)
				truncate_table(rv->schemaname, rv->relname);
		}
	}

	/* Tell apply to re-read sync statuses. */
	LWLockAcquire(PGLogicalCtx->lock, LW_EXCLUSIVE);
	apply = pglogical_apply_find(MyDatabaseId, sub->id);
	if (apply)
		apply->worker.apply.sync_pending = true;
	LWLockRelease(PGLogicalCtx->lock);

	pglogical_connections_changed();

	PG_RETURN_BOOL(true);
}

/*
 * Resyncrhonize one existing table.
 */
Datum
pglogical_alter_subscription_resynchronize_table(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	Oid						reloid = PG_GETARG_OID(1);
	PGLogicalSubscription  *sub = get_subscription_by_name(sub_name, false);
	PGLogicalSyncStatus	   *oldsync;
	PGLogicalWorker		   *apply;
	Relation				rel;
	char				   *nspname,
						   *relname;

	rel = heap_open(reloid, AccessShareLock);

	nspname = get_namespace_name(RelationGetNamespace(rel));
	relname = RelationGetRelationName(rel);

	/* Reset sync status of the table. */
	oldsync = get_table_sync_status(sub->id, nspname, relname, true);
	if (oldsync)
	{
		if (oldsync->status != SYNC_STATUS_READY &&
			oldsync->status != SYNC_STATUS_NONE)
			elog(ERROR, "table %s.%s is already being synchronized",
				 nspname, relname);

		set_table_sync_status(sub->id, nspname, relname, SYNC_STATUS_INIT);
	}
	else
	{
		PGLogicalSyncStatus	   newsync;

		newsync.kind = SYNC_KIND_DATA;
		newsync.subid = sub->id;
		newsync.nspname = nspname;
		newsync.relname = relname;
		newsync.status = SYNC_STATUS_INIT;
		create_local_sync_status(&newsync);
	}

	/* Tell apply to re-read sync statuses. */
	LWLockAcquire(PGLogicalCtx->lock, LW_EXCLUSIVE);
	apply = pglogical_apply_find(MyDatabaseId, sub->id);
	if (apply)
		apply->worker.apply.sync_pending = true;
	LWLockRelease(PGLogicalCtx->lock);

	heap_close(rel, NoLock);

	truncate_table(nspname, relname);

	pglogical_connections_changed();

	PG_RETURN_BOOL(true);
}

static char *
sync_status_to_string(char status)
{
	switch (status)
	{
		case SYNC_STATUS_INIT:
			return "sync_init";
		case SYNC_STATUS_STRUCTURE:
			return "sync_structure";
		case SYNC_STATUS_DATA:
			return "sync_data";
		case SYNC_STATUS_CONSTAINTS:
			return "sync_constraints";
		case SYNC_STATUS_SYNCWAIT:
			return "sync_waiting";
		case SYNC_STATUS_CATCHUP:
			return "catchup";
		case SYNC_STATUS_READY:
			return "syncronized";
		default:
			return "unknown";
	}
}

/*
 * Show info about one table.
 */
Datum
pglogical_show_subscription_table(PG_FUNCTION_ARGS)
{
	char				   *sub_name = NameStr(*PG_GETARG_NAME(0));
	Oid						reloid = PG_GETARG_OID(1);
	PGLogicalSubscription  *sub = get_subscription_by_name(sub_name, false);
	char				   *nspname;
	char				   *relname;
	PGLogicalSyncStatus	   *sync;
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	Datum		values[3];
	bool		nulls[3];

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* Switch into long-lived context to construct returned data structures */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	MemoryContextSwitchTo(oldcontext);

	nspname = get_namespace_name(get_rel_namespace(reloid));
	relname = get_rel_name(reloid);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	values[0] = CStringGetTextDatum(nspname);
	values[1] = CStringGetTextDatum(relname);

	/* Reset sync status of the table. */
	sync = get_table_sync_status(sub->id, nspname, relname, true);
	if (sync)
		values[2] = CStringGetTextDatum(sync_status_to_string(sync->status));
	else
		values[2] = CStringGetTextDatum("unknown");

	tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	tuplestore_donestoring(tupstore);

	PG_RETURN_VOID();
}

/*
 * Create new replication set.
 */
Datum
pglogical_create_replication_set(PG_FUNCTION_ARGS)
{
	PGLogicalRepSet		repset;
	PGLogicalLocalNode *node;

	node = get_local_node(true);
	if (!node)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("current database is not configured as pglogical node"),
				 errhint("create pglogical node first")));

	repset.id = InvalidOid;

	repset.nodeid = node->node->id;
	repset.name = NameStr(*PG_GETARG_NAME(0));

	repset.replicate_insert = PG_GETARG_BOOL(1);
	repset.replicate_update = PG_GETARG_BOOL(2);
	repset.replicate_delete = PG_GETARG_BOOL(3);
	repset.replicate_truncate = PG_GETARG_BOOL(4);

	create_replication_set(&repset);

	PG_RETURN_OID(repset.id);
}

/*
 * Alter existing replication set.
 */
Datum
pglogical_alter_replication_set(PG_FUNCTION_ARGS)
{
	PGLogicalRepSet	   *repset;
	PGLogicalLocalNode *node;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("set_name cannot be NULL")));

	node = get_local_node(true);
	if (!node)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("current database is not configured as pglogical node"),
				 errhint("create pglogical node first")));

	repset = get_replication_set_by_name(node->node->id,
										 NameStr(*PG_GETARG_NAME(0)), false);

	if (!PG_ARGISNULL(1))
		repset->replicate_insert = PG_GETARG_BOOL(1);
	if (!PG_ARGISNULL(2))
		repset->replicate_update = PG_GETARG_BOOL(2);
	if (!PG_ARGISNULL(3))
		repset->replicate_delete = PG_GETARG_BOOL(3);
	if (!PG_ARGISNULL(4))
		repset->replicate_truncate = PG_GETARG_BOOL(4);

	alter_replication_set(repset);

	PG_RETURN_OID(repset->id);
}

/*
 * Drop existing replication set.
 */
Datum
pglogical_drop_replication_set(PG_FUNCTION_ARGS)
{
	char	   *set_name = NameStr(*PG_GETARG_NAME(0));
	bool		ifexists = PG_GETARG_BOOL(1);
	PGLogicalRepSet    *repset;
	PGLogicalLocalNode *node;

	node = get_local_node(true);
	if (!node)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("current database is not configured as pglogical node"),
				 errhint("create pglogical node first")));

	repset = get_replication_set_by_name(node->node->id, set_name, !ifexists);

	if (repset != NULL)
		drop_replication_set(repset->id);

	PG_RETURN_BOOL(repset != NULL);
}

/*
 * Add replication set / relation mapping.
 */
Datum
pglogical_replication_set_add_table(PG_FUNCTION_ARGS)
{
	Name		repset_name = PG_GETARG_NAME(0);
	Oid			reloid = PG_GETARG_OID(1);
	bool		synchronize = PG_GETARG_BOOL(2);
	PGLogicalRepSet    *repset;
	Relation			rel;
	PGLogicalLocalNode *node;
	char			   *nspname;
	char			   *relname;
	StringInfoData		json;

	node = get_local_node(true);
	if (!node)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("current database is not configured as pglogical node"),
				 errhint("create pglogical node first")));

	/* Find the replication set. */
	repset = get_replication_set_by_name(node->node->id,
										 NameStr(*repset_name), false);

	/* Make sure the relation exists. */
	rel = heap_open(reloid, AccessShareLock);

	replication_set_add_table(repset->id, reloid);

	if (synchronize)
	{
		nspname = get_namespace_name(RelationGetNamespace(rel));
		relname = RelationGetRelationName(rel);

		/* It's easier to construct json manually than via Jsonb API... */
		initStringInfo(&json);
		appendStringInfo(&json, "{\"schema_name\": ");
		escape_json(&json, nspname);
		appendStringInfo(&json, ",\"table_name\": ");
		escape_json(&json, relname);
		appendStringInfo(&json, "}");

		/* Queue the truncate for replication. */
		queue_message(repset->name, GetUserId(), QUEUE_COMMAND_TYPE_TABLESYNC,
					  json.data);
	}

	/* Cleanup. */
	heap_close(rel, NoLock);

	PG_RETURN_BOOL(true);
}

/*
 * Remove replication set / relation mapping.
 *
 * Unlike the pglogical_replication_set_add_table, this function does not care
 * if table is valid or not, as we are just removing the record from repset.
 */
Datum
pglogical_replication_set_remove_table(PG_FUNCTION_ARGS)
{
	Oid			reloid = PG_GETARG_OID(1);
	PGLogicalRepSet    *repset;
	PGLogicalLocalNode *node;

	node = get_local_node(true);
	if (!node)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("current database is not configured as pglogical node"),
				 errhint("create pglogical node first")));

	/* Find the replication set. */
	repset = get_replication_set_by_name(node->node->id,
										 NameStr(*PG_GETARG_NAME(0)), false);

	replication_set_remove_table(repset->id, reloid, false);

	PG_RETURN_BOOL(true);
}

/*
 * pglogical_replicate_ddl_command
 *
 * Queues the input SQL for replication.
 */
Datum
pglogical_replicate_ddl_command(PG_FUNCTION_ARGS)
{
	text   *command = PG_GETARG_TEXT_PP(0);
	char   *query = text_to_cstring(command);
	int		save_nestlevel;
	StringInfoData	cmd;

	save_nestlevel = NewGUCNestLevel();

	/* Force everything in the query to be fully qualified. */
	(void) set_config_option("search_path", "",
							 PGC_USERSET, PGC_S_SESSION,
							 GUC_ACTION_SAVE, true, 0, false);

	/* Convert the query to json string. */
	initStringInfo(&cmd);
	escape_json(&cmd, query);

	/*
	 * Queue the query for replication.
	 *
	 * Note, we keep "DDL" message type for the future when we have deparsing
	 * support.
	 */
	queue_message("all", GetUserId(), QUEUE_COMMAND_TYPE_SQL, cmd.data);

	/* Execute the query locally. */
	pglogical_execute_sql_command(query, GetUserNameFromId(GetUserId(), false),
								  false);

	/*
	 * Restore the GUC variables we set above.
	 */
	AtEOXact_GUC(true, save_nestlevel);

	PG_RETURN_BOOL(true);
}

/*
 * pglogical_queue_trigger
 *
 * Trigger which queues the TRUNCATE command.
 *
 * XXX: There does not seem to be a way to support RESTART IDENTITY at the
 * moment.
 */
Datum
pglogical_queue_truncate(PG_FUNCTION_ARGS)
{
	TriggerData	   *trigdata = (TriggerData *) fcinfo->context;
	const char	   *funcname = "queue_truncate";
	char		   *nspname;
	char		   *relname;
	StringInfoData	json;

	/* Return if this function was called from apply process. */
	if (MyPGLogicalWorker)
		PG_RETURN_VOID();

	/* Make sure this is being called as an AFTER TRUNCTATE trigger. */
	if (!CALLED_AS_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" was not called by trigger manager",
						funcname)));

	if (!TRIGGER_FIRED_AFTER(trigdata->tg_event) ||
		!TRIGGER_FIRED_BY_TRUNCATE(trigdata->tg_event))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" must be fired AFTER TRUNCATE",
						funcname)));

	/* Format the query. */
	nspname = get_namespace_name(RelationGetNamespace(trigdata->tg_relation));
	relname = RelationGetRelationName(trigdata->tg_relation);

	/* It's easier to construct json manually than via Jsonb API... */
	initStringInfo(&json);
	appendStringInfo(&json, "{\"schema_name\": ");
	escape_json(&json, nspname);
	appendStringInfo(&json, ",\"table_name\": ");
	escape_json(&json, relname);
	appendStringInfo(&json, "}");

	/* Queue the truncate for replication. */
	queue_message("all", GetUserId(), QUEUE_COMMAND_TYPE_TRUNCATE, json.data);

	PG_RETURN_VOID();
}

/*
 * pglogical_truncate_trigger_add
 *
 * This function, which is called as an event trigger handler, adds TRUNCATE
 * trigger to newly created tables where appropriate.
 *
 * Since triggers are created tgisinternal and their creation is
 * not replicated or dumped we must create truncate triggers on
 * tables even if they're created by a replicated command or
 * restore of a dump. Recursion is not a problem since we don't
 * queue anything for replication anymore.
 */
Datum
pglogical_truncate_trigger_add(PG_FUNCTION_ARGS)
{
	EventTriggerData   *trigdata = (EventTriggerData *) fcinfo->context;
	const char	   *funcname = "truncate_trigger_add";

	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" was not called by event trigger manager",
						funcname)));

	/* Check if this is CREATE TABLE [AS] and if it is, add the trigger. */
	if (strncmp(trigdata->tag, "CREATE TABLE", strlen("CREATE TABLE")) == 0 &&
		IsA(trigdata->parsetree, CreateStmt))
	{
		CreateStmt *stmt = (CreateStmt *)trigdata->parsetree;
		char *nspname;

		/* Skip temporary and unlogged tables */
		if (stmt->relation->relpersistence != RELPERSISTENCE_PERMANENT)
			PG_RETURN_VOID();

		nspname = get_namespace_name(RangeVarGetCreationNamespace(stmt->relation));

		/*
		 * By this time the relation has been created so it's safe to
		 * call RangeVarGetRelid.
		 */
		create_truncate_trigger(nspname, stmt->relation->relname);

		pfree(nspname);
	}

	PG_RETURN_VOID();
}


/*
 * pglogical_dependency_check_trigger
 *
 * This function, which is called as an event trigger handler, does
 * our custom dependency checking.
 */
Datum
pglogical_dependency_check_trigger(PG_FUNCTION_ARGS)
{
	EventTriggerData   *trigdata = (EventTriggerData *) fcinfo->context;
	const char	   *funcname = "dependency_check_trigger";
	int				res,
					i;
	DropStmt	   *stmt;
	StringInfoData	logdetail;
	int				numDependentObjects = 0;
	PGLogicalLocalNode *node;

	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))
		ereport(ERROR,
				(errcode(ERRCODE_E_R_I_E_TRIGGER_PROTOCOL_VIOLATED),
				 errmsg("function \"%s\" was not called by event trigger manager",
						funcname)));

	/* No local node? */
	node = get_local_node(true);
	if (!node)
		PG_RETURN_VOID();

	stmt = (DropStmt *)trigdata->parsetree;
	initStringInfo(&logdetail);

	SPI_connect();

	res = SPI_execute("SELECT objid, schema_name, object_name "
					  "FROM pg_event_trigger_dropped_objects() "
					  "WHERE object_type = 'table'",
					  false, 0);
	if (res != SPI_OK_SELECT)
		elog(ERROR, "SPI query failed: %d", res);

	for (i = 0; i < SPI_processed; i++)
	{
		Oid		reloid;
		char   *schema_name;
		char   *table_name;
		bool	isnull;
		List   *repsets;

		reloid = (Oid) SPI_getbinval(SPI_tuptable->vals[i],
									 SPI_tuptable->tupdesc, 1, &isnull);
		Assert(!isnull);
		schema_name = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 2);
		table_name = SPI_getvalue(SPI_tuptable->vals[i], SPI_tuptable->tupdesc, 3);

		repsets = get_relation_replication_sets(node->node->id, reloid);

		if (list_length(repsets))
		{
			ListCell	   *lc;

			foreach (lc, repsets)
			{
				PGLogicalRepSet	   *repset = (PGLogicalRepSet *) lfirst(lc);

				if (numDependentObjects++)
					appendStringInfoString(&logdetail, "\n");
				appendStringInfo(&logdetail, "table %s.%s in replication set %s",
								 schema_name, table_name, repset->name);

				if (stmt->behavior == DROP_CASCADE)
					replication_set_remove_table(repset->id, reloid, true);
			}
		}

		drop_table_sync_status(schema_name, table_name);
	}

	SPI_finish();

	if (numDependentObjects)
	{
		if (stmt->behavior != DROP_CASCADE)
			ereport(ERROR,
					(errcode(ERRCODE_DEPENDENT_OBJECTS_STILL_EXIST),
					 errmsg("cannot drop desired object(s) because other objects depend on them"),
					 errdetail("%s", logdetail.data),
					 errhint("Use DROP ... CASCADE to drop the dependent objects too.")));
		else
			ereport(NOTICE,
					(errmsg_plural("drop cascades to %d other object",
								   "drop cascades to %d other objects",
								   numDependentObjects, numDependentObjects),
					 errdetail("%s", logdetail.data)));
	}

	PG_RETURN_VOID();
}

Datum
pglogical_node_info(PG_FUNCTION_ARGS)
{
	TupleDesc	tupdesc;
	Datum		values[5];
	bool		nulls[5];
	HeapTuple	htup;
	char		sysid[32];
	List	   *repsets;
	PGLogicalLocalNode *node;

	/* Build a tuple descriptor for our result type */
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");
	tupdesc = BlessTupleDesc(tupdesc);

	node = get_local_node(false);

	memset(nulls, 0, sizeof(nulls));

	snprintf(sysid, sizeof(sysid), UINT64_FORMAT,
			 GetSystemIdentifier());
	repsets = get_node_replication_sets(node->node->id);

	values[0] = ObjectIdGetDatum(node->node->id);
	values[1] = CStringGetTextDatum(node->node->name);
	values[2] = CStringGetTextDatum(sysid);
	values[3] = CStringGetTextDatum(get_database_name(MyDatabaseId));
	values[4] = CStringGetTextDatum(stringlist_to_identifierstr(repsets));

	htup = heap_form_tuple(tupdesc, values, nulls);

	PG_RETURN_DATUM(HeapTupleGetDatum(htup));

}

Datum
pglogical_gen_slot_name(PG_FUNCTION_ARGS)
{
	char	   *subscriber_name = NameStr(*PG_GETARG_NAME(0));
	Name		slot_name;
	PGLogicalLocalNode *node;

	node = get_local_node(false);

	slot_name = (Name) palloc0(NAMEDATALEN);

	/* This must be same as what is in subscription_fromtuple() */
	snprintf(NameStr(*slot_name), NAMEDATALEN,
		"pgl_%s_%s_%s",
		 shorten_hash(get_database_name(MyDatabaseId), 16),
		 shorten_hash(node->node->name, 16),
		 shorten_hash(subscriber_name, 16));
	NameStr(*slot_name)[NAMEDATALEN-1] = '\0';

	PG_RETURN_NAME(slot_name);
}