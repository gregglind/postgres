/*-------------------------------------------------------------------------
 *
 * istrat.c
 *	  index scan strategy manipulation code and index strategy manipulation
 *	  operator code.
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $Header$
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/istrat.h"
#include "catalog/catname.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_index.h"
#include "catalog/pg_operator.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/syscache.h"

#ifdef USE_ASSERT_CHECKING
static bool StrategyEvaluationIsValid(StrategyEvaluation evaluation);
static bool StrategyExpressionIsValid(StrategyExpression expression,
						  StrategyNumber maxStrategy);
static ScanKey StrategyMapGetScanKeyEntry(StrategyMap map,
						   StrategyNumber strategyNumber);
static bool StrategyOperatorIsValid(StrategyOperator operator,
						StrategyNumber maxStrategy);
static bool StrategyTermIsValid(StrategyTerm term,
					StrategyNumber maxStrategy);
#endif


/* ----------------------------------------------------------------
 *				   misc strategy support routines
 * ----------------------------------------------------------------
 */

/*
 *		StrategyNumberIsValid
 *		StrategyNumberIsInBounds
 *		StrategyMapIsValid
 *		StrategyTransformMapIsValid
 *		IndexStrategyIsValid
 *
 *				... are now macros in istrat.h -cim 4/27/91
 */

/*
 * StrategyMapGetScanKeyEntry
 *		Returns a scan key entry of a index strategy mapping member.
 *
 * Note:
 *		Assumes that the index strategy mapping is valid.
 *		Assumes that the index strategy number is valid.
 *		Bounds checking should be done outside this routine.
 */
static ScanKey
StrategyMapGetScanKeyEntry(StrategyMap map,
						   StrategyNumber strategyNumber)
{
	Assert(StrategyMapIsValid(map));
	Assert(StrategyNumberIsValid(strategyNumber));
	return &map->entry[strategyNumber - 1];
}

/*
 * IndexStrategyGetStrategyMap
 *		Returns an index strategy mapping of an index strategy.
 *
 * Note:
 *		Assumes that the index strategy is valid.
 *		Assumes that the number of index strategies is valid.
 *		Bounds checking should be done outside this routine.
 */
StrategyMap
IndexStrategyGetStrategyMap(IndexStrategy indexStrategy,
							StrategyNumber maxStrategyNum,
							AttrNumber attrNum)
{
	Assert(IndexStrategyIsValid(indexStrategy));
	Assert(StrategyNumberIsValid(maxStrategyNum));
	Assert(AttributeNumberIsValid(attrNum));

	maxStrategyNum = AMStrategies(maxStrategyNum);		/* XXX */
	return &indexStrategy->strategyMapData[maxStrategyNum * (attrNum - 1)];
}

/*
 * AttributeNumberGetIndexStrategySize
 *		Computes the size of an index strategy.
 */
Size
AttributeNumberGetIndexStrategySize(AttrNumber maxAttributeNumber,
									StrategyNumber maxStrategyNumber)
{
	maxStrategyNumber = AMStrategies(maxStrategyNumber);		/* XXX */
	return maxAttributeNumber * maxStrategyNumber * sizeof(ScanKeyData);
}

#ifdef USE_ASSERT_CHECKING
/*
 * StrategyTransformMapIsValid is now a macro in istrat.h -cim 4/27/91
 */

/* ----------------
 *		StrategyOperatorIsValid
 * ----------------
 */
static bool
StrategyOperatorIsValid(StrategyOperator operator,
						StrategyNumber maxStrategy)
{
	return (bool)
	(PointerIsValid(operator) &&
	 StrategyNumberIsInBounds(operator->strategy, maxStrategy) &&
	 !(operator->flags & ~(SK_NEGATE | SK_COMMUTE)));
}

/* ----------------
 *		StrategyTermIsValid
 * ----------------
 */
static bool
StrategyTermIsValid(StrategyTerm term,
					StrategyNumber maxStrategy)
{
	Index		index;

	if (!PointerIsValid(term) || term->degree == 0)
		return false;

	for (index = 0; index < term->degree; index += 1)
	{
		if (!StrategyOperatorIsValid(&term->operatorData[index],
									 maxStrategy))
			return false;
	}

	return true;
}

/* ----------------
 *		StrategyExpressionIsValid
 * ----------------
 */
static bool
StrategyExpressionIsValid(StrategyExpression expression,
						  StrategyNumber maxStrategy)
{
	StrategyTerm *termP;

	if (!PointerIsValid(expression))
		return true;

	if (!StrategyTermIsValid(expression->term[0], maxStrategy))
		return false;

	termP = &expression->term[1];
	while (StrategyTermIsValid(*termP, maxStrategy))
		termP += 1;

	return (bool)
		(!PointerIsValid(*termP));
}

/* ----------------
 *		StrategyEvaluationIsValid
 * ----------------
 */
static bool
StrategyEvaluationIsValid(StrategyEvaluation evaluation)
{
	Index		index;

	if (!PointerIsValid(evaluation) ||
		!StrategyNumberIsValid(evaluation->maxStrategy) ||
		!StrategyTransformMapIsValid(evaluation->negateTransform) ||
		!StrategyTransformMapIsValid(evaluation->commuteTransform) ||
		!StrategyTransformMapIsValid(evaluation->negateCommuteTransform))
		return false;

	for (index = 0; index < evaluation->maxStrategy; index += 1)
	{
		if (!StrategyExpressionIsValid(evaluation->expression[index],
									   evaluation->maxStrategy))
			return false;
	}
	return true;
}

#endif

#ifdef NOT_USED
/* ----------------
 *		StrategyTermEvaluate
 * ----------------
 */
static bool
StrategyTermEvaluate(StrategyTerm term,
					 StrategyMap map,
					 Datum left,
					 Datum right)
{
	bool		result = false;
	Index		index;
	StrategyOperator operator;

	for (index = 0, operator = &term->operatorData[0];
		 index < term->degree; index += 1, operator += 1)
	{
		ScanKey		entry;

		entry = &map->entry[operator->strategy - 1];

		Assert(RegProcedureIsValid(entry->sk_procedure));

		switch (operator->flags ^ entry->sk_flags)
		{
			case 0x0:
				result = DatumGetBool(FunctionCall2(&entry->sk_func,
													left, right));
				break;

			case SK_NEGATE:
				result = !DatumGetBool(FunctionCall2(&entry->sk_func,
													 left, right));
				break;

			case SK_COMMUTE:
				result = DatumGetBool(FunctionCall2(&entry->sk_func,
													right, left));
				break;

			case SK_NEGATE | SK_COMMUTE:
				result = !DatumGetBool(FunctionCall2(&entry->sk_func,
													 right, left));
				break;

			default:
				elog(ERROR, "StrategyTermEvaluate: impossible case %d",
					 operator->flags ^ entry->sk_flags);
		}
		if (!result)
			return result;
	}

	return result;
}

#endif

/* ----------------
 *		RelationGetStrategy
 *
 * Identify strategy number that describes given procedure, if there is one.
 * ----------------
 */
StrategyNumber
RelationGetStrategy(Relation relation,
					AttrNumber attributeNumber,
					StrategyEvaluation evaluation,
					RegProcedure procedure)
{
	StrategyNumber strategy;
	StrategyMap strategyMap;
	ScanKey		entry;
	Index		index;
	int			numattrs;

	Assert(RelationIsValid(relation));
	numattrs = RelationGetNumberOfAttributes(relation);

	Assert(relation->rd_rel->relkind == RELKIND_INDEX); /* XXX use accessor */
	Assert((attributeNumber >= 1) && (attributeNumber <= numattrs));

	Assert(StrategyEvaluationIsValid(evaluation));
	Assert(RegProcedureIsValid(procedure));

	strategyMap = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
											  evaluation->maxStrategy,
											  attributeNumber);

	/* get a strategy number for the procedure ignoring flags for now */
	for (index = 0; index < evaluation->maxStrategy; index += 1)
	{
		if (strategyMap->entry[index].sk_procedure == procedure)
			break;
	}

	if (index == evaluation->maxStrategy)
		return InvalidStrategy;

	strategy = 1 + index;
	entry = StrategyMapGetScanKeyEntry(strategyMap, strategy);

	Assert(!(entry->sk_flags & ~(SK_NEGATE | SK_COMMUTE)));

	switch (entry->sk_flags & (SK_NEGATE | SK_COMMUTE))
	{
		case 0x0:
			return strategy;

		case SK_NEGATE:
			strategy = evaluation->negateTransform->strategy[strategy - 1];
			break;

		case SK_COMMUTE:
			strategy = evaluation->commuteTransform->strategy[strategy - 1];
			break;

		case SK_NEGATE | SK_COMMUTE:
			strategy = evaluation->negateCommuteTransform->strategy[strategy - 1];
			break;

		default:
			elog(FATAL, "RelationGetStrategy: impossible case %d", entry->sk_flags);
	}

	if (!StrategyNumberIsInBounds(strategy, evaluation->maxStrategy))
	{
		if (!StrategyNumberIsValid(strategy))
			elog(ERROR, "RelationGetStrategy: corrupted evaluation");
	}

	return strategy;
}

#ifdef NOT_USED
/* ----------------
 *		RelationInvokeStrategy
 * ----------------
 */
bool							/* XXX someday, this may return Datum */
RelationInvokeStrategy(Relation relation,
					   StrategyEvaluation evaluation,
					   AttrNumber attributeNumber,
					   StrategyNumber strategy,
					   Datum left,
					   Datum right)
{
	StrategyNumber newStrategy;
	StrategyMap strategyMap;
	ScanKey		entry;
	StrategyTermData termData;
	int			numattrs;

	Assert(RelationIsValid(relation));
	Assert(relation->rd_rel->relkind == RELKIND_INDEX); /* XXX use accessor */
	numattrs = RelationGetNumberOfAttributes(relation);

	Assert(StrategyEvaluationIsValid(evaluation));
	Assert(AttributeNumberIsValid(attributeNumber));
	Assert((attributeNumber >= 1) && (attributeNumber < 1 + numattrs));

	Assert(StrategyNumberIsInBounds(strategy, evaluation->maxStrategy));

	termData.degree = 1;

	strategyMap = IndexStrategyGetStrategyMap(RelationGetIndexStrategy(relation),
											  evaluation->maxStrategy,
											  attributeNumber);

	entry = StrategyMapGetScanKeyEntry(strategyMap, strategy);

	if (RegProcedureIsValid(entry->sk_procedure))
	{
		termData.operatorData[0].strategy = strategy;
		termData.operatorData[0].flags = 0x0;

		return StrategyTermEvaluate(&termData, strategyMap, left, right);
	}


	newStrategy = evaluation->negateTransform->strategy[strategy - 1];
	if (newStrategy != strategy && StrategyNumberIsValid(newStrategy))
	{
		entry = StrategyMapGetScanKeyEntry(strategyMap, newStrategy);

		if (RegProcedureIsValid(entry->sk_procedure))
		{
			termData.operatorData[0].strategy = newStrategy;
			termData.operatorData[0].flags = SK_NEGATE;

			return StrategyTermEvaluate(&termData, strategyMap, left, right);
		}
	}

	newStrategy = evaluation->commuteTransform->strategy[strategy - 1];
	if (newStrategy != strategy && StrategyNumberIsValid(newStrategy))
	{
		entry = StrategyMapGetScanKeyEntry(strategyMap, newStrategy);

		if (RegProcedureIsValid(entry->sk_procedure))
		{
			termData.operatorData[0].strategy = newStrategy;
			termData.operatorData[0].flags = SK_COMMUTE;

			return StrategyTermEvaluate(&termData, strategyMap, left, right);
		}
	}

	newStrategy = evaluation->negateCommuteTransform->strategy[strategy - 1];
	if (newStrategy != strategy && StrategyNumberIsValid(newStrategy))
	{
		entry = StrategyMapGetScanKeyEntry(strategyMap, newStrategy);

		if (RegProcedureIsValid(entry->sk_procedure))
		{
			termData.operatorData[0].strategy = newStrategy;
			termData.operatorData[0].flags = SK_NEGATE | SK_COMMUTE;

			return StrategyTermEvaluate(&termData, strategyMap, left, right);
		}
	}

	if (PointerIsValid(evaluation->expression[strategy - 1]))
	{
		StrategyTerm *termP;

		termP = &evaluation->expression[strategy - 1]->term[0];
		while (PointerIsValid(*termP))
		{
			Index		index;

			for (index = 0; index < (*termP)->degree; index += 1)
			{
				entry = StrategyMapGetScanKeyEntry(strategyMap,
								 (*termP)->operatorData[index].strategy);

				if (!RegProcedureIsValid(entry->sk_procedure))
					break;
			}

			if (index == (*termP)->degree)
				return StrategyTermEvaluate(*termP, strategyMap, left, right);

			termP += 1;
		}
	}

	elog(ERROR, "RelationInvokeStrategy: cannot evaluate strategy %d",
		 strategy);

	/* not reached, just to make compiler happy */
	return FALSE;
}

#endif

/* ----------------
 *		OperatorRelationFillScanKeyEntry
 *
 * Initialize a ScanKey entry given already-opened pg_operator relation.
 * ----------------
 */
static void
OperatorRelationFillScanKeyEntry(Relation operatorRelation,
								 Oid operatorObjectId,
								 ScanKey entry)
{
	HeapTuple	tuple;
	HeapScanDesc scan = NULL;
	bool		cachesearch = (!IsBootstrapProcessingMode()) && IsCacheInitialized();

	if (cachesearch)
	{
		tuple = SearchSysCache(OPEROID,
							   ObjectIdGetDatum(operatorObjectId),
							   0, 0, 0);
	}
	else
	{
		ScanKeyData scanKeyData;

		ScanKeyEntryInitialize(&scanKeyData, 0,
							   ObjectIdAttributeNumber,
							   F_OIDEQ,
							   ObjectIdGetDatum(operatorObjectId));

		scan = heap_beginscan(operatorRelation, false, SnapshotNow,
							  1, &scanKeyData);

		tuple = heap_getnext(scan, 0);
	}

	if (!HeapTupleIsValid(tuple))
	{
		if (!cachesearch)
			heap_endscan(scan);
		elog(ERROR, "OperatorRelationFillScanKeyEntry: unknown operator %u",
			 operatorObjectId);
	}

	MemSet(entry, 0, sizeof(*entry));
	entry->sk_flags = 0;
	entry->sk_procedure = ((Form_pg_operator) GETSTRUCT(tuple))->oprcode;

	if (cachesearch)
		ReleaseSysCache(tuple);
	else
		heap_endscan(scan);

	if (!RegProcedureIsValid(entry->sk_procedure))
		elog(ERROR,
		"OperatorRelationFillScanKeyEntry: no procedure for operator %u",
			 operatorObjectId);

	/*
	 * Formerly we initialized entry->sk_func here, but that's a waste of
	 * time because ScanKey entries in strategy maps are never actually
	 * used to invoke the operator.  Furthermore, to do that we'd have to
	 * worry about setting the proper memory context (the map is probably
	 * not allocated in the current memory context!)
	 */
}


/*
 * IndexSupportInitialize
 *		Initializes an index strategy and associated support procedures.
 *
 * Data is returned into *indexStrategy, *indexSupport, and *isUnique,
 * all of which are objects allocated by the caller.
 *
 * The primary input keys are indexObjectId and accessMethodObjectId.
 * The caller also passes maxStrategyNumber, maxSupportNumber, and
 * maxAttributeNumber, since these indicate the size of the indexStrategy
 * and indexSupport arrays it has allocated --- but in practice these
 * numbers must always match those obtainable from the system catalog
 * entries for the index and access method.
 */
void
IndexSupportInitialize(IndexStrategy indexStrategy,
					   RegProcedure *indexSupport,
					   bool *isUnique,
					   Oid indexObjectId,
					   Oid accessMethodObjectId,
					   StrategyNumber maxStrategyNumber,
					   StrategyNumber maxSupportNumber,
					   AttrNumber maxAttributeNumber)
{
	Relation	relation = NULL;
	HeapScanDesc scan = NULL;
	ScanKeyData entry[2];
	Relation	operatorRelation;
	HeapTuple	tuple;
	Form_pg_index iform;
	StrategyMap map;
	AttrNumber	attNumber;
	int			attIndex;
	Oid			operatorClassObjectId[INDEX_MAX_KEYS];
	bool		cachesearch = (!IsBootstrapProcessingMode()) && IsCacheInitialized();

	if (cachesearch)
	{
		tuple = SearchSysCache(INDEXRELID,
							   ObjectIdGetDatum(indexObjectId),
							   0, 0, 0);
	}
	else
	{
		ScanKeyEntryInitialize(&entry[0], 0, Anum_pg_index_indexrelid,
							   F_OIDEQ,
							   ObjectIdGetDatum(indexObjectId));

		relation = heap_openr(IndexRelationName, AccessShareLock);
		scan = heap_beginscan(relation, false, SnapshotNow, 1, entry);
		tuple = heap_getnext(scan, 0);
	}

	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "IndexSupportInitialize: no pg_index entry for index %u",
			 indexObjectId);

	iform = (Form_pg_index) GETSTRUCT(tuple);

	*isUnique = iform->indisunique;

	maxStrategyNumber = AMStrategies(maxStrategyNumber);

	/*
	 * XXX note that the following assumes the INDEX tuple is well formed
	 * and that the *key and *class are 0 terminated.
	 */
	for (attIndex = 0; attIndex < maxAttributeNumber; attIndex++)
	{
		if (!OidIsValid(iform->indkey[attIndex]))
		{
			if (attIndex == InvalidAttrNumber)
				elog(ERROR, "IndexSupportInitialize: bogus pg_index tuple");
			break;
		}

		operatorClassObjectId[attIndex] = iform->indclass[attIndex];
	}

	if (cachesearch)
		ReleaseSysCache(tuple);
	else
	{
		heap_endscan(scan);
		heap_close(relation, AccessShareLock);
	}

	/* if support routines exist for this access method, load them */
	if (maxSupportNumber > 0)
	{
		ScanKeyEntryInitialize(&entry[0], 0, Anum_pg_amproc_amid,
							   F_OIDEQ,
							   ObjectIdGetDatum(accessMethodObjectId));

		ScanKeyEntryInitialize(&entry[1], 0, Anum_pg_amproc_amopclaid,
							   F_OIDEQ,
							   InvalidOid);	/* will set below */

		relation = heap_openr(AccessMethodProcedureRelationName,
							  AccessShareLock);

		for (attNumber = 1; attNumber <= maxAttributeNumber; attNumber++)
		{
			RegProcedure *loc;
			StrategyNumber support;

			loc = &indexSupport[((attNumber - 1) * maxSupportNumber)];

			for (support = 0; support < maxSupportNumber; ++support)
				loc[support] = InvalidOid;

			entry[1].sk_argument =
				ObjectIdGetDatum(operatorClassObjectId[attNumber - 1]);

			scan = heap_beginscan(relation, false, SnapshotNow, 2, entry);

			while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
			{
				Form_pg_amproc aform;

				aform = (Form_pg_amproc) GETSTRUCT(tuple);
				support = aform->amprocnum;
				Assert(support > 0 && support <= maxSupportNumber);
				loc[support - 1] = aform->amproc;
			}

			heap_endscan(scan);
		}
		heap_close(relation, AccessShareLock);
	}

	/* Now load the strategy information for the index operators */
	ScanKeyEntryInitialize(&entry[0], 0,
						   Anum_pg_amop_amopid,
						   F_OIDEQ,
						   ObjectIdGetDatum(accessMethodObjectId));

	ScanKeyEntryInitialize(&entry[1], 0,
						   Anum_pg_amop_amopclaid,
						   F_OIDEQ,
						   0);	/* will fill below */

	relation = heap_openr(AccessMethodOperatorRelationName, AccessShareLock);
	operatorRelation = heap_openr(OperatorRelationName, AccessShareLock);

	for (attNumber = maxAttributeNumber; attNumber > 0; attNumber--)
	{
		StrategyNumber strategy;

		entry[1].sk_argument =
			ObjectIdGetDatum(operatorClassObjectId[attNumber - 1]);

		map = IndexStrategyGetStrategyMap(indexStrategy,
										  maxStrategyNumber,
										  attNumber);

		for (strategy = 1; strategy <= maxStrategyNumber; strategy++)
			ScanKeyEntrySetIllegal(StrategyMapGetScanKeyEntry(map, strategy));

		scan = heap_beginscan(relation, false, SnapshotNow, 2, entry);

		while (HeapTupleIsValid(tuple = heap_getnext(scan, 0)))
		{
			Form_pg_amop aform;

			aform = (Form_pg_amop) GETSTRUCT(tuple);
			strategy = aform->amopstrategy;
			Assert(strategy > 0 && strategy <= maxStrategyNumber);
			OperatorRelationFillScanKeyEntry(operatorRelation,
											 aform->amopopr,
				   StrategyMapGetScanKeyEntry(map, strategy));
		}

		heap_endscan(scan);
	}

	heap_close(operatorRelation, AccessShareLock);
	heap_close(relation, AccessShareLock);
}

/* ----------------
 *		IndexStrategyDisplay
 * ----------------
 */
#ifdef	ISTRATDEBUG
int
IndexStrategyDisplay(IndexStrategy indexStrategy,
					 StrategyNumber numberOfStrategies,
					 int numberOfAttributes)
{
	StrategyMap strategyMap;
	AttrNumber	attributeNumber;
	StrategyNumber strategyNumber;

	for (attributeNumber = 1; attributeNumber <= numberOfAttributes;
		 attributeNumber += 1)
	{
		strategyMap = IndexStrategyGetStrategyMap(indexStrategy,
												  numberOfStrategies,
												  attributeNumber);

		for (strategyNumber = 1;
			 strategyNumber <= AMStrategies(numberOfStrategies);
			 strategyNumber += 1)
		{
			printf(":att %d\t:str %d\t:opr 0x%x(%d)\n",
				   attributeNumber, strategyNumber,
				   strategyMap->entry[strategyNumber - 1].sk_procedure,
				   strategyMap->entry[strategyNumber - 1].sk_procedure);
		}
	}
}

#endif	 /* defined(ISTRATDEBUG) */
