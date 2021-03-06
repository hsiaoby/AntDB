#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/heap.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_aux_class.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "nodes/makefuncs.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"

extern bool enable_aux_dml;

/*
 * InsertAuxClassTuple
 *
 * add record for pg_aux_class
 */
void
InsertAuxClassTuple(Oid auxrelid, Oid relid, AttrNumber attnum)
{
	Relation		auxrelation;
	HeapTuple		tuple;
	Datum			values[Natts_pg_aux_class];
	bool			nulls[Natts_pg_aux_class];
	ObjectAddress	myself,
					referenced;

	/* Sanity check */
	Assert(OidIsValid(auxrelid));
	Assert(OidIsValid(relid));
	Assert(AttrNumberIsForUserDefinedAttr(attnum));

	MemSet(nulls, 0, sizeof(nulls));
	MemSet(values, 0, sizeof(values));

	values[Anum_pg_aux_class_auxrelid - 1] = ObjectIdGetDatum(auxrelid);
	values[Anum_pg_aux_class_relid - 1] = ObjectIdGetDatum(relid);
	values[Anum_pg_aux_class_attnum - 1] = Int16GetDatum(attnum);

	auxrelation = heap_open(AuxClassRelationId, RowExclusiveLock);
	tuple = heap_form_tuple(RelationGetDescr(auxrelation), values, nulls);
	simple_heap_insert(auxrelation, tuple);
	/* keep the catalog indexes up to date */
	CatalogUpdateIndexes(auxrelation, tuple);
	heap_freetuple(tuple);
	heap_close(auxrelation, RowExclusiveLock);

	/* reference object address */
	ObjectAddressSubSet(referenced, RelationRelationId, relid, attnum);

	/* Make pg_aux_class object depend entry */
	ObjectAddressSet(myself, AuxClassRelationId, auxrelid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_AUTO);

	/* Make pg_class object depend entry */
	ObjectAddressSet(myself, RelationRelationId, auxrelid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
}

/*
 * RemoveAuxClassTuple
 *
 * remove record from pg_aux_class by "auxrelid" if valid
 * or "auxrelid" and "attnum".
 */
void
RemoveAuxClassTuple(Oid auxrelid, Oid relid, AttrNumber attnum)
{
	Relation	auxrelation;
	HeapTuple	tuple;

	auxrelation = heap_open(AuxClassRelationId, RowExclusiveLock);

	if (OidIsValid(auxrelid))
		tuple = SearchSysCache1(AUXCLASSIDENT,
								ObjectIdGetDatum(auxrelid));
	else
		tuple = SearchSysCache2(AUXCLASSRELIDATT,
								ObjectIdGetDatum(relid),
								Int16GetDatum(attnum));

	if (!HeapTupleIsValid(tuple))
		return ;

	simple_heap_delete(auxrelation, &(tuple->t_self));
	ReleaseSysCache(tuple);
	heap_close(auxrelation, RowExclusiveLock);
}

/*
 * LookupAuxRelation
 *
 * find out the auxiliary relation by the Oid of master
 * relation and relevant attribute number.
 */
Oid
LookupAuxRelation(Oid relid, AttrNumber attnum)
{
	HeapTuple				tuple;
	Form_pg_aux_class		auxtup;
	Oid						auxrelid;

	/*
	 * Sanity check
	 */
	if (!OidIsValid(relid) ||
		!AttrNumberIsForUserDefinedAttr(attnum))
		return InvalidOid;

	tuple = SearchSysCache2(AUXCLASSRELIDATT,
							ObjectIdGetDatum(relid),
							Int16GetDatum(attnum));
	if (!HeapTupleIsValid(tuple))
		return InvalidOid;

	auxtup = (Form_pg_aux_class) GETSTRUCT(tuple);
	auxrelid = auxtup->auxrelid;

	ReleaseSysCache(tuple);

	return auxrelid;
}

/*
 * LookupAuxMasterRel
 *
 * find out Oid of the master relation by the specified
 * Oid of auxiliary relation.
 */
Oid
LookupAuxMasterRel(Oid auxrelid, AttrNumber *attnum)
{
	HeapTuple				tuple;
	Form_pg_aux_class		auxtup;
	Oid						master_relid;

	if (attnum)
		*attnum = InvalidAttrNumber;

	if (!OidIsValid(auxrelid))
		return InvalidOid;

	tuple = SearchSysCache1(AUXCLASSIDENT,
							ObjectIdGetDatum(auxrelid));
	if (!HeapTupleIsValid(tuple))
		return InvalidOid;

	auxtup = (Form_pg_aux_class) GETSTRUCT(tuple);
	master_relid = auxtup->relid;
	if (attnum)
		*attnum = auxtup->attnum;

	Assert(AttributeNumberIsValid(auxtup->attnum));

	ReleaseSysCache(tuple);

	return master_relid;
}

/*
 * RelationIdGetAuxAttnum
 *
 * is it an auxiliary relation? retrun auxiliary attribute
 * number if true.
 */
bool
RelationIdGetAuxAttnum(Oid auxrelid, AttrNumber *attnum)
{
	HeapTuple			tuple;
	Form_pg_aux_class	auxtup;

	if (attnum)
		*attnum = InvalidAttrNumber;

	if (!OidIsValid(auxrelid))
		return false;

	tuple = SearchSysCache1(AUXCLASSIDENT,
							ObjectIdGetDatum(auxrelid));
	if (!HeapTupleIsValid(tuple))
		return false;

	auxtup = (Form_pg_aux_class) GETSTRUCT(tuple);
	if (attnum)
		*attnum = auxtup->attnum;

	Assert(AttributeNumberIsValid(auxtup->attnum));

	ReleaseSysCache(tuple);

	return true;
}

static char *
ChooseAuxTableName(const char *name1, const char *name2,
				   const char *label, Oid namespaceid)
{
	int 		pass = 0;
	char	   *relname = NULL;
	char		modlabel[NAMEDATALEN];

	/* try the unmodified label first */
	StrNCpy(modlabel, label, sizeof(modlabel));

	for (;;)
	{
		relname = makeObjectName(name1, name2, modlabel);

		if (!OidIsValid(get_relname_relid(relname, namespaceid)))
			break;

		/* found a conflict, so try a new name component */
		pfree(relname);
		snprintf(modlabel, sizeof(modlabel), "%s%d", label, ++pass);
	}

	return relname;
}

bool HasAuxRelation(Oid relid)
{
	HeapTuple		tuple;
	ScanKeyData		skey;
	SysScanDesc		auxscan;
	Relation		auxrel;
	bool			result;

	if (relid < FirstNormalObjectId)
		return false;

	ScanKeyInit(&skey,
				Anum_pg_aux_class_relid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(relid));

	auxrel = heap_open(AuxClassRelationId, AccessShareLock);
	auxscan = systable_beginscan(auxrel,
								 AuxClassRelidAttnumIndexId,
								 true,
								 NULL,
								 1,
								 &skey);
	tuple = systable_getnext(auxscan);
	result = HeapTupleIsValid(tuple);
	systable_endscan(auxscan);
	heap_close(auxrel, AccessShareLock);

	return result;
}

static List *
MakeAuxTableColumns(Form_pg_attribute auxcolumn, Relation rel, AttrNumber *distattnum)
{
	ColumnDef		   *coldef;
	List			   *tableElts = NIL;
	RelationLocInfo	   *loc;
	AttrNumber			attnum;			/* distribute column attrnum */
	Form_pg_attribute	discolumn;

	Assert(auxcolumn && rel);

	/* 1. auxiliary column */
	coldef = makeColumnDef(NameStr(auxcolumn->attname),
						   auxcolumn->atttypid,
						   auxcolumn->atttypmod,
						   auxcolumn->attcollation);
	tableElts = lappend(tableElts, coldef);

	/* 2. distribute column */
	loc = RelationGetLocInfo(rel);
	if (IsRelationDistributedByValue(loc))
	{
		attnum = loc->partAttrNum;
	} else
	if (IsRelationDistributedByUserDefined(loc))
	{
		Assert(list_length(loc->funcAttrNums) == 1);
		attnum = linitial_int(loc->funcAttrNums);
	} else
	{
		/* should not reach here */
		attnum = InvalidAttrNumber;
	}
	Assert(AttrNumberIsForUserDefinedAttr(attnum));
	if (distattnum)
		*distattnum = attnum;
	discolumn = rel->rd_att->attrs[attnum - 1];
	coldef = makeColumnDef(NameStr(discolumn->attname),
						   discolumn->atttypid,
						   discolumn->atttypmod,
						   discolumn->attcollation);
	tableElts = lappend(tableElts, coldef);

	/* 3. additional fixed columns -- auxnodeid */
	coldef = makeColumnDef("auxnodeid",
						   INT4OID,
						   -1,
						   0);
	tableElts = lappend(tableElts, coldef);

	/* 4. additional fixed columns -- auxctid */
	coldef = makeColumnDef("auxctid",
						   TIDOID,
						   -1,
						   0);
	tableElts = lappend(tableElts, coldef);

	return tableElts;
}

/*
 * QueryRewriteAuxStmt
 *
 * rewrite auxiliary query.
 */
List *
QueryRewriteAuxStmt(Query *auxquery)
{
	CreateAuxStmt	   *auxstmt;
	CreateStmt		   *create_stmt;
	IndexStmt		   *index_stmt;
	HeapTuple			atttuple;
	Form_pg_attribute	auxattform;
	Form_pg_attribute	disattform;
	AttrNumber			distattnum;
	Relation			master_relation;
	Oid					master_nspid;
	Oid					master_relid;
	RelationLocInfo	   *master_reloc;
	StringInfoData		querystr;
	Query			   *create_query;
	List			   *raw_insert_parsetree = NIL;
	List			   *each_querytree_list = NIL;
	List			   *rewrite_tree_list = NIL;
	ListCell		   *lc = NULL,
					   *lc_query = NULL;
	Query			   *insert_query = NULL;
	bool				saved_enable_aux_dml;

	if (auxquery->commandType != CMD_UTILITY ||
		!IsA(auxquery->utilityStmt, CreateAuxStmt))
		elog(ERROR, "Expect auxiliary table query rewriten");

	auxstmt = (CreateAuxStmt *) auxquery->utilityStmt;
	create_stmt = (CreateStmt *)auxstmt->create_stmt;
	index_stmt = (IndexStmt *) auxstmt->index_stmt;

	/* Sanity check */
	Assert(create_stmt && index_stmt);
	Assert(auxstmt->master_relation);
	Assert(auxstmt->aux_column);
	Assert(create_stmt->master_relation);

	/* Master relation check */
	master_relid = RangeVarGetRelidExtended(auxstmt->master_relation,
											ShareLock,
											false, false,
											RangeVarCallbackOwnsRelation,
											NULL);
	master_relation = relation_open(master_relid, NoLock);
	master_reloc = RelationGetLocInfo(master_relation);
	master_nspid = RelationGetNamespace(master_relation);
	switch (master_reloc->locatorType)
	{
		case LOCATOR_TYPE_REPLICATED:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("no need to build auxiliary table for replication table")));
			break;
		case LOCATOR_TYPE_RROBIN:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("cannot build auxiliary table for roundrobin table")));
			break;
		case LOCATOR_TYPE_USER_DEFINED:
			if (list_length(master_reloc->funcAttrNums) > 1)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("auxiliary table on master table which distribute by "
						 		"user-defined function with more than one argument is "
						 		"not supported yet")));
			break;
		case LOCATOR_TYPE_HASH:
		case LOCATOR_TYPE_MODULO:
			/* it is OK */
			break;
		case LOCATOR_TYPE_CUSTOM:
		case LOCATOR_TYPE_RANGE:
			/* not support yet */
			break;
		case LOCATOR_TYPE_NONE:
		case LOCATOR_TYPE_DISTRIBUTED:
		default:
			/* should not reach here */
			Assert(false);
			break;
	}

	/* Auxiliary column check */
	atttuple = SearchSysCacheAttName(master_relid, auxstmt->aux_column);
	if (!HeapTupleIsValid(atttuple))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" does not exist",
						auxstmt->aux_column)));
	auxattform = (Form_pg_attribute) GETSTRUCT(atttuple);
	if (!AttrNumberIsForUserDefinedAttr(auxattform->attnum))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("auxiliary table on system column \"%s\" is not supported",
				 auxstmt->aux_column)));
	if (IsDistribColumn(master_relid, auxattform->attnum))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("no need to build auxiliary table for distribute column \"%s\"",
				 auxstmt->aux_column)));

	/* choose auxiliary table name */
	if (create_stmt->relation == NULL)
	{
		char *relname = ChooseAuxTableName(RelationGetRelationName(master_relation),
										   NameStr(auxattform->attname),
										   "aux", master_nspid);
		create_stmt->relation = makeRangeVar(NULL, pstrdup(relname), -1);
		index_stmt->relation = makeRangeVar(NULL, pstrdup(relname), -1);
	}

	/* makeup table elements */
	create_stmt->tableElts = MakeAuxTableColumns(auxattform, master_relation, &distattnum);
	Assert(AttributeNumberIsValid(distattnum));
	create_stmt->aux_attnum = auxattform->attnum;
	disattform = master_relation->rd_att->attrs[distattnum - 1];

	create_query = copyObject(auxquery);
	create_query->commandType = CMD_UTILITY;
	create_query->utilityStmt = (Node *) create_stmt;

	initStringInfo(&querystr);
	deparse_query(create_query, &querystr, NIL, false, false);

	/* create auxiliary table first */
	ProcessUtility(create_query->utilityStmt,
				   querystr.data,
				   PROCESS_UTILITY_TOPLEVEL, NULL, NULL,
				   false,
				   NULL);

	/* Insert into auxiliary table */
	resetStringInfo(&querystr);
	appendStringInfoString(&querystr, "INSERT INTO ");
	if (create_stmt->relation->schemaname)
		appendStringInfo(&querystr, "%s.", create_stmt->relation->schemaname);
	appendStringInfo(&querystr, "%s ", create_stmt->relation->relname);
	appendStringInfo(&querystr, "SELECT %s, %s, xc_node_id, ctid FROM ",
					NameStr(auxattform->attname),
					NameStr(disattform->attname));
	if (auxstmt->master_relation->schemaname)
		appendStringInfo(&querystr, "%s.", auxstmt->master_relation->schemaname);
	appendStringInfo(&querystr, "%s;", auxstmt->master_relation->relname);

	ReleaseSysCache(atttuple);
	relation_close(master_relation, NoLock);

	raw_insert_parsetree = pg_parse_query(querystr.data);
	saved_enable_aux_dml = enable_aux_dml;
	enable_aux_dml = true;
	PG_TRY();
	{
		foreach (lc, raw_insert_parsetree)
		{
			each_querytree_list = pg_analyze_and_rewrite((Node *) lfirst(lc), querystr.data, NULL, 0);
			foreach (lc_query, each_querytree_list)
			{
				if (IsA(lfirst(lc_query), Query))
				{
					insert_query = (Query *) lfirst(lc_query);
					insert_query->canSetTag = false;
					insert_query->querySource = QSRC_PARSER;
				}
				rewrite_tree_list = lappend(rewrite_tree_list, lfirst(lc_query));
			}
		}
	} PG_CATCH();
	{
		enable_aux_dml = saved_enable_aux_dml;
		PG_RE_THROW();
	} PG_END_TRY();
	enable_aux_dml = saved_enable_aux_dml;

	/* Create index for auxiliary table */
	auxquery->utilityStmt = (Node *) index_stmt;
	auxquery->canSetTag = false;
	auxquery->querySource = QSRC_PARSER;

	return lappend(rewrite_tree_list, auxquery);
}

void RelationBuildAuxiliary(Relation rel)
{
	HeapTuple				tuple;
	Form_pg_aux_class		form_aux;
	Relation				auxrel;
	ScanKeyData				skey;
	SysScanDesc				auxscan;
	List				   *auxlist;
	Bitmapset			   *auxatt;
	MemoryContext			old_context;
	AssertArg(rel);

	if (RelationGetRelid(rel) < FirstNormalObjectId)
		return;

	ScanKeyInit(&skey,
				Anum_pg_aux_class_relid,
				BTEqualStrategyNumber,
				F_OIDEQ,
				ObjectIdGetDatum(RelationGetRelid(rel)));

	auxrel = heap_open(AuxClassRelationId, AccessShareLock);
	auxscan = systable_beginscan(auxrel,
								 AuxClassRelidAttnumIndexId,
								 true,
								 NULL,
								 1,
								 &skey);

	old_context = MemoryContextSwitchTo(CacheMemoryContext);
	auxlist = NIL;
	auxatt = NULL;
	while (HeapTupleIsValid(tuple = systable_getnext(auxscan)))
	{
		form_aux = (Form_pg_aux_class) GETSTRUCT(tuple);
		auxlist = lappend_oid(auxlist, form_aux->auxrelid);
		auxatt = bms_add_member(auxatt, form_aux->attnum);
	}
	rel->rd_auxlist = auxlist;
	rel->rd_auxatt = auxatt;
	MemoryContextSwitchTo(old_context);

	systable_endscan(auxscan);
	heap_close(auxrel, AccessShareLock);
}

Bitmapset *MakeAuxMainRelResultAttnos(Relation rel)
{
	Bitmapset *attr;
	int x;
	Assert(rel->rd_auxatt && rel->rd_locator_info);

	/* system attrs */
	attr = bms_make_singleton(SelfItemPointerAttributeNumber - FirstLowInvalidHeapAttributeNumber);
	attr = bms_add_member(attr, XC_NodeIdAttributeNumber - FirstLowInvalidHeapAttributeNumber);

	/* distribute key */
	if (IsRelationDistributedByUserDefined(rel->rd_locator_info))
	{
		if (list_length(rel->rd_locator_info->funcAttrNums) != 1)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("only support one distribute column yet!"),
					 err_generic_string(PG_DIAG_TABLE_NAME, RelationGetRelationName(rel))));
		attr = bms_add_member(attr, linitial_int(rel->rd_locator_info->funcAttrNums) - FirstLowInvalidHeapAttributeNumber);
	}else if(IsRelationDistributedByValue(rel->rd_locator_info))
	{
		attr = bms_add_member(attr, rel->rd_locator_info->partAttrNum - FirstLowInvalidHeapAttributeNumber);
	}else
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("only support one distribute column yet!"),
				 err_generic_string(PG_DIAG_TABLE_NAME, RelationGetRelationName(rel))));
	}

	/* auxiliary columns */
	x = -1;
	while ((x=bms_next_member(rel->rd_auxatt, x)) >= 0)
		attr = bms_add_member(attr, x - FirstLowInvalidHeapAttributeNumber);

	return attr;
}

List *MakeMainRelTargetForAux(Relation main_rel, Relation aux_rel, Index relid, bool target_entry)
{
	Form_pg_attribute	main_attr;
	Form_pg_attribute	aux_attr;
	TupleDesc			main_desc = RelationGetDescr(main_rel);
	TupleDesc			aux_desc = RelationGetDescr(aux_rel);
	Var				   *var;
	TargetEntry		   *te;
	List			   *result = NIL;
	char			   *attname;
	int					anum;
	int					i,j;

	for(i=anum=0;i<aux_desc->natts;++i)
	{
		aux_attr = TupleDescAttr(aux_desc, i);
		if (aux_attr->attisdropped)
			continue;

		++anum;
		attname = NameStr(aux_attr->attname);
		if (anum == Anum_aux_table_auxnodeid)
		{
			main_attr = SystemAttributeDefinition(XC_NodeIdAttributeNumber,
												  RelationGetForm(main_rel)->relhasoids);
		}else if (anum == Anum_aux_table_auxctid)
		{
			main_attr = SystemAttributeDefinition(SelfItemPointerAttributeNumber,
												  RelationGetForm(main_rel)->relhasoids);
		}else
		{
			for(j=0;j<main_desc->natts;++j)
			{
				main_attr = TupleDescAttr(main_desc, j);
				if (main_attr->attisdropped)
					continue;

				if (strcmp(attname, NameStr(main_attr->attname)) == 0)
					break;
			}
			if (j >= main_desc->natts)
				main_attr = NULL;
		}

		if (main_attr == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("Can not found column \"%s\" in relation \"%s\" for auxiliary table \"%s\"",
					 		attname, RelationGetRelationName(main_rel), RelationGetRelationName(aux_rel)),
					 err_generic_string(PG_DIAG_SCHEMA_NAME, get_namespace_name(RelationGetNamespace(main_rel))),
					 err_generic_string(PG_DIAG_TABLE_NAME, RelationGetRelationName(main_rel)),
					 err_generic_string(PG_DIAG_COLUMN_NAME, attname)));

		if (main_attr->atttypid != aux_attr->atttypid ||
			main_attr->atttypmod != aux_attr->atttypmod)
			ereport(ERROR,
					(errcode(ERRCODE_DATATYPE_MISMATCH),
					 errmsg("Column \"%s\" in relation \"%s\" off type %s does not match auxiliary column of type %s.",
					 		NameStr(main_attr->attname),
							RelationGetRelationName(main_rel),
							format_type_with_typemod(main_attr->atttypid, main_attr->atttypmod),
							format_type_with_typemod(aux_attr->atttypid, aux_attr->atttypmod)),
					 err_generic_string(PG_DIAG_SCHEMA_NAME, get_namespace_name(RelationGetNamespace(main_rel))),
					 err_generic_string(PG_DIAG_TABLE_NAME, RelationGetRelationName(main_rel)),
					 err_generic_string(PG_DIAG_COLUMN_NAME, attname)));

		var = makeVar(relid, main_attr->attnum, main_attr->atttypid, main_attr->atttypmod, main_attr->attcollation, 0);
		if (target_entry)
		{
			te = makeTargetEntry((Expr*)var, (AttrNumber)anum, pstrdup(NameStr(main_attr->attname)), false);
			result = lappend(result, te);
		}else
		{
			result = lappend(result, var);
		}
	}

	return result;
}
