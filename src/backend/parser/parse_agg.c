/*-------------------------------------------------------------------------
 *
 * parse_agg.c--
 *	  handle aggregates in parser
 *
 * Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "postgres.h"
#include "access/heapam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "nodes/relation.h"
#include "optimizer/clauses.h"
#include "parser/parse_agg.h"
#include "parser/parse_expr.h"
#include "parser/parse_node.h"
#include "parser/parse_target.h"
#include "utils/syscache.h"
#include "utils/lsyscache.h"

static bool contain_agg_clause(Node *clause);
static bool exprIsAggOrGroupCol(Node *expr, List *groupClause);
static bool tleIsAggOrGroupCol(TargetEntry *tle, List *groupClause);

/*
 * AddAggToParseState -
 *	  add the aggregate to the list of unique aggregates in pstate.
 *
 * SIDE EFFECT: aggno in target list entry will be modified
 */
void
AddAggToParseState(ParseState *pstate, Aggreg *aggreg)
{
	List	   *ag;
	int			i;

	/*
	 * see if we have the aggregate already (we only need to record the
	 * aggregate once)
	 */
	i = 0;
	foreach(ag, pstate->p_aggs)
	{
		Aggreg	   *a = lfirst(ag);

		if (!strcmp(a->aggname, aggreg->aggname) &&
			equal(a->target, aggreg->target))
		{

			/* fill in the aggno and we're done */
			aggreg->aggno = i;
			return;
		}
		i++;
	}

	/* not found, new aggregate */
	aggreg->aggno = i;
	pstate->p_numAgg++;
	pstate->p_aggs = lappend(pstate->p_aggs, aggreg);
	return;
}

/*
 * finalizeAggregates -
 *	  fill in qry_aggs from pstate. Also checks to make sure that aggregates
 *	  are used in the proper place.
 */
void
finalizeAggregates(ParseState *pstate, Query *qry)
{
	List	   *l;
	int			i;

	parseCheckAggregates(pstate, qry);

	qry->qry_numAgg = pstate->p_numAgg;
	qry->qry_aggs =
		(Aggreg **) palloc(sizeof(Aggreg *) * qry->qry_numAgg);
	i = 0;
	foreach(l, pstate->p_aggs)
		qry->qry_aggs[i++] = (Aggreg *) lfirst(l);
}

/*
 * contain_agg_clause--
 *	  Recursively find aggreg nodes from a clause.
 *
 *	  Returns true if any aggregate found.
 */
static bool
contain_agg_clause(Node *clause)
{
	if (clause == NULL)
		return FALSE;
	else if (IsA(clause, Aggreg))
		return TRUE;
	else if (IsA(clause, Iter))
		return contain_agg_clause(((Iter *) clause)->iterexpr);
	else if (single_node(clause))
		return FALSE;
	else if (or_clause(clause) || and_clause(clause))
	{
		List	   *temp;

		foreach(temp, ((Expr *) clause)->args)
			if (contain_agg_clause(lfirst(temp)))
			return TRUE;
		return FALSE;
	}
	else if (is_funcclause(clause))
	{
		List	   *temp;

		foreach(temp, ((Expr *) clause)->args)
			if (contain_agg_clause(lfirst(temp)))
			return TRUE;
		return FALSE;
	}
	else if (IsA(clause, ArrayRef))
	{
		List	   *temp;

		foreach(temp, ((ArrayRef *) clause)->refupperindexpr)
			if (contain_agg_clause(lfirst(temp)))
			return TRUE;
		foreach(temp, ((ArrayRef *) clause)->reflowerindexpr)
			if (contain_agg_clause(lfirst(temp)))
			return TRUE;
		if (contain_agg_clause(((ArrayRef *) clause)->refexpr))
			return TRUE;
		if (contain_agg_clause(((ArrayRef *) clause)->refassgnexpr))
			return TRUE;
		return FALSE;
	}
	else if (not_clause(clause))
		return contain_agg_clause((Node *) get_notclausearg((Expr *) clause));
	else if (is_opclause(clause))
		return (contain_agg_clause((Node *) get_leftop((Expr *) clause)) ||
			  contain_agg_clause((Node *) get_rightop((Expr *) clause)));

	return FALSE;
}

/*
 * exprIsAggOrGroupCol -
 *	  returns true if the expression does not contain non-group columns.
 */
static bool
exprIsAggOrGroupCol(Node *expr, List *groupClause)
{
	List	   *gl;

	if (expr == NULL || IsA(expr, Const) ||
		IsA(expr, Param) ||IsA(expr, Aggreg))
		return TRUE;

	foreach(gl, groupClause)
	{
		GroupClause *grpcl = lfirst(gl);

		if (equal(expr, grpcl->entry->expr))
			return TRUE;
	}

	if (IsA(expr, Expr))
	{
		List	   *temp;

		foreach(temp, ((Expr *) expr)->args)
			if (!exprIsAggOrGroupCol(lfirst(temp), groupClause))
			return FALSE;
		return TRUE;
	}

	return FALSE;
}

/*
 * tleIsAggOrGroupCol -
 *	  returns true if the TargetEntry is Agg or GroupCol.
 */
static bool
tleIsAggOrGroupCol(TargetEntry *tle, List *groupClause)
{
	Node	   *expr = tle->expr;
	List	   *gl;

	if (expr == NULL || IsA(expr, Const) ||IsA(expr, Param))
		return TRUE;

	foreach(gl, groupClause)
	{
		GroupClause *grpcl = lfirst(gl);

		if (tle->resdom->resno == grpcl->entry->resdom->resno)
		{
			if (contain_agg_clause((Node *) expr))
				elog(WARN, "parser: aggregates not allowed in GROUP BY clause");
			return TRUE;
		}
	}

	if (IsA(expr, Aggreg))
		return TRUE;

	if (IsA(expr, Expr))
	{
		List	   *temp;

		foreach(temp, ((Expr *) expr)->args)
			if (!exprIsAggOrGroupCol(lfirst(temp), groupClause))
			return FALSE;
		return TRUE;
	}

	return FALSE;
}

/*
 * parseCheckAggregates -
 *	  this should really be done earlier but the current grammar
 *	  cannot differentiate functions from aggregates. So we have do check
 *	  here when the target list and the qualifications are finalized.
 */
void
parseCheckAggregates(ParseState *pstate, Query *qry)
{
	List	   *tl;

	Assert(pstate->p_numAgg > 0);

	/*
	 * aggregates never appear in WHERE clauses. (we have to check where
	 * clause first because if there is an aggregate, the check for
	 * non-group column in target list may fail.)
	 */
	if (contain_agg_clause(qry->qual))
		elog(WARN, "parser: aggregates not allowed in WHERE clause");

	/*
	 * the target list can only contain aggregates, group columns and
	 * functions thereof.
	 */
	foreach(tl, qry->targetList)
	{
		TargetEntry *tle = lfirst(tl);

		if (!tleIsAggOrGroupCol(tle, qry->groupClause))
			elog(WARN,
				 "parser: illegal use of aggregates or non-group column in target list");
	}

	/*
	 * the expression specified in the HAVING clause has the same
	 * restriction as those in the target list.
	 */
/*
 * Need to change here when we get HAVING works. Currently
 * qry->havingQual is NULL.		- vadim 04/05/97
	if (!exprIsAggOrGroupCol(qry->havingQual, qry->groupClause))
		elog(WARN,
			 "parser: illegal use of aggregates or non-group column in HAVING clause");
 */
	return;
}


Aggreg	   *
ParseAgg(ParseState *pstate, char *aggname, Oid basetype,
			List *target, int precedence)
{
	Oid			fintype;
	Oid			vartype;
	Oid			xfn1;
	Form_pg_aggregate aggform;
	Aggreg	   *aggreg;
	HeapTuple	theAggTuple;
	bool		usenulls = false;
	
	theAggTuple = SearchSysCacheTuple(AGGNAME, PointerGetDatum(aggname),
									  ObjectIdGetDatum(basetype),
									  0, 0);
	if (!HeapTupleIsValid(theAggTuple))
	{
		elog(WARN, "aggregate %s does not exist", aggname);
	}

	/*
	 *	We do a major hack for count(*) here.
	 *
	 *	Count(*) poses several problems.  First, we need a field that is
	 *	guaranteed to be in the range table, and unique.  Using a constant
	 *	causes the optimizer to properly remove the aggragate from any
	 *	elements of the query.
	 *	Using just 'oid', which can not be null, in the parser fails on:
	 *
	 *		select count(*) from tab1, tab2	    -- oid is not unique
	 *		select count(*) from viewtable		-- views don't have real oids
	 *
	 *	So, for an aggregate with parameter '*', we use the first valid
	 *	range table entry, and pick the first column from the table.
	 *	We set a flag to count nulls, because we could have nulls in
	 *	that column.
	*/
		
	if (nodeTag(lfirst(target)) == T_Const)
	{
		Const *con = (Const *)lfirst(target);
		
		if (con->consttype == UNKNOWNOID && VARSIZE(con->constvalue) == VARHDRSZ)
		{
			Attr *attr = makeNode(Attr);
			List	   *rtable, *rlist;
			RangeTblEntry *first_valid_rte;

			Assert(lnext(target) == NULL);

			if (pstate->p_is_rule)
				rtable = lnext(lnext(pstate->p_rtable));
			else
				rtable = pstate->p_rtable;
		
			first_valid_rte = NULL;
			foreach(rlist, rtable)
			{
				RangeTblEntry *rte = lfirst(rlist);
		
				/* only entries on outer(non-function?) scope */
				if (!rte->inFromCl && rte != pstate->p_target_rangetblentry)
					continue;

				first_valid_rte =rte;
				break;
			}
			if (first_valid_rte == NULL)
				elog(WARN, "Can't find column to do aggregate(*) on.");
				
			attr->relname = first_valid_rte->refname;
			attr->attrs = lcons(makeString(
							get_attname(first_valid_rte->relid,1)),NIL);

			lfirst(target) = transformExpr(pstate, (Node *) attr, precedence);
			usenulls = true;
		}
	}
	
	aggform = (Form_pg_aggregate) GETSTRUCT(theAggTuple);
	fintype = aggform->aggfinaltype;
	xfn1 = aggform->aggtransfn1;

	
	/* only aggregates with transfn1 need a base type */
	if (OidIsValid(xfn1))
	{
		basetype = aggform->aggbasetype;
		if (nodeTag(lfirst(target)) == T_Var)
			vartype = ((Var *) lfirst(target))->vartype;
		else
			vartype = ((Expr *) lfirst(target))->typeOid;

		if (basetype != vartype)
		{
			Type		tp1,
						tp2;

			tp1 = typeidType(basetype);
			tp2 = typeidType(vartype);
			elog(NOTICE, "Aggregate type mismatch:");
			elog(WARN, "%s works on %s, not %s", aggname,
				 typeTypeName(tp1), typeTypeName(tp2));
		}
	}

	aggreg = makeNode(Aggreg);
	aggreg->aggname = pstrdup(aggname);
	aggreg->basetype = aggform->aggbasetype;
	aggreg->aggtype = fintype;

	aggreg->target = lfirst(target);
	if (usenulls)
		aggreg->usenulls = true;

	return aggreg;
}

/*
 * Error message when aggregate lookup fails that gives details of the
 * basetype
 */
void
agg_error(char *caller, char *aggname, Oid basetypeID)
{

	/*
	 * basetypeID that is Invalid (zero) means aggregate over all types.
	 * (count)
	 */

	if (basetypeID == InvalidOid)
	{
		elog(WARN, "%s: aggregate '%s' for all types does not exist", caller, aggname);
	}
	else
	{
		elog(WARN, "%s: aggregate '%s' for '%s' does not exist", caller, aggname,
			 typeidTypeName(basetypeID));
	}
}

