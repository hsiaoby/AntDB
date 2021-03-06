/*-------------------------------------------------------------------------
 *
 * remotecopy.h
 *		Routines for extension of COPY command for cluster management
 *
 *
 * Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 *
 * IDENTIFICATION
 *		src/include/pgxc/remotecopy.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REMOTECOPY_H
#define REMOTECOPY_H

#include "pgxc/execRemote.h"
#include "nodes/parsenodes.h"
#include "pgxc/pgxcnode.h"

typedef struct RemoteCopyExtra
{
	FmgrInfo		   *inflinfos;
	Oid				   *typioparams;
	Datum			   *values;
	bool			   *nulls;
} RemoteCopyExtra;

/*
 * This contains the set of data necessary for remote COPY control.
 */
typedef struct RemoteCopyState {
	NodeTag				type;

	/* COPY FROM/TO? */
	bool				is_from;

	/*
	 * On Coordinator we need to rewrite query.
	 * While client may submit a copy command dealing with file, Datanodes
	 * always send/receive data to/from the Coordinator. So we can not use
	 * original statement and should rewrite statement, specifing STDIN/STDOUT
	 * as copy source or destination
	 */
	StringInfoData		query_buf;

	/* Execution nodes for COPY */
	ExecNodes		   *exec_nodes;

	/* Locator information */
	RelationLocInfo	   *rel_loc;			/* the locator key */
	int 				idx_dist_by_col;	/* index of the distributed by column */

	List			   *copy_handles;

	TupleDesc			tuple_desc;
	RemoteCopyType		remoteCopyType;		/* Type of remote COPY operation */
	RemoteCopyExtra	   *copy_extra;			/* valid if remoteCopyType is REMOTE_COPY_TUPLESTORE
											   see RemoteCopyBuildExtra. */
	FILE			   *copy_file;			/* used if remoteCopyType == REMOTE_COPY_FILE */
	uint64				processed;			/* count of data rows when running CopyOut */
	Tuplestorestate	   *tuplestorestate;
} RemoteCopyState;

/*
 * List of all the options used for query deparse step
 * As CopyStateData stays private in copy.c and in order not to
 * make Postgres-XC code too much intrusive in PostgreSQL code,
 * this intermediate structure is used primarily to generate remote
 * COPY queries based on deparsed options.
 */
typedef struct RemoteCopyOptions {
	bool		rco_binary;			/* binary format? */
	bool		rco_oids;			/* include OIDs? */
	bool		rco_csv_mode;		/* Comma Separated Value format? */
	char	   *rco_delim;			/* column delimiter (must be 1 byte) */
	char	   *rco_null_print;		/* NULL marker string (server encoding!) */
	char	   *rco_quote;			/* CSV quote char (must be 1 byte) */
	char	   *rco_escape;			/* CSV escape char (must be 1 byte) */
	List	   *rco_force_quote;	/* list of column names */
	List	   *rco_force_notnull;	/* list of column names */
} RemoteCopyOptions;

extern void RemoteCopyBuildExtra(RemoteCopyState *rcstate, TupleDesc tupdesc);
extern void RemoteCopyBuildStatement(RemoteCopyState *state,
									 Relation rel,
									 RemoteCopyOptions *options,
									 List *attnamelist,
									 List *attnums);
extern void RemoteCopyGetRelationLoc(RemoteCopyState *state,
									 Relation rel,
									 List *attnums);
extern RemoteCopyOptions *makeRemoteCopyOptions(void);
extern void FreeRemoteCopyExtra(RemoteCopyExtra *extra);
extern void FreeRemoteCopyState(RemoteCopyState *state);
extern void FreeRemoteCopyOptions(RemoteCopyOptions *options);
#endif
