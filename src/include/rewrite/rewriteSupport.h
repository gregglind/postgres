/*-------------------------------------------------------------------------
 *
 * rewriteSupport.h
 *
 *
 *
 * Portions Copyright (c) 1996-2001, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITESUPPORT_H
#define REWRITESUPPORT_H

extern bool IsDefinedRewriteRule(const char *ruleName);

extern char *MakeRetrieveViewRuleName(const char *view_name);

extern void SetRelationRuleStatus(Oid relationId, bool relHasRules,
					  bool relIsBecomingView);
#endif	 /* REWRITESUPPORT_H */
