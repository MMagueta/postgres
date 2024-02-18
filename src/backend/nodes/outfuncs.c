/*-------------------------------------------------------------------------
 *
 * outfuncs.c
 *	  Output functions for Postgres tree nodes.
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/nodes/outfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>

#include "access/attnum.h"
#include "common/shortest_dec.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/bitmapset.h"
#include "nodes/nodes.h"
#include "nodes/pg_list.h"
#include "utils/datum.h"

static void outChar(StringInfo str, char c);
static void outDouble(StringInfo str, double d);


/*
 * Macros to simplify output of different kinds of fields.  Use these
 * wherever possible to reduce the chance for silly typos.  Note that these
 * hard-wire conventions about the names of the local variables in an Out
 * routine.
 */

/* Write the label for the node type */
#define WRITE_NODE_TYPE(nodelabel) \
	appendStringInfoString(str, nodelabel)

/* Write an integer field (anything written as ":fldname %d") */
#define WRITE_INT_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", node->fldname)

/* Write an unsigned integer field (anything written as ":fldname %u") */
#define WRITE_UINT_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %u", node->fldname)

/* Write an unsigned integer field (anything written with UINT64_FORMAT) */
#define WRITE_UINT64_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " " UINT64_FORMAT, \
					 node->fldname)

/* Write an OID field (don't hard-wire assumption that OID is same as uint) */
#define WRITE_OID_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %u", node->fldname)

/* Write a long-integer field */
#define WRITE_LONG_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %ld", node->fldname)

/* Write a char field (ie, one ascii character) */
#define WRITE_CHAR_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 outChar(str, node->fldname))

/* Write an enumerated-type field as an integer code */
#define WRITE_ENUM_FIELD(fldname, enumtype) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", \
					 (int) node->fldname)

/* Write a float field (actually, they're double) */
#define WRITE_FLOAT_FIELD(fldname) \
	(appendStringInfo(str, " :" CppAsString(fldname) " "), \
	 outDouble(str, node->fldname))

/* Write a boolean field */
#define WRITE_BOOL_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %s", \
					 booltostr(node->fldname))

/* Write a character-string (possibly NULL) field */
#define WRITE_STRING_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outToken(str, node->fldname))

/* Write a parse location field (actually same as INT case) */
#define WRITE_LOCATION_FIELD(fldname) \
	appendStringInfo(str, " :" CppAsString(fldname) " %d", node->fldname)

/* Write a Node field */
#define WRITE_NODE_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outNode(str, node->fldname))

/* Write a bitmapset field */
#define WRITE_BITMAPSET_FIELD(fldname) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 outBitmapset(str, node->fldname))

/* Write a variable-length array (not a List) of Node pointers */
#define WRITE_NODE_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeNodeArray(str, (const Node * const *) node->fldname, len))

/* Write a variable-length array of AttrNumber */
#define WRITE_ATTRNUMBER_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeAttrNumberCols(str, node->fldname, len))

/* Write a variable-length array of Oid */
#define WRITE_OID_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeOidCols(str, node->fldname, len))

/* Write a variable-length array of Index */
#define WRITE_INDEX_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeIndexCols(str, node->fldname, len))

/* Write a variable-length array of int */
#define WRITE_INT_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeIntCols(str, node->fldname, len))

/* Write a variable-length array of bool */
#define WRITE_BOOL_ARRAY(fldname, len) \
	(appendStringInfoString(str, " :" CppAsString(fldname) " "), \
	 writeBoolCols(str, node->fldname, len))

#define booltostr(x)  ((x) ? "true" : "false")


/*
 * outToken
 *	  Convert an ordinary string (eg, an identifier) into a form that
 *	  will be decoded back to a plain token by read.c's functions.
 *
 *	  If a null string pointer is given, it is encoded as '<>'.
 *	  An empty string is encoded as '""'.  To avoid ambiguity, input
 *	  strings beginning with '<' or '"' receive a leading backslash.
 */
void
outToken(StringInfo str, const char *s)
{
	if (s == NULL)
	{
		appendStringInfoString(str, "<>");
		return;
	}
	if (*s == '\0')
	{
		appendStringInfoString(str, "\"\"");
		return;
	}

	/*
	 * Look for characters or patterns that are treated specially by read.c
	 * (either in pg_strtok() or in nodeRead()), and therefore need a
	 * protective backslash.
	 */
	/* These characters only need to be quoted at the start of the string */
	if (*s == '<' ||
		*s == '"' ||
		isdigit((unsigned char) *s) ||
		((*s == '+' || *s == '-') &&
		 (isdigit((unsigned char) s[1]) || s[1] == '.')))
		appendStringInfoChar(str, '\\');
	while (*s)
	{
		/* These chars must be backslashed anywhere in the string */
		if (*s == ' ' || *s == '\n' || *s == '\t' ||
			*s == '(' || *s == ')' || *s == '{' || *s == '}' ||
			*s == '\\')
			appendStringInfoChar(str, '\\');
		appendStringInfoChar(str, *s++);
	}
}

/*
 * Convert one char.  Goes through outToken() so that special characters are
 * escaped.
 */
static void
outChar(StringInfo str, char c)
{
	char		in[2];

	/* Traditionally, we've represented \0 as <>, so keep doing that */
	if (c == '\0')
	{
		appendStringInfoString(str, "<>");
		return;
	}

	in[0] = c;
	in[1] = '\0';

	outToken(str, in);
}

/*
 * Convert a double value, attempting to ensure the value is preserved exactly.
 */
static void
outDouble(StringInfo str, double d)
{
	char		buf[DOUBLE_SHORTEST_DECIMAL_LEN];

	double_to_shortest_decimal_buf(d, buf);
	appendStringInfoString(str, buf);
}

/*
 * common implementation for scalar-array-writing functions
 *
 * The data format is either "<>" for a NULL pointer or "(item item item)".
 * fmtstr must include a leading space, and the rest of it must produce
 * something that will be seen as a single simple token by pg_strtok().
 * convfunc can be empty, or the name of a conversion macro or function.
 */
#define WRITE_SCALAR_ARRAY(fnname, datatype, fmtstr, convfunc) \
static void \
fnname(StringInfo str, const datatype *arr, int len) \
{ \
	if (arr != NULL) \
	{ \
		appendStringInfoChar(str, '('); \
		for (int i = 0; i < len; i++) \
			appendStringInfo(str, fmtstr, convfunc(arr[i])); \
		appendStringInfoChar(str, ')'); \
	} \
	else \
		appendStringInfoString(str, "<>"); \
}

WRITE_SCALAR_ARRAY(writeAttrNumberCols, AttrNumber, " %d",)
WRITE_SCALAR_ARRAY(writeOidCols, Oid, " %u",)
WRITE_SCALAR_ARRAY(writeIndexCols, Index, " %u",)
WRITE_SCALAR_ARRAY(writeIntCols, int, " %d",)
WRITE_SCALAR_ARRAY(writeBoolCols, bool, " %s", booltostr)

/*
 * Print an array (not a List) of Node pointers.
 *
 * The decoration is identical to that of scalar arrays, but we can't
 * quite use appendStringInfo() in the loop.
 */
static void
writeNodeArray(StringInfo str, const Node *const *arr, int len)
{
	if (arr != NULL)
	{
		appendStringInfoChar(str, '(');
		for (int i = 0; i < len; i++)
		{
			appendStringInfoChar(str, ' ');
			outNode(str, arr[i]);
		}
		appendStringInfoChar(str, ')');
	}
	else
		appendStringInfoString(str, "<>");
}

/*
 * Print a List.
 */
static void
_outList(StringInfo str, const List *node)
{
	const ListCell *lc;

	appendStringInfoChar(str, '(');

	if (IsA(node, IntList))
		appendStringInfoChar(str, 'i');
	else if (IsA(node, OidList))
		appendStringInfoChar(str, 'o');
	else if (IsA(node, XidList))
		appendStringInfoChar(str, 'x');

	foreach(lc, node)
	{
		/*
		 * For the sake of backward compatibility, we emit a slightly
		 * different whitespace format for lists of nodes vs. other types of
		 * lists. XXX: is this necessary?
		 */
		if (IsA(node, List))
		{
			outNode(str, lfirst(lc));
			if (lnext(node, lc))
				appendStringInfoChar(str, ' ');
		}
		else if (IsA(node, IntList))
			appendStringInfo(str, " %d", lfirst_int(lc));
		else if (IsA(node, OidList))
			appendStringInfo(str, " %u", lfirst_oid(lc));
		else if (IsA(node, XidList))
			appendStringInfo(str, " %u", lfirst_xid(lc));
		else
			elog(ERROR, "unrecognized list node type: %d",
				 (int) node->type);
	}

	appendStringInfoChar(str, ')');
}

/*
 * outBitmapset -
 *	   converts a bitmap set of integers
 *
 * Note: the output format is "(b int int ...)", similar to an integer List.
 *
 * We export this function for use by extensions that define extensible nodes.
 * That's somewhat historical, though, because calling outNode() will work.
 */
void
outBitmapset(StringInfo str, const Bitmapset *bms)
{
	int			x;

	appendStringInfoChar(str, '(');
	appendStringInfoChar(str, 'b');
	x = -1;
	while ((x = bms_next_member(bms, x)) >= 0)
		appendStringInfo(str, " %d", x);
	appendStringInfoChar(str, ')');
}

/*
 * Print the value of a Datum given its type.
 */
void
outDatum(StringInfo str, Datum value, int typlen, bool typbyval)
{
	Size		length,
				i;
	char	   *s;

	length = datumGetSize(value, typbyval, typlen);

	if (typbyval)
	{
		s = (char *) (&value);
		appendStringInfo(str, "%u [ ", (unsigned int) length);
		for (i = 0; i < (Size) sizeof(Datum); i++)
			appendStringInfo(str, "%d ", (int) (s[i]));
		appendStringInfoChar(str, ']');
	}
	else
	{
		s = (char *) DatumGetPointer(value);
		if (!PointerIsValid(s))
			appendStringInfoString(str, "0 [ ]");
		else
		{
			appendStringInfo(str, "%u [ ", (unsigned int) length);
			for (i = 0; i < length; i++)
				appendStringInfo(str, "%d ", (int) (s[i]));
			appendStringInfoChar(str, ']');
		}
	}
}


#include "outfuncs.funcs.c"


/*
 * Support functions for nodes with custom_read_write attribute or
 * special_read_write attribute
 */

static void
_outConst(StringInfo str, const Const *node)
{
	WRITE_NODE_TYPE("CONST");

	WRITE_OID_FIELD(consttype);
	WRITE_INT_FIELD(consttypmod);
	WRITE_OID_FIELD(constcollid);
	WRITE_INT_FIELD(constlen);
	WRITE_BOOL_FIELD(constbyval);
	WRITE_BOOL_FIELD(constisnull);
	WRITE_LOCATION_FIELD(location);

	appendStringInfoString(str, " :constvalue ");
	if (node->constisnull)
		appendStringInfoString(str, "<>");
	else
		outDatum(str, node->constvalue, node->constlen, node->constbyval);
}

static void
_outBoolExpr(StringInfo str, const BoolExpr *node)
{
	char	   *opstr = NULL;

	WRITE_NODE_TYPE("BOOLEXPR");

	/* do-it-yourself enum representation */
	switch (node->boolop)
	{
		case AND_EXPR:
			opstr = "and";
			break;
		case OR_EXPR:
			opstr = "or";
			break;
		case NOT_EXPR:
			opstr = "not";
			break;
	}
	appendStringInfoString(str, " :boolop ");
	outToken(str, opstr);

	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outForeignKeyOptInfo(StringInfo str, const ForeignKeyOptInfo *node)
{
	int			i;

	WRITE_NODE_TYPE("FOREIGNKEYOPTINFO");

	WRITE_UINT_FIELD(con_relid);
	WRITE_UINT_FIELD(ref_relid);
	WRITE_INT_FIELD(nkeys);
	WRITE_ATTRNUMBER_ARRAY(conkey, node->nkeys);
	WRITE_ATTRNUMBER_ARRAY(confkey, node->nkeys);
	WRITE_OID_ARRAY(conpfeqop, node->nkeys);
	WRITE_INT_FIELD(nmatched_ec);
	WRITE_INT_FIELD(nconst_ec);
	WRITE_INT_FIELD(nmatched_rcols);
	WRITE_INT_FIELD(nmatched_ri);
	/* for compactness, just print the number of matches per column: */
	appendStringInfoString(str, " :eclass");
	for (i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", (node->eclass[i] != NULL));
	appendStringInfoString(str, " :rinfos");
	for (i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", list_length(node->rinfos[i]));
}

static void
_outEquivalenceClass(StringInfo str, const EquivalenceClass *node)
{
	/*
	 * To simplify reading, we just chase up to the topmost merged EC and
	 * print that, without bothering to show the merge-ees separately.
	 */
	while (node->ec_merged)
		node = node->ec_merged;

	WRITE_NODE_TYPE("EQUIVALENCECLASS");

	WRITE_NODE_FIELD(ec_opfamilies);
	WRITE_OID_FIELD(ec_collation);
	WRITE_NODE_FIELD(ec_members);
	WRITE_NODE_FIELD(ec_sources);
	WRITE_NODE_FIELD(ec_derives);
	WRITE_BITMAPSET_FIELD(ec_relids);
	WRITE_BOOL_FIELD(ec_has_const);
	WRITE_BOOL_FIELD(ec_has_volatile);
	WRITE_BOOL_FIELD(ec_broken);
	WRITE_UINT_FIELD(ec_sortref);
	WRITE_UINT_FIELD(ec_min_security);
	WRITE_UINT_FIELD(ec_max_security);
}

static void
_outExtensibleNode(StringInfo str, const ExtensibleNode *node)
{
	const ExtensibleNodeMethods *methods;

	methods = GetExtensibleNodeMethods(node->extnodename, false);

	WRITE_NODE_TYPE("EXTENSIBLENODE");

	WRITE_STRING_FIELD(extnodename);

	/* serialize the private fields */
	methods->nodeOut(str, node);
}

static void
_outRangeTblEntry(StringInfo str, const RangeTblEntry *node)
{
	WRITE_NODE_TYPE("RANGETBLENTRY");

	/* put alias + eref first to make dump more legible */
	WRITE_NODE_FIELD(alias);
	WRITE_NODE_FIELD(eref);
	WRITE_ENUM_FIELD(rtekind, RTEKind);

	switch (node->rtekind)
	{
		case RTE_RELATION:
			WRITE_OID_FIELD(relid);
			WRITE_CHAR_FIELD(relkind);
			WRITE_INT_FIELD(rellockmode);
			WRITE_NODE_FIELD(tablesample);
			WRITE_UINT_FIELD(perminfoindex);
			break;
		case RTE_SUBQUERY:
			WRITE_NODE_FIELD(subquery);
			WRITE_BOOL_FIELD(security_barrier);
			/* we re-use these RELATION fields, too: */
			WRITE_OID_FIELD(relid);
			WRITE_CHAR_FIELD(relkind);
			WRITE_INT_FIELD(rellockmode);
			WRITE_UINT_FIELD(perminfoindex);
			break;
		case RTE_JOIN:
			WRITE_ENUM_FIELD(jointype, JoinType);
			WRITE_INT_FIELD(joinmergedcols);
			WRITE_NODE_FIELD(joinaliasvars);
			WRITE_NODE_FIELD(joinleftcols);
			WRITE_NODE_FIELD(joinrightcols);
			WRITE_NODE_FIELD(join_using_alias);
			break;
		case RTE_FUNCTION:
			WRITE_NODE_FIELD(functions);
			WRITE_BOOL_FIELD(funcordinality);
			break;
		case RTE_TABLEFUNC:
			WRITE_NODE_FIELD(tablefunc);
			break;
		case RTE_VALUES:
			WRITE_NODE_FIELD(values_lists);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_CTE:
			WRITE_STRING_FIELD(ctename);
			WRITE_UINT_FIELD(ctelevelsup);
			WRITE_BOOL_FIELD(self_reference);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_NAMEDTUPLESTORE:
			WRITE_STRING_FIELD(enrname);
			WRITE_FLOAT_FIELD(enrtuples);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			/* we re-use these RELATION fields, too: */
			WRITE_OID_FIELD(relid);
			break;
		case RTE_RESULT:
			/* no extra fields */
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) node->rtekind);
			break;
	}

	WRITE_BOOL_FIELD(lateral);
	WRITE_BOOL_FIELD(inh);
	WRITE_BOOL_FIELD(inFromCl);
	WRITE_NODE_FIELD(securityQuals);
}

static void
_outA_Expr(StringInfo str, const A_Expr *node)
{
	WRITE_NODE_TYPE("A_EXPR");

	switch (node->kind)
	{
		case AEXPR_OP:
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_OP_ANY:
			appendStringInfoString(str, " ANY");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_OP_ALL:
			appendStringInfoString(str, " ALL");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_DISTINCT:
			appendStringInfoString(str, " DISTINCT");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_DISTINCT:
			appendStringInfoString(str, " NOT_DISTINCT");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NULLIF:
			appendStringInfoString(str, " NULLIF");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_IN:
			appendStringInfoString(str, " IN");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_LIKE:
			appendStringInfoString(str, " LIKE");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_ILIKE:
			appendStringInfoString(str, " ILIKE");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_SIMILAR:
			appendStringInfoString(str, " SIMILAR");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN:
			appendStringInfoString(str, " BETWEEN");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN:
			appendStringInfoString(str, " NOT_BETWEEN");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN_SYM:
			appendStringInfoString(str, " BETWEEN_SYM");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN_SYM:
			appendStringInfoString(str, " NOT_BETWEEN_SYM");
			WRITE_NODE_FIELD(name);
			break;
		default:
			elog(ERROR, "unrecognized A_Expr_Kind: %d", (int) node->kind);
			break;
	}

	WRITE_NODE_FIELD(lexpr);
	WRITE_NODE_FIELD(rexpr);
	WRITE_LOCATION_FIELD(location);
}

static void
_outInteger(StringInfo str, const Integer *node)
{
	appendStringInfo(str, "%d", node->ival);
}

static void
_outFloat(StringInfo str, const Float *node)
{
	/*
	 * We assume the value is a valid numeric literal and so does not need
	 * quoting.
	 */
	appendStringInfoString(str, node->fval);
}

static void
_outBoolean(StringInfo str, const Boolean *node)
{
	appendStringInfoString(str, node->boolval ? "true" : "false");
}

static void
_outString(StringInfo str, const String *node)
{
	/*
	 * We use outToken to provide escaping of the string's content, but we
	 * don't want it to convert an empty string to '""', because we're putting
	 * double quotes around the string already.
	 */
	appendStringInfoChar(str, '"');
	if (node->sval[0] != '\0')
		outToken(str, node->sval);
	appendStringInfoChar(str, '"');
}

static void
_outBitString(StringInfo str, const BitString *node)
{
	/*
	 * The lexer will always produce a string starting with 'b' or 'x'.  There
	 * might be characters following that that need escaping, but outToken
	 * won't escape the 'b' or 'x'.  This is relied on by nodeTokenType.
	 */
	Assert(node->bsval[0] == 'b' || node->bsval[0] == 'x');
	outToken(str, node->bsval);
}

static void
_outA_Const(StringInfo str, const A_Const *node)
{
	WRITE_NODE_TYPE("A_CONST");

	if (node->isnull)
		appendStringInfoString(str, " NULL");
	else
	{
		appendStringInfoString(str, " :val ");
		outNode(str, &node->val);
	}
	WRITE_LOCATION_FIELD(location);
}

static void
_outConstraint(StringInfo str, const Constraint *node)
{
	WRITE_NODE_TYPE("CONSTRAINT");

	WRITE_STRING_FIELD(conname);
	WRITE_BOOL_FIELD(deferrable);
	WRITE_BOOL_FIELD(initdeferred);
	WRITE_LOCATION_FIELD(location);

	appendStringInfoString(str, " :contype ");
	switch (node->contype)
	{
		case CONSTR_NULL:
			appendStringInfoString(str, "NULL");
			break;

		case CONSTR_NOTNULL:
			appendStringInfoString(str, "NOT_NULL");
			WRITE_NODE_FIELD(keys);
			WRITE_INT_FIELD(inhcount);
			WRITE_BOOL_FIELD(is_no_inherit);
			WRITE_BOOL_FIELD(skip_validation);
			WRITE_BOOL_FIELD(initially_valid);
			break;

		case CONSTR_DEFAULT:
			appendStringInfoString(str, "DEFAULT");
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			break;

		case CONSTR_IDENTITY:
			appendStringInfoString(str, "IDENTITY");
			WRITE_NODE_FIELD(options);
			WRITE_CHAR_FIELD(generated_when);
			break;

		case CONSTR_GENERATED:
			appendStringInfoString(str, "GENERATED");
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			WRITE_CHAR_FIELD(generated_when);
			break;

		case CONSTR_CHECK:
			appendStringInfoString(str, "CHECK");
			WRITE_BOOL_FIELD(is_no_inherit);
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			WRITE_BOOL_FIELD(skip_validation);
			WRITE_BOOL_FIELD(initially_valid);
			break;

		case CONSTR_PRIMARY:
			appendStringInfoString(str, "PRIMARY_KEY");
			WRITE_NODE_FIELD(keys);
			WRITE_BOOL_FIELD(without_overlaps);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			/* access_method and where_clause not currently used */
			break;

		case CONSTR_UNIQUE:
			appendStringInfoString(str, "UNIQUE");
			WRITE_BOOL_FIELD(nulls_not_distinct);
			WRITE_NODE_FIELD(keys);
			WRITE_BOOL_FIELD(without_overlaps);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			/* access_method and where_clause not currently used */
			break;

		case CONSTR_EXCLUSION:
			appendStringInfoString(str, "EXCLUSION");
			WRITE_NODE_FIELD(exclusions);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			WRITE_STRING_FIELD(access_method);
			WRITE_NODE_FIELD(where_clause);
			break;

		case CONSTR_FOREIGN:
			appendStringInfoString(str, "FOREIGN_KEY");
			WRITE_NODE_FIELD(pktable);
			WRITE_NODE_FIELD(fk_attrs);
			WRITE_NODE_FIELD(pk_attrs);
			WRITE_CHAR_FIELD(fk_matchtype);
			WRITE_CHAR_FIELD(fk_upd_action);
			WRITE_CHAR_FIELD(fk_del_action);
			WRITE_NODE_FIELD(fk_del_set_cols);
			WRITE_NODE_FIELD(old_conpfeqop);
			WRITE_OID_FIELD(old_pktable_oid);
			WRITE_BOOL_FIELD(skip_validation);
			WRITE_BOOL_FIELD(initially_valid);
			break;

		case CONSTR_ATTR_DEFERRABLE:
			appendStringInfoString(str, "ATTR_DEFERRABLE");
			break;

		case CONSTR_ATTR_NOT_DEFERRABLE:
			appendStringInfoString(str, "ATTR_NOT_DEFERRABLE");
			break;

		case CONSTR_ATTR_DEFERRED:
			appendStringInfoString(str, "ATTR_DEFERRED");
			break;

		case CONSTR_ATTR_IMMEDIATE:
			appendStringInfoString(str, "ATTR_IMMEDIATE");
			break;

		default:
			elog(ERROR, "unrecognized ConstrType: %d", (int) node->contype);
			break;
	}
}


/*
 * outNode -
 *	  converts a Node into ascii string and append it to 'str'
 */
void
outNode(StringInfo str, const void *obj)
{
<<<<<<< ours
	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();
||||||| base
	WRITE_NODE_TYPE("WINDOWAGG");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_UINT_FIELD(winref);
	WRITE_INT_FIELD(partNumCols);
	WRITE_ATTRNUMBER_ARRAY(partColIdx, node->partNumCols);
	WRITE_OID_ARRAY(partOperators, node->partNumCols);
	WRITE_OID_ARRAY(partCollations, node->partNumCols);
	WRITE_INT_FIELD(ordNumCols);
	WRITE_ATTRNUMBER_ARRAY(ordColIdx, node->ordNumCols);
	WRITE_OID_ARRAY(ordOperators, node->ordNumCols);
	WRITE_OID_ARRAY(ordCollations, node->ordNumCols);
	WRITE_INT_FIELD(frameOptions);
	WRITE_NODE_FIELD(startOffset);
	WRITE_NODE_FIELD(endOffset);
	WRITE_OID_FIELD(startInRangeFunc);
	WRITE_OID_FIELD(endInRangeFunc);
	WRITE_OID_FIELD(inRangeColl);
	WRITE_BOOL_FIELD(inRangeAsc);
	WRITE_BOOL_FIELD(inRangeNullsFirst);
}

static void
_outGroup(StringInfo str, const Group *node)
{
	WRITE_NODE_TYPE("GROUP");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(grpColIdx, node->numCols);
	WRITE_OID_ARRAY(grpOperators, node->numCols);
	WRITE_OID_ARRAY(grpCollations, node->numCols);
}

static void
_outMaterial(StringInfo str, const Material *node)
{
	WRITE_NODE_TYPE("MATERIAL");

	_outPlanInfo(str, (const Plan *) node);
}

static void
_outMemoize(StringInfo str, const Memoize *node)
{
	WRITE_NODE_TYPE("MEMOIZE");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numKeys);
	WRITE_OID_ARRAY(hashOperators, node->numKeys);
	WRITE_OID_ARRAY(collations, node->numKeys);
	WRITE_NODE_FIELD(param_exprs);
	WRITE_BOOL_FIELD(singlerow);
	WRITE_UINT_FIELD(est_entries);
}

static void
_outSortInfo(StringInfo str, const Sort *node)
{
	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(sortColIdx, node->numCols);
	WRITE_OID_ARRAY(sortOperators, node->numCols);
	WRITE_OID_ARRAY(collations, node->numCols);
	WRITE_BOOL_ARRAY(nullsFirst, node->numCols);
}

static void
_outSort(StringInfo str, const Sort *node)
{
	WRITE_NODE_TYPE("SORT");

	_outSortInfo(str, node);
}

static void
_outIncrementalSort(StringInfo str, const IncrementalSort *node)
{
	WRITE_NODE_TYPE("INCREMENTALSORT");

	_outSortInfo(str, (const Sort *) node);

	WRITE_INT_FIELD(nPresortedCols);
}

static void
_outUnique(StringInfo str, const Unique *node)
{
	WRITE_NODE_TYPE("UNIQUE");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(uniqColIdx, node->numCols);
	WRITE_OID_ARRAY(uniqOperators, node->numCols);
	WRITE_OID_ARRAY(uniqCollations, node->numCols);
}

static void
_outHash(StringInfo str, const Hash *node)
{
	WRITE_NODE_TYPE("HASH");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(hashkeys);
	WRITE_OID_FIELD(skewTable);
	WRITE_INT_FIELD(skewColumn);
	WRITE_BOOL_FIELD(skewInherit);
	WRITE_FLOAT_FIELD(rows_total, "%.0f");
}

static void
_outSetOp(StringInfo str, const SetOp *node)
{
	WRITE_NODE_TYPE("SETOP");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_ENUM_FIELD(cmd, SetOpCmd);
	WRITE_ENUM_FIELD(strategy, SetOpStrategy);
	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(dupColIdx, node->numCols);
	WRITE_OID_ARRAY(dupOperators, node->numCols);
	WRITE_OID_ARRAY(dupCollations, node->numCols);
	WRITE_INT_FIELD(flagColIdx);
	WRITE_INT_FIELD(firstFlag);
	WRITE_LONG_FIELD(numGroups);
}

static void
_outLockRows(StringInfo str, const LockRows *node)
{
	WRITE_NODE_TYPE("LOCKROWS");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(rowMarks);
	WRITE_INT_FIELD(epqParam);
}

static void
_outLimit(StringInfo str, const Limit *node)
{
	WRITE_NODE_TYPE("LIMIT");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
	WRITE_INT_FIELD(uniqNumCols);
	WRITE_ATTRNUMBER_ARRAY(uniqColIdx, node->uniqNumCols);
	WRITE_OID_ARRAY(uniqOperators, node->uniqNumCols);
	WRITE_OID_ARRAY(uniqCollations, node->uniqNumCols);
}

static void
_outNestLoopParam(StringInfo str, const NestLoopParam *node)
{
	WRITE_NODE_TYPE("NESTLOOPPARAM");

	WRITE_INT_FIELD(paramno);
	WRITE_NODE_FIELD(paramval);
}

static void
_outPlanRowMark(StringInfo str, const PlanRowMark *node)
{
	WRITE_NODE_TYPE("PLANROWMARK");

	WRITE_UINT_FIELD(rti);
	WRITE_UINT_FIELD(prti);
	WRITE_UINT_FIELD(rowmarkId);
	WRITE_ENUM_FIELD(markType, RowMarkType);
	WRITE_INT_FIELD(allMarkTypes);
	WRITE_ENUM_FIELD(strength, LockClauseStrength);
	WRITE_ENUM_FIELD(waitPolicy, LockWaitPolicy);
	WRITE_BOOL_FIELD(isParent);
}

static void
_outPartitionPruneInfo(StringInfo str, const PartitionPruneInfo *node)
{
	WRITE_NODE_TYPE("PARTITIONPRUNEINFO");

	WRITE_NODE_FIELD(prune_infos);
	WRITE_BITMAPSET_FIELD(other_subplans);
}

static void
_outPartitionedRelPruneInfo(StringInfo str, const PartitionedRelPruneInfo *node)
{
	WRITE_NODE_TYPE("PARTITIONEDRELPRUNEINFO");

	WRITE_UINT_FIELD(rtindex);
	WRITE_BITMAPSET_FIELD(present_parts);
	WRITE_INT_FIELD(nparts);
	WRITE_INT_ARRAY(subplan_map, node->nparts);
	WRITE_INT_ARRAY(subpart_map, node->nparts);
	WRITE_OID_ARRAY(relid_map, node->nparts);
	WRITE_NODE_FIELD(initial_pruning_steps);
	WRITE_NODE_FIELD(exec_pruning_steps);
	WRITE_BITMAPSET_FIELD(execparamids);
}

static void
_outPartitionPruneStepOp(StringInfo str, const PartitionPruneStepOp *node)
{
	WRITE_NODE_TYPE("PARTITIONPRUNESTEPOP");

	WRITE_INT_FIELD(step.step_id);
	WRITE_INT_FIELD(opstrategy);
	WRITE_NODE_FIELD(exprs);
	WRITE_NODE_FIELD(cmpfns);
	WRITE_BITMAPSET_FIELD(nullkeys);
}

static void
_outPartitionPruneStepCombine(StringInfo str, const PartitionPruneStepCombine *node)
{
	WRITE_NODE_TYPE("PARTITIONPRUNESTEPCOMBINE");

	WRITE_INT_FIELD(step.step_id);
	WRITE_ENUM_FIELD(combineOp, PartitionPruneCombineOp);
	WRITE_NODE_FIELD(source_stepids);
}

static void
_outPlanInvalItem(StringInfo str, const PlanInvalItem *node)
{
	WRITE_NODE_TYPE("PLANINVALITEM");

	WRITE_INT_FIELD(cacheId);
	WRITE_UINT_FIELD(hashValue);
}

/*****************************************************************************
 *
 *	Stuff from primnodes.h.
 *
 *****************************************************************************/

static void
_outAlias(StringInfo str, const Alias *node)
{
	WRITE_NODE_TYPE("ALIAS");

	WRITE_STRING_FIELD(aliasname);
	WRITE_NODE_FIELD(colnames);
}

static void
_outRangeVar(StringInfo str, const RangeVar *node)
{
	WRITE_NODE_TYPE("RANGEVAR");

	/*
	 * we deliberately ignore catalogname here, since it is presently not
	 * semantically meaningful
	 */
	WRITE_STRING_FIELD(schemaname);
	WRITE_STRING_FIELD(relname);
	WRITE_BOOL_FIELD(inh);
	WRITE_CHAR_FIELD(relpersistence);
	WRITE_NODE_FIELD(alias);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTableFunc(StringInfo str, const TableFunc *node)
{
	WRITE_NODE_TYPE("TABLEFUNC");

	WRITE_NODE_FIELD(ns_uris);
	WRITE_NODE_FIELD(ns_names);
	WRITE_NODE_FIELD(docexpr);
	WRITE_NODE_FIELD(rowexpr);
	WRITE_NODE_FIELD(colnames);
	WRITE_NODE_FIELD(coltypes);
	WRITE_NODE_FIELD(coltypmods);
	WRITE_NODE_FIELD(colcollations);
	WRITE_NODE_FIELD(colexprs);
	WRITE_NODE_FIELD(coldefexprs);
	WRITE_BITMAPSET_FIELD(notnulls);
	WRITE_INT_FIELD(ordinalitycol);
	WRITE_LOCATION_FIELD(location);
}

static void
_outIntoClause(StringInfo str, const IntoClause *node)
{
	WRITE_NODE_TYPE("INTOCLAUSE");

	WRITE_NODE_FIELD(rel);
	WRITE_NODE_FIELD(colNames);
	WRITE_STRING_FIELD(accessMethod);
	WRITE_NODE_FIELD(options);
	WRITE_ENUM_FIELD(onCommit, OnCommitAction);
	WRITE_STRING_FIELD(tableSpaceName);
	WRITE_NODE_FIELD(viewQuery);
	WRITE_BOOL_FIELD(skipData);
}

static void
_outVar(StringInfo str, const Var *node)
{
	WRITE_NODE_TYPE("VAR");

	WRITE_UINT_FIELD(varno);
	WRITE_INT_FIELD(varattno);
	WRITE_OID_FIELD(vartype);
	WRITE_INT_FIELD(vartypmod);
	WRITE_OID_FIELD(varcollid);
	WRITE_UINT_FIELD(varlevelsup);
	WRITE_UINT_FIELD(varnosyn);
	WRITE_INT_FIELD(varattnosyn);
	WRITE_LOCATION_FIELD(location);
}

static void
_outConst(StringInfo str, const Const *node)
{
	WRITE_NODE_TYPE("CONST");

	WRITE_OID_FIELD(consttype);
	WRITE_INT_FIELD(consttypmod);
	WRITE_OID_FIELD(constcollid);
	WRITE_INT_FIELD(constlen);
	WRITE_BOOL_FIELD(constbyval);
	WRITE_BOOL_FIELD(constisnull);
	WRITE_LOCATION_FIELD(location);

	appendStringInfoString(str, " :constvalue ");
	if (node->constisnull)
		appendStringInfoString(str, "<>");
	else
		outDatum(str, node->constvalue, node->constlen, node->constbyval);
}

static void
_outParam(StringInfo str, const Param *node)
{
	WRITE_NODE_TYPE("PARAM");

	WRITE_ENUM_FIELD(paramkind, ParamKind);
	WRITE_INT_FIELD(paramid);
	WRITE_OID_FIELD(paramtype);
	WRITE_INT_FIELD(paramtypmod);
	WRITE_OID_FIELD(paramcollid);
	WRITE_LOCATION_FIELD(location);
}

static void
_outAggref(StringInfo str, const Aggref *node)
{
	WRITE_NODE_TYPE("AGGREF");

	WRITE_OID_FIELD(aggfnoid);
	WRITE_OID_FIELD(aggtype);
	WRITE_OID_FIELD(aggcollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_OID_FIELD(aggtranstype);
	WRITE_NODE_FIELD(aggargtypes);
	WRITE_NODE_FIELD(aggdirectargs);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(aggorder);
	WRITE_NODE_FIELD(aggdistinct);
	WRITE_NODE_FIELD(aggfilter);
	WRITE_BOOL_FIELD(aggstar);
	WRITE_BOOL_FIELD(aggvariadic);
	WRITE_CHAR_FIELD(aggkind);
	WRITE_UINT_FIELD(agglevelsup);
	WRITE_ENUM_FIELD(aggsplit, AggSplit);
	WRITE_INT_FIELD(aggno);
	WRITE_INT_FIELD(aggtransno);
	WRITE_LOCATION_FIELD(location);
}

static void
_outGroupingFunc(StringInfo str, const GroupingFunc *node)
{
	WRITE_NODE_TYPE("GROUPINGFUNC");

	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(refs);
	WRITE_NODE_FIELD(cols);
	WRITE_UINT_FIELD(agglevelsup);
	WRITE_LOCATION_FIELD(location);
}

static void
_outWindowFunc(StringInfo str, const WindowFunc *node)
{
	WRITE_NODE_TYPE("WINDOWFUNC");

	WRITE_OID_FIELD(winfnoid);
	WRITE_OID_FIELD(wintype);
	WRITE_OID_FIELD(wincollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(aggfilter);
	WRITE_UINT_FIELD(winref);
	WRITE_BOOL_FIELD(winstar);
	WRITE_BOOL_FIELD(winagg);
	WRITE_LOCATION_FIELD(location);
}

static void
_outSubscriptingRef(StringInfo str, const SubscriptingRef *node)
{
	WRITE_NODE_TYPE("SUBSCRIPTINGREF");

	WRITE_OID_FIELD(refcontainertype);
	WRITE_OID_FIELD(refelemtype);
	WRITE_OID_FIELD(refrestype);
	WRITE_INT_FIELD(reftypmod);
	WRITE_OID_FIELD(refcollid);
	WRITE_NODE_FIELD(refupperindexpr);
	WRITE_NODE_FIELD(reflowerindexpr);
	WRITE_NODE_FIELD(refexpr);
	WRITE_NODE_FIELD(refassgnexpr);
}

static void
_outFuncExpr(StringInfo str, const FuncExpr *node)
{
	WRITE_NODE_TYPE("FUNCEXPR");

	WRITE_OID_FIELD(funcid);
	WRITE_OID_FIELD(funcresulttype);
	WRITE_BOOL_FIELD(funcretset);
	WRITE_BOOL_FIELD(funcvariadic);
	WRITE_ENUM_FIELD(funcformat, CoercionForm);
	WRITE_OID_FIELD(funccollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outNamedArgExpr(StringInfo str, const NamedArgExpr *node)
{
	WRITE_NODE_TYPE("NAMEDARGEXPR");

	WRITE_NODE_FIELD(arg);
	WRITE_STRING_FIELD(name);
	WRITE_INT_FIELD(argnumber);
	WRITE_LOCATION_FIELD(location);
}

static void
_outOpExpr(StringInfo str, const OpExpr *node)
{
	WRITE_NODE_TYPE("OPEXPR");

	WRITE_OID_FIELD(opno);
	WRITE_OID_FIELD(opfuncid);
	WRITE_OID_FIELD(opresulttype);
	WRITE_BOOL_FIELD(opretset);
	WRITE_OID_FIELD(opcollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outDistinctExpr(StringInfo str, const DistinctExpr *node)
{
	WRITE_NODE_TYPE("DISTINCTEXPR");

	WRITE_OID_FIELD(opno);
	WRITE_OID_FIELD(opfuncid);
	WRITE_OID_FIELD(opresulttype);
	WRITE_BOOL_FIELD(opretset);
	WRITE_OID_FIELD(opcollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outNullIfExpr(StringInfo str, const NullIfExpr *node)
{
	WRITE_NODE_TYPE("NULLIFEXPR");

	WRITE_OID_FIELD(opno);
	WRITE_OID_FIELD(opfuncid);
	WRITE_OID_FIELD(opresulttype);
	WRITE_BOOL_FIELD(opretset);
	WRITE_OID_FIELD(opcollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outScalarArrayOpExpr(StringInfo str, const ScalarArrayOpExpr *node)
{
	WRITE_NODE_TYPE("SCALARARRAYOPEXPR");

	WRITE_OID_FIELD(opno);
	WRITE_OID_FIELD(opfuncid);
	WRITE_OID_FIELD(hashfuncid);
	WRITE_OID_FIELD(negfuncid);
	WRITE_BOOL_FIELD(useOr);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outBoolExpr(StringInfo str, const BoolExpr *node)
{
	char	   *opstr = NULL;

	WRITE_NODE_TYPE("BOOLEXPR");

	/* do-it-yourself enum representation */
	switch (node->boolop)
	{
		case AND_EXPR:
			opstr = "and";
			break;
		case OR_EXPR:
			opstr = "or";
			break;
		case NOT_EXPR:
			opstr = "not";
			break;
	}
	appendStringInfoString(str, " :boolop ");
	outToken(str, opstr);

	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outSubLink(StringInfo str, const SubLink *node)
{
	WRITE_NODE_TYPE("SUBLINK");

	WRITE_ENUM_FIELD(subLinkType, SubLinkType);
	WRITE_INT_FIELD(subLinkId);
	WRITE_NODE_FIELD(testexpr);
	WRITE_NODE_FIELD(operName);
	WRITE_NODE_FIELD(subselect);
	WRITE_LOCATION_FIELD(location);
}

static void
_outSubPlan(StringInfo str, const SubPlan *node)
{
	WRITE_NODE_TYPE("SUBPLAN");

	WRITE_ENUM_FIELD(subLinkType, SubLinkType);
	WRITE_NODE_FIELD(testexpr);
	WRITE_NODE_FIELD(paramIds);
	WRITE_INT_FIELD(plan_id);
	WRITE_STRING_FIELD(plan_name);
	WRITE_OID_FIELD(firstColType);
	WRITE_INT_FIELD(firstColTypmod);
	WRITE_OID_FIELD(firstColCollation);
	WRITE_BOOL_FIELD(useHashTable);
	WRITE_BOOL_FIELD(unknownEqFalse);
	WRITE_BOOL_FIELD(parallel_safe);
	WRITE_NODE_FIELD(setParam);
	WRITE_NODE_FIELD(parParam);
	WRITE_NODE_FIELD(args);
	WRITE_FLOAT_FIELD(startup_cost, "%.2f");
	WRITE_FLOAT_FIELD(per_call_cost, "%.2f");
}

static void
_outAlternativeSubPlan(StringInfo str, const AlternativeSubPlan *node)
{
	WRITE_NODE_TYPE("ALTERNATIVESUBPLAN");

	WRITE_NODE_FIELD(subplans);
}

static void
_outFieldSelect(StringInfo str, const FieldSelect *node)
{
	WRITE_NODE_TYPE("FIELDSELECT");

	WRITE_NODE_FIELD(arg);
	WRITE_INT_FIELD(fieldnum);
	WRITE_OID_FIELD(resulttype);
	WRITE_INT_FIELD(resulttypmod);
	WRITE_OID_FIELD(resultcollid);
}

static void
_outFieldStore(StringInfo str, const FieldStore *node)
{
	WRITE_NODE_TYPE("FIELDSTORE");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(newvals);
	WRITE_NODE_FIELD(fieldnums);
	WRITE_OID_FIELD(resulttype);
}

static void
_outRelabelType(StringInfo str, const RelabelType *node)
{
	WRITE_NODE_TYPE("RELABELTYPE");

	WRITE_NODE_FIELD(arg);
	WRITE_OID_FIELD(resulttype);
	WRITE_INT_FIELD(resulttypmod);
	WRITE_OID_FIELD(resultcollid);
	WRITE_ENUM_FIELD(relabelformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCoerceViaIO(StringInfo str, const CoerceViaIO *node)
{
	WRITE_NODE_TYPE("COERCEVIAIO");

	WRITE_NODE_FIELD(arg);
	WRITE_OID_FIELD(resulttype);
	WRITE_OID_FIELD(resultcollid);
	WRITE_ENUM_FIELD(coerceformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outArrayCoerceExpr(StringInfo str, const ArrayCoerceExpr *node)
{
	WRITE_NODE_TYPE("ARRAYCOERCEEXPR");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(elemexpr);
	WRITE_OID_FIELD(resulttype);
	WRITE_INT_FIELD(resulttypmod);
	WRITE_OID_FIELD(resultcollid);
	WRITE_ENUM_FIELD(coerceformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outConvertRowtypeExpr(StringInfo str, const ConvertRowtypeExpr *node)
{
	WRITE_NODE_TYPE("CONVERTROWTYPEEXPR");

	WRITE_NODE_FIELD(arg);
	WRITE_OID_FIELD(resulttype);
	WRITE_ENUM_FIELD(convertformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCollateExpr(StringInfo str, const CollateExpr *node)
{
	WRITE_NODE_TYPE("COLLATE");

	WRITE_NODE_FIELD(arg);
	WRITE_OID_FIELD(collOid);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCaseExpr(StringInfo str, const CaseExpr *node)
{
	WRITE_NODE_TYPE("CASE");

	WRITE_OID_FIELD(casetype);
	WRITE_OID_FIELD(casecollid);
	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(defresult);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCaseWhen(StringInfo str, const CaseWhen *node)
{
	WRITE_NODE_TYPE("WHEN");

	WRITE_NODE_FIELD(expr);
	WRITE_NODE_FIELD(result);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCaseTestExpr(StringInfo str, const CaseTestExpr *node)
{
	WRITE_NODE_TYPE("CASETESTEXPR");

	WRITE_OID_FIELD(typeId);
	WRITE_INT_FIELD(typeMod);
	WRITE_OID_FIELD(collation);
}

static void
_outArrayExpr(StringInfo str, const ArrayExpr *node)
{
	WRITE_NODE_TYPE("ARRAY");

	WRITE_OID_FIELD(array_typeid);
	WRITE_OID_FIELD(array_collid);
	WRITE_OID_FIELD(element_typeid);
	WRITE_NODE_FIELD(elements);
	WRITE_BOOL_FIELD(multidims);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRowExpr(StringInfo str, const RowExpr *node)
{
	WRITE_NODE_TYPE("ROW");

	WRITE_NODE_FIELD(args);
	WRITE_OID_FIELD(row_typeid);
	WRITE_ENUM_FIELD(row_format, CoercionForm);
	WRITE_NODE_FIELD(colnames);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRowCompareExpr(StringInfo str, const RowCompareExpr *node)
{
	WRITE_NODE_TYPE("ROWCOMPARE");

	WRITE_ENUM_FIELD(rctype, RowCompareType);
	WRITE_NODE_FIELD(opnos);
	WRITE_NODE_FIELD(opfamilies);
	WRITE_NODE_FIELD(inputcollids);
	WRITE_NODE_FIELD(largs);
	WRITE_NODE_FIELD(rargs);
}

static void
_outCoalesceExpr(StringInfo str, const CoalesceExpr *node)
{
	WRITE_NODE_TYPE("COALESCE");

	WRITE_OID_FIELD(coalescetype);
	WRITE_OID_FIELD(coalescecollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outMinMaxExpr(StringInfo str, const MinMaxExpr *node)
{
	WRITE_NODE_TYPE("MINMAX");

	WRITE_OID_FIELD(minmaxtype);
	WRITE_OID_FIELD(minmaxcollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_ENUM_FIELD(op, MinMaxOp);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outSQLValueFunction(StringInfo str, const SQLValueFunction *node)
{
	WRITE_NODE_TYPE("SQLVALUEFUNCTION");

	WRITE_ENUM_FIELD(op, SQLValueFunctionOp);
	WRITE_OID_FIELD(type);
	WRITE_INT_FIELD(typmod);
	WRITE_LOCATION_FIELD(location);
}

static void
_outXmlExpr(StringInfo str, const XmlExpr *node)
{
	WRITE_NODE_TYPE("XMLEXPR");

	WRITE_ENUM_FIELD(op, XmlExprOp);
	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(named_args);
	WRITE_NODE_FIELD(arg_names);
	WRITE_NODE_FIELD(args);
	WRITE_ENUM_FIELD(xmloption, XmlOptionType);
	WRITE_OID_FIELD(type);
	WRITE_INT_FIELD(typmod);
	WRITE_LOCATION_FIELD(location);
}

static void
_outNullTest(StringInfo str, const NullTest *node)
{
	WRITE_NODE_TYPE("NULLTEST");

	WRITE_NODE_FIELD(arg);
	WRITE_ENUM_FIELD(nulltesttype, NullTestType);
	WRITE_BOOL_FIELD(argisrow);
	WRITE_LOCATION_FIELD(location);
}

static void
_outBooleanTest(StringInfo str, const BooleanTest *node)
{
	WRITE_NODE_TYPE("BOOLEANTEST");

	WRITE_NODE_FIELD(arg);
	WRITE_ENUM_FIELD(booltesttype, BoolTestType);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCoerceToDomain(StringInfo str, const CoerceToDomain *node)
{
	WRITE_NODE_TYPE("COERCETODOMAIN");

	WRITE_NODE_FIELD(arg);
	WRITE_OID_FIELD(resulttype);
	WRITE_INT_FIELD(resulttypmod);
	WRITE_OID_FIELD(resultcollid);
	WRITE_ENUM_FIELD(coercionformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCoerceToDomainValue(StringInfo str, const CoerceToDomainValue *node)
{
	WRITE_NODE_TYPE("COERCETODOMAINVALUE");

	WRITE_OID_FIELD(typeId);
	WRITE_INT_FIELD(typeMod);
	WRITE_OID_FIELD(collation);
	WRITE_LOCATION_FIELD(location);
}

static void
_outSetToDefault(StringInfo str, const SetToDefault *node)
{
	WRITE_NODE_TYPE("SETTODEFAULT");

	WRITE_OID_FIELD(typeId);
	WRITE_INT_FIELD(typeMod);
	WRITE_OID_FIELD(collation);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCurrentOfExpr(StringInfo str, const CurrentOfExpr *node)
{
	WRITE_NODE_TYPE("CURRENTOFEXPR");

	WRITE_UINT_FIELD(cvarno);
	WRITE_STRING_FIELD(cursor_name);
	WRITE_INT_FIELD(cursor_param);
}

static void
_outNextValueExpr(StringInfo str, const NextValueExpr *node)
{
	WRITE_NODE_TYPE("NEXTVALUEEXPR");

	WRITE_OID_FIELD(seqid);
	WRITE_OID_FIELD(typeId);
}

static void
_outInferenceElem(StringInfo str, const InferenceElem *node)
{
	WRITE_NODE_TYPE("INFERENCEELEM");

	WRITE_NODE_FIELD(expr);
	WRITE_OID_FIELD(infercollid);
	WRITE_OID_FIELD(inferopclass);
}

static void
_outTargetEntry(StringInfo str, const TargetEntry *node)
{
	WRITE_NODE_TYPE("TARGETENTRY");

	WRITE_NODE_FIELD(expr);
	WRITE_INT_FIELD(resno);
	WRITE_STRING_FIELD(resname);
	WRITE_UINT_FIELD(ressortgroupref);
	WRITE_OID_FIELD(resorigtbl);
	WRITE_INT_FIELD(resorigcol);
	WRITE_BOOL_FIELD(resjunk);
}

static void
_outRangeTblRef(StringInfo str, const RangeTblRef *node)
{
	WRITE_NODE_TYPE("RANGETBLREF");

	WRITE_INT_FIELD(rtindex);
}

static void
_outJoinExpr(StringInfo str, const JoinExpr *node)
{
	WRITE_NODE_TYPE("JOINEXPR");

	WRITE_ENUM_FIELD(jointype, JoinType);
	WRITE_BOOL_FIELD(isNatural);
	WRITE_NODE_FIELD(larg);
	WRITE_NODE_FIELD(rarg);
	WRITE_NODE_FIELD(usingClause);
	WRITE_NODE_FIELD(join_using_alias);
	WRITE_NODE_FIELD(quals);
	WRITE_NODE_FIELD(alias);
	WRITE_INT_FIELD(rtindex);
}

static void
_outFromExpr(StringInfo str, const FromExpr *node)
{
	WRITE_NODE_TYPE("FROMEXPR");

	WRITE_NODE_FIELD(fromlist);
	WRITE_NODE_FIELD(quals);
}

static void
_outOnConflictExpr(StringInfo str, const OnConflictExpr *node)
{
	WRITE_NODE_TYPE("ONCONFLICTEXPR");

	WRITE_ENUM_FIELD(action, OnConflictAction);
	WRITE_NODE_FIELD(arbiterElems);
	WRITE_NODE_FIELD(arbiterWhere);
	WRITE_OID_FIELD(constraint);
	WRITE_NODE_FIELD(onConflictSet);
	WRITE_NODE_FIELD(onConflictWhere);
	WRITE_INT_FIELD(exclRelIndex);
	WRITE_NODE_FIELD(exclRelTlist);
}

/*****************************************************************************
 *
 *	Stuff from pathnodes.h.
 *
 *****************************************************************************/

/*
 * print the basic stuff of all nodes that inherit from Path
 *
 * Note we do NOT print the parent, else we'd be in infinite recursion.
 * We can print the parent's relids for identification purposes, though.
 * We print the pathtarget only if it's not the default one for the rel.
 * We also do not print the whole of param_info, since it's printed by
 * _outRelOptInfo; it's sufficient and less cluttering to print just the
 * required outer relids.
 */
static void
_outPathInfo(StringInfo str, const Path *node)
{
	WRITE_ENUM_FIELD(pathtype, NodeTag);
	appendStringInfoString(str, " :parent_relids ");
	outBitmapset(str, node->parent->relids);
	if (node->pathtarget != node->parent->reltarget)
		WRITE_NODE_FIELD(pathtarget);
	appendStringInfoString(str, " :required_outer ");
	if (node->param_info)
		outBitmapset(str, node->param_info->ppi_req_outer);
	else
		outBitmapset(str, NULL);
	WRITE_BOOL_FIELD(parallel_aware);
	WRITE_BOOL_FIELD(parallel_safe);
	WRITE_INT_FIELD(parallel_workers);
	WRITE_FLOAT_FIELD(rows, "%.0f");
	WRITE_FLOAT_FIELD(startup_cost, "%.2f");
	WRITE_FLOAT_FIELD(total_cost, "%.2f");
	WRITE_NODE_FIELD(pathkeys);
}

/*
 * print the basic stuff of all nodes that inherit from JoinPath
 */
static void
_outJoinPathInfo(StringInfo str, const JoinPath *node)
{
	_outPathInfo(str, (const Path *) node);

	WRITE_ENUM_FIELD(jointype, JoinType);
	WRITE_BOOL_FIELD(inner_unique);
	WRITE_NODE_FIELD(outerjoinpath);
	WRITE_NODE_FIELD(innerjoinpath);
	WRITE_NODE_FIELD(joinrestrictinfo);
}

static void
_outPath(StringInfo str, const Path *node)
{
	WRITE_NODE_TYPE("PATH");

	_outPathInfo(str, (const Path *) node);
}

static void
_outIndexPath(StringInfo str, const IndexPath *node)
{
	WRITE_NODE_TYPE("INDEXPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(indexinfo);
	WRITE_NODE_FIELD(indexclauses);
	WRITE_NODE_FIELD(indexorderbys);
	WRITE_NODE_FIELD(indexorderbycols);
	WRITE_ENUM_FIELD(indexscandir, ScanDirection);
	WRITE_FLOAT_FIELD(indextotalcost, "%.2f");
	WRITE_FLOAT_FIELD(indexselectivity, "%.4f");
}

static void
_outBitmapHeapPath(StringInfo str, const BitmapHeapPath *node)
{
	WRITE_NODE_TYPE("BITMAPHEAPPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(bitmapqual);
}

static void
_outBitmapAndPath(StringInfo str, const BitmapAndPath *node)
{
	WRITE_NODE_TYPE("BITMAPANDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(bitmapquals);
	WRITE_FLOAT_FIELD(bitmapselectivity, "%.4f");
}

static void
_outBitmapOrPath(StringInfo str, const BitmapOrPath *node)
{
	WRITE_NODE_TYPE("BITMAPORPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(bitmapquals);
	WRITE_FLOAT_FIELD(bitmapselectivity, "%.4f");
}

static void
_outTidPath(StringInfo str, const TidPath *node)
{
	WRITE_NODE_TYPE("TIDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(tidquals);
}

static void
_outTidRangePath(StringInfo str, const TidRangePath *node)
{
	WRITE_NODE_TYPE("TIDRANGEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(tidrangequals);
}

static void
_outSubqueryScanPath(StringInfo str, const SubqueryScanPath *node)
{
	WRITE_NODE_TYPE("SUBQUERYSCANPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outForeignPath(StringInfo str, const ForeignPath *node)
{
	WRITE_NODE_TYPE("FOREIGNPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(fdw_outerpath);
	WRITE_NODE_FIELD(fdw_private);
}

static void
_outCustomPath(StringInfo str, const CustomPath *node)
{
	WRITE_NODE_TYPE("CUSTOMPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_UINT_FIELD(flags);
	WRITE_NODE_FIELD(custom_paths);
	WRITE_NODE_FIELD(custom_private);
	appendStringInfoString(str, " :methods ");
	outToken(str, node->methods->CustomName);
}

static void
_outAppendPath(StringInfo str, const AppendPath *node)
{
	WRITE_NODE_TYPE("APPENDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpaths);
	WRITE_INT_FIELD(first_partial_path);
	WRITE_FLOAT_FIELD(limit_tuples, "%.0f");
}

static void
_outMergeAppendPath(StringInfo str, const MergeAppendPath *node)
{
	WRITE_NODE_TYPE("MERGEAPPENDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpaths);
	WRITE_FLOAT_FIELD(limit_tuples, "%.0f");
}

static void
_outGroupResultPath(StringInfo str, const GroupResultPath *node)
{
	WRITE_NODE_TYPE("GROUPRESULTPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(quals);
}

static void
_outMaterialPath(StringInfo str, const MaterialPath *node)
{
	WRITE_NODE_TYPE("MATERIALPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outMemoizePath(StringInfo str, const MemoizePath *node)
{
	WRITE_NODE_TYPE("MEMOIZEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(hash_operators);
	WRITE_NODE_FIELD(param_exprs);
	WRITE_BOOL_FIELD(singlerow);
	WRITE_FLOAT_FIELD(calls, "%.0f");
	WRITE_UINT_FIELD(est_entries);
}

static void
_outUniquePath(StringInfo str, const UniquePath *node)
{
	WRITE_NODE_TYPE("UNIQUEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(umethod, UniquePathMethod);
	WRITE_NODE_FIELD(in_operators);
	WRITE_NODE_FIELD(uniq_exprs);
}

static void
_outGatherPath(StringInfo str, const GatherPath *node)
{
	WRITE_NODE_TYPE("GATHERPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_BOOL_FIELD(single_copy);
	WRITE_INT_FIELD(num_workers);
}

static void
_outProjectionPath(StringInfo str, const ProjectionPath *node)
{
	WRITE_NODE_TYPE("PROJECTIONPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_BOOL_FIELD(dummypp);
}

static void
_outProjectSetPath(StringInfo str, const ProjectSetPath *node)
{
	WRITE_NODE_TYPE("PROJECTSETPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outSortPathInfo(StringInfo str, const SortPath *node)
{
	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outSortPath(StringInfo str, const SortPath *node)
{
	WRITE_NODE_TYPE("SORTPATH");

	_outSortPathInfo(str, node);
}

static void
_outIncrementalSortPath(StringInfo str, const IncrementalSortPath *node)
{
	WRITE_NODE_TYPE("INCREMENTALSORTPATH");

	_outSortPathInfo(str, (const SortPath *) node);

	WRITE_INT_FIELD(nPresortedCols);
}

static void
_outGroupPath(StringInfo str, const GroupPath *node)
{
	WRITE_NODE_TYPE("GROUPPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(groupClause);
	WRITE_NODE_FIELD(qual);
}

static void
_outUpperUniquePath(StringInfo str, const UpperUniquePath *node)
{
	WRITE_NODE_TYPE("UPPERUNIQUEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_INT_FIELD(numkeys);
}

static void
_outAggPath(StringInfo str, const AggPath *node)
{
	WRITE_NODE_TYPE("AGGPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(aggstrategy, AggStrategy);
	WRITE_ENUM_FIELD(aggsplit, AggSplit);
	WRITE_FLOAT_FIELD(numGroups, "%.0f");
	WRITE_UINT64_FIELD(transitionSpace);
	WRITE_NODE_FIELD(groupClause);
	WRITE_NODE_FIELD(qual);
}

static void
_outRollupData(StringInfo str, const RollupData *node)
{
	WRITE_NODE_TYPE("ROLLUP");

	WRITE_NODE_FIELD(groupClause);
	WRITE_NODE_FIELD(gsets);
	WRITE_NODE_FIELD(gsets_data);
	WRITE_FLOAT_FIELD(numGroups, "%.0f");
	WRITE_BOOL_FIELD(hashable);
	WRITE_BOOL_FIELD(is_hashed);
}

static void
_outGroupingSetData(StringInfo str, const GroupingSetData *node)
{
	WRITE_NODE_TYPE("GSDATA");

	WRITE_NODE_FIELD(set);
	WRITE_FLOAT_FIELD(numGroups, "%.0f");
}

static void
_outGroupingSetsPath(StringInfo str, const GroupingSetsPath *node)
{
	WRITE_NODE_TYPE("GROUPINGSETSPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(aggstrategy, AggStrategy);
	WRITE_NODE_FIELD(rollups);
	WRITE_NODE_FIELD(qual);
	WRITE_UINT64_FIELD(transitionSpace);
}

static void
_outMinMaxAggPath(StringInfo str, const MinMaxAggPath *node)
{
	WRITE_NODE_TYPE("MINMAXAGGPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(mmaggregates);
	WRITE_NODE_FIELD(quals);
}

static void
_outWindowAggPath(StringInfo str, const WindowAggPath *node)
{
	WRITE_NODE_TYPE("WINDOWAGGPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(winclause);
}

static void
_outSetOpPath(StringInfo str, const SetOpPath *node)
{
	WRITE_NODE_TYPE("SETOPPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(cmd, SetOpCmd);
	WRITE_ENUM_FIELD(strategy, SetOpStrategy);
	WRITE_NODE_FIELD(distinctList);
	WRITE_INT_FIELD(flagColIdx);
	WRITE_INT_FIELD(firstFlag);
	WRITE_FLOAT_FIELD(numGroups, "%.0f");
}

static void
_outRecursiveUnionPath(StringInfo str, const RecursiveUnionPath *node)
{
	WRITE_NODE_TYPE("RECURSIVEUNIONPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(leftpath);
	WRITE_NODE_FIELD(rightpath);
	WRITE_NODE_FIELD(distinctList);
	WRITE_INT_FIELD(wtParam);
	WRITE_FLOAT_FIELD(numGroups, "%.0f");
}

static void
_outLockRowsPath(StringInfo str, const LockRowsPath *node)
{
	WRITE_NODE_TYPE("LOCKROWSPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_INT_FIELD(epqParam);
}

static void
_outModifyTablePath(StringInfo str, const ModifyTablePath *node)
{
	WRITE_NODE_TYPE("MODIFYTABLEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(operation, CmdType);
	WRITE_BOOL_FIELD(canSetTag);
	WRITE_UINT_FIELD(nominalRelation);
	WRITE_UINT_FIELD(rootRelation);
	WRITE_BOOL_FIELD(partColsUpdated);
	WRITE_NODE_FIELD(resultRelations);
	WRITE_NODE_FIELD(updateColnosLists);
	WRITE_NODE_FIELD(withCheckOptionLists);
	WRITE_NODE_FIELD(returningLists);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(onconflict);
	WRITE_INT_FIELD(epqParam);
}

static void
_outLimitPath(StringInfo str, const LimitPath *node)
{
	WRITE_NODE_TYPE("LIMITPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
}

static void
_outGatherMergePath(StringInfo str, const GatherMergePath *node)
{
	WRITE_NODE_TYPE("GATHERMERGEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_INT_FIELD(num_workers);
}

static void
_outNestPath(StringInfo str, const NestPath *node)
{
	WRITE_NODE_TYPE("NESTPATH");

	_outJoinPathInfo(str, (const JoinPath *) node);
}

static void
_outMergePath(StringInfo str, const MergePath *node)
{
	WRITE_NODE_TYPE("MERGEPATH");

	_outJoinPathInfo(str, (const JoinPath *) node);

	WRITE_NODE_FIELD(path_mergeclauses);
	WRITE_NODE_FIELD(outersortkeys);
	WRITE_NODE_FIELD(innersortkeys);
	WRITE_BOOL_FIELD(skip_mark_restore);
	WRITE_BOOL_FIELD(materialize_inner);
}

static void
_outHashPath(StringInfo str, const HashPath *node)
{
	WRITE_NODE_TYPE("HASHPATH");

	_outJoinPathInfo(str, (const JoinPath *) node);

	WRITE_NODE_FIELD(path_hashclauses);
	WRITE_INT_FIELD(num_batches);
	WRITE_FLOAT_FIELD(inner_rows_total, "%.0f");
}

static void
_outPlannerGlobal(StringInfo str, const PlannerGlobal *node)
{
	WRITE_NODE_TYPE("PLANNERGLOBAL");

	/* NB: this isn't a complete set of fields */
	WRITE_NODE_FIELD(subplans);
	WRITE_BITMAPSET_FIELD(rewindPlanIDs);
	WRITE_NODE_FIELD(finalrtable);
	WRITE_NODE_FIELD(finalrowmarks);
	WRITE_NODE_FIELD(resultRelations);
	WRITE_NODE_FIELD(appendRelations);
	WRITE_NODE_FIELD(relationOids);
	WRITE_NODE_FIELD(invalItems);
	WRITE_NODE_FIELD(paramExecTypes);
	WRITE_UINT_FIELD(lastPHId);
	WRITE_UINT_FIELD(lastRowMarkId);
	WRITE_INT_FIELD(lastPlanNodeId);
	WRITE_BOOL_FIELD(transientPlan);
	WRITE_BOOL_FIELD(dependsOnRole);
	WRITE_BOOL_FIELD(parallelModeOK);
	WRITE_BOOL_FIELD(parallelModeNeeded);
	WRITE_CHAR_FIELD(maxParallelHazard);
}

static void
_outPlannerInfo(StringInfo str, const PlannerInfo *node)
{
	WRITE_NODE_TYPE("PLANNERINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_NODE_FIELD(parse);
	WRITE_NODE_FIELD(glob);
	WRITE_UINT_FIELD(query_level);
	WRITE_NODE_FIELD(plan_params);
	WRITE_BITMAPSET_FIELD(outer_params);
	WRITE_BITMAPSET_FIELD(all_baserels);
	WRITE_BITMAPSET_FIELD(nullable_baserels);
	WRITE_NODE_FIELD(join_rel_list);
	WRITE_INT_FIELD(join_cur_level);
	WRITE_NODE_FIELD(init_plans);
	WRITE_NODE_FIELD(cte_plan_ids);
	WRITE_NODE_FIELD(multiexpr_params);
	WRITE_NODE_FIELD(eq_classes);
	WRITE_BOOL_FIELD(ec_merging_done);
	WRITE_NODE_FIELD(canon_pathkeys);
	WRITE_NODE_FIELD(left_join_clauses);
	WRITE_NODE_FIELD(right_join_clauses);
	WRITE_NODE_FIELD(full_join_clauses);
	WRITE_NODE_FIELD(join_info_list);
	WRITE_BITMAPSET_FIELD(all_result_relids);
	WRITE_BITMAPSET_FIELD(leaf_result_relids);
	WRITE_NODE_FIELD(append_rel_list);
	WRITE_NODE_FIELD(row_identity_vars);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(placeholder_list);
	WRITE_NODE_FIELD(fkey_list);
	WRITE_NODE_FIELD(query_pathkeys);
	WRITE_NODE_FIELD(group_pathkeys);
	WRITE_NODE_FIELD(window_pathkeys);
	WRITE_NODE_FIELD(distinct_pathkeys);
	WRITE_NODE_FIELD(sort_pathkeys);
	WRITE_NODE_FIELD(processed_tlist);
	WRITE_NODE_FIELD(update_colnos);
	WRITE_NODE_FIELD(minmax_aggs);
	WRITE_FLOAT_FIELD(total_table_pages, "%.0f");
	WRITE_FLOAT_FIELD(tuple_fraction, "%.4f");
	WRITE_FLOAT_FIELD(limit_tuples, "%.0f");
	WRITE_UINT_FIELD(qual_security_level);
	WRITE_BOOL_FIELD(hasJoinRTEs);
	WRITE_BOOL_FIELD(hasLateralRTEs);
	WRITE_BOOL_FIELD(hasHavingQual);
	WRITE_BOOL_FIELD(hasPseudoConstantQuals);
	WRITE_BOOL_FIELD(hasAlternativeSubPlans);
	WRITE_BOOL_FIELD(hasRecursion);
	WRITE_INT_FIELD(wt_param_id);
	WRITE_BITMAPSET_FIELD(curOuterRels);
	WRITE_NODE_FIELD(curOuterParams);
	WRITE_BOOL_FIELD(partColsUpdated);
}

static void
_outRelOptInfo(StringInfo str, const RelOptInfo *node)
{
	WRITE_NODE_TYPE("RELOPTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_ENUM_FIELD(reloptkind, RelOptKind);
	WRITE_BITMAPSET_FIELD(relids);
	WRITE_FLOAT_FIELD(rows, "%.0f");
	WRITE_BOOL_FIELD(consider_startup);
	WRITE_BOOL_FIELD(consider_param_startup);
	WRITE_BOOL_FIELD(consider_parallel);
	WRITE_NODE_FIELD(reltarget);
	WRITE_NODE_FIELD(pathlist);
	WRITE_NODE_FIELD(ppilist);
	WRITE_NODE_FIELD(partial_pathlist);
	WRITE_NODE_FIELD(cheapest_startup_path);
	WRITE_NODE_FIELD(cheapest_total_path);
	WRITE_NODE_FIELD(cheapest_unique_path);
	WRITE_NODE_FIELD(cheapest_parameterized_paths);
	WRITE_BITMAPSET_FIELD(direct_lateral_relids);
	WRITE_BITMAPSET_FIELD(lateral_relids);
	WRITE_UINT_FIELD(relid);
	WRITE_OID_FIELD(reltablespace);
	WRITE_ENUM_FIELD(rtekind, RTEKind);
	WRITE_INT_FIELD(min_attr);
	WRITE_INT_FIELD(max_attr);
	WRITE_NODE_FIELD(lateral_vars);
	WRITE_BITMAPSET_FIELD(lateral_referencers);
	WRITE_NODE_FIELD(indexlist);
	WRITE_NODE_FIELD(statlist);
	WRITE_UINT_FIELD(pages);
	WRITE_FLOAT_FIELD(tuples, "%.0f");
	WRITE_FLOAT_FIELD(allvisfrac, "%.6f");
	WRITE_BITMAPSET_FIELD(eclass_indexes);
	WRITE_NODE_FIELD(subroot);
	WRITE_NODE_FIELD(subplan_params);
	WRITE_INT_FIELD(rel_parallel_workers);
	WRITE_UINT_FIELD(amflags);
	WRITE_OID_FIELD(serverid);
	WRITE_OID_FIELD(userid);
	WRITE_BOOL_FIELD(useridiscurrent);
	/* we don't try to print fdwroutine or fdw_private */
	/* can't print unique_for_rels/non_unique_for_rels; BMSes aren't Nodes */
	WRITE_NODE_FIELD(baserestrictinfo);
	WRITE_UINT_FIELD(baserestrict_min_security);
	WRITE_NODE_FIELD(joininfo);
	WRITE_BOOL_FIELD(has_eclass_joins);
	WRITE_BOOL_FIELD(consider_partitionwise_join);
	WRITE_BITMAPSET_FIELD(top_parent_relids);
	WRITE_BOOL_FIELD(partbounds_merged);
	WRITE_BITMAPSET_FIELD(live_parts);
	WRITE_BITMAPSET_FIELD(all_partrels);
}

static void
_outIndexOptInfo(StringInfo str, const IndexOptInfo *node)
{
	WRITE_NODE_TYPE("INDEXOPTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_OID_FIELD(indexoid);
	/* Do NOT print rel field, else infinite recursion */
	WRITE_UINT_FIELD(pages);
	WRITE_FLOAT_FIELD(tuples, "%.0f");
	WRITE_INT_FIELD(tree_height);
	WRITE_INT_FIELD(ncolumns);
	/* array fields aren't really worth the trouble to print */
	WRITE_OID_FIELD(relam);
	/* indexprs is redundant since we print indextlist */
	WRITE_NODE_FIELD(indpred);
	WRITE_NODE_FIELD(indextlist);
	WRITE_NODE_FIELD(indrestrictinfo);
	WRITE_BOOL_FIELD(predOK);
	WRITE_BOOL_FIELD(unique);
	WRITE_BOOL_FIELD(immediate);
	WRITE_BOOL_FIELD(hypothetical);
	/* we don't bother with fields copied from the index AM's API struct */
}

static void
_outForeignKeyOptInfo(StringInfo str, const ForeignKeyOptInfo *node)
{
	int			i;

	WRITE_NODE_TYPE("FOREIGNKEYOPTINFO");

	WRITE_UINT_FIELD(con_relid);
	WRITE_UINT_FIELD(ref_relid);
	WRITE_INT_FIELD(nkeys);
	WRITE_ATTRNUMBER_ARRAY(conkey, node->nkeys);
	WRITE_ATTRNUMBER_ARRAY(confkey, node->nkeys);
	WRITE_OID_ARRAY(conpfeqop, node->nkeys);
	WRITE_INT_FIELD(nmatched_ec);
	WRITE_INT_FIELD(nconst_ec);
	WRITE_INT_FIELD(nmatched_rcols);
	WRITE_INT_FIELD(nmatched_ri);
	/* for compactness, just print the number of matches per column: */
	appendStringInfoString(str, " :eclass");
	for (i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", (node->eclass[i] != NULL));
	appendStringInfoString(str, " :rinfos");
	for (i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", list_length(node->rinfos[i]));
}

static void
_outStatisticExtInfo(StringInfo str, const StatisticExtInfo *node)
{
	WRITE_NODE_TYPE("STATISTICEXTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_OID_FIELD(statOid);
	/* don't write rel, leads to infinite recursion in plan tree dump */
	WRITE_CHAR_FIELD(kind);
	WRITE_BITMAPSET_FIELD(keys);
}

static void
_outEquivalenceClass(StringInfo str, const EquivalenceClass *node)
{
	/*
	 * To simplify reading, we just chase up to the topmost merged EC and
	 * print that, without bothering to show the merge-ees separately.
	 */
	while (node->ec_merged)
		node = node->ec_merged;

	WRITE_NODE_TYPE("EQUIVALENCECLASS");

	WRITE_NODE_FIELD(ec_opfamilies);
	WRITE_OID_FIELD(ec_collation);
	WRITE_NODE_FIELD(ec_members);
	WRITE_NODE_FIELD(ec_sources);
	WRITE_NODE_FIELD(ec_derives);
	WRITE_BITMAPSET_FIELD(ec_relids);
	WRITE_BOOL_FIELD(ec_has_const);
	WRITE_BOOL_FIELD(ec_has_volatile);
	WRITE_BOOL_FIELD(ec_below_outer_join);
	WRITE_BOOL_FIELD(ec_broken);
	WRITE_UINT_FIELD(ec_sortref);
	WRITE_UINT_FIELD(ec_min_security);
	WRITE_UINT_FIELD(ec_max_security);
}

static void
_outEquivalenceMember(StringInfo str, const EquivalenceMember *node)
{
	WRITE_NODE_TYPE("EQUIVALENCEMEMBER");

	WRITE_NODE_FIELD(em_expr);
	WRITE_BITMAPSET_FIELD(em_relids);
	WRITE_BITMAPSET_FIELD(em_nullable_relids);
	WRITE_BOOL_FIELD(em_is_const);
	WRITE_BOOL_FIELD(em_is_child);
	WRITE_OID_FIELD(em_datatype);
}

static void
_outPathKey(StringInfo str, const PathKey *node)
{
	WRITE_NODE_TYPE("PATHKEY");

	WRITE_NODE_FIELD(pk_eclass);
	WRITE_OID_FIELD(pk_opfamily);
	WRITE_INT_FIELD(pk_strategy);
	WRITE_BOOL_FIELD(pk_nulls_first);
}

static void
_outPathTarget(StringInfo str, const PathTarget *node)
{
	WRITE_NODE_TYPE("PATHTARGET");

	WRITE_NODE_FIELD(exprs);
	if (node->sortgrouprefs)
	{
		int			i;

		appendStringInfoString(str, " :sortgrouprefs");
		for (i = 0; i < list_length(node->exprs); i++)
			appendStringInfo(str, " %u", node->sortgrouprefs[i]);
	}
	WRITE_FLOAT_FIELD(cost.startup, "%.2f");
	WRITE_FLOAT_FIELD(cost.per_tuple, "%.2f");
	WRITE_INT_FIELD(width);
	WRITE_ENUM_FIELD(has_volatile_expr, VolatileFunctionStatus);
}

static void
_outParamPathInfo(StringInfo str, const ParamPathInfo *node)
{
	WRITE_NODE_TYPE("PARAMPATHINFO");

	WRITE_BITMAPSET_FIELD(ppi_req_outer);
	WRITE_FLOAT_FIELD(ppi_rows, "%.0f");
	WRITE_NODE_FIELD(ppi_clauses);
}

static void
_outRestrictInfo(StringInfo str, const RestrictInfo *node)
{
	WRITE_NODE_TYPE("RESTRICTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_NODE_FIELD(clause);
	WRITE_BOOL_FIELD(is_pushed_down);
	WRITE_BOOL_FIELD(outerjoin_delayed);
	WRITE_BOOL_FIELD(can_join);
	WRITE_BOOL_FIELD(pseudoconstant);
	WRITE_BOOL_FIELD(leakproof);
	WRITE_ENUM_FIELD(has_volatile, VolatileFunctionStatus);
	WRITE_UINT_FIELD(security_level);
	WRITE_BITMAPSET_FIELD(clause_relids);
	WRITE_BITMAPSET_FIELD(required_relids);
	WRITE_BITMAPSET_FIELD(outer_relids);
	WRITE_BITMAPSET_FIELD(nullable_relids);
	WRITE_BITMAPSET_FIELD(left_relids);
	WRITE_BITMAPSET_FIELD(right_relids);
	WRITE_NODE_FIELD(orclause);
	/* don't write parent_ec, leads to infinite recursion in plan tree dump */
	WRITE_FLOAT_FIELD(norm_selec, "%.4f");
	WRITE_FLOAT_FIELD(outer_selec, "%.4f");
	WRITE_NODE_FIELD(mergeopfamilies);
	/* don't write left_ec, leads to infinite recursion in plan tree dump */
	/* don't write right_ec, leads to infinite recursion in plan tree dump */
	WRITE_NODE_FIELD(left_em);
	WRITE_NODE_FIELD(right_em);
	WRITE_BOOL_FIELD(outer_is_left);
	WRITE_OID_FIELD(hashjoinoperator);
	WRITE_OID_FIELD(hasheqoperator);
}

static void
_outIndexClause(StringInfo str, const IndexClause *node)
{
	WRITE_NODE_TYPE("INDEXCLAUSE");

	WRITE_NODE_FIELD(rinfo);
	WRITE_NODE_FIELD(indexquals);
	WRITE_BOOL_FIELD(lossy);
	WRITE_INT_FIELD(indexcol);
	WRITE_NODE_FIELD(indexcols);
}

static void
_outPlaceHolderVar(StringInfo str, const PlaceHolderVar *node)
{
	WRITE_NODE_TYPE("PLACEHOLDERVAR");

	WRITE_NODE_FIELD(phexpr);
	WRITE_BITMAPSET_FIELD(phrels);
	WRITE_UINT_FIELD(phid);
	WRITE_UINT_FIELD(phlevelsup);
}

static void
_outSpecialJoinInfo(StringInfo str, const SpecialJoinInfo *node)
{
	WRITE_NODE_TYPE("SPECIALJOININFO");

	WRITE_BITMAPSET_FIELD(min_lefthand);
	WRITE_BITMAPSET_FIELD(min_righthand);
	WRITE_BITMAPSET_FIELD(syn_lefthand);
	WRITE_BITMAPSET_FIELD(syn_righthand);
	WRITE_ENUM_FIELD(jointype, JoinType);
	WRITE_BOOL_FIELD(lhs_strict);
	WRITE_BOOL_FIELD(delay_upper_joins);
	WRITE_BOOL_FIELD(semi_can_btree);
	WRITE_BOOL_FIELD(semi_can_hash);
	WRITE_NODE_FIELD(semi_operators);
	WRITE_NODE_FIELD(semi_rhs_exprs);
}

static void
_outAppendRelInfo(StringInfo str, const AppendRelInfo *node)
{
	WRITE_NODE_TYPE("APPENDRELINFO");

	WRITE_UINT_FIELD(parent_relid);
	WRITE_UINT_FIELD(child_relid);
	WRITE_OID_FIELD(parent_reltype);
	WRITE_OID_FIELD(child_reltype);
	WRITE_NODE_FIELD(translated_vars);
	WRITE_INT_FIELD(num_child_cols);
	WRITE_ATTRNUMBER_ARRAY(parent_colnos, node->num_child_cols);
	WRITE_OID_FIELD(parent_reloid);
}

static void
_outRowIdentityVarInfo(StringInfo str, const RowIdentityVarInfo *node)
{
	WRITE_NODE_TYPE("ROWIDENTITYVARINFO");

	WRITE_NODE_FIELD(rowidvar);
	WRITE_INT_FIELD(rowidwidth);
	WRITE_STRING_FIELD(rowidname);
	WRITE_BITMAPSET_FIELD(rowidrels);
}

static void
_outPlaceHolderInfo(StringInfo str, const PlaceHolderInfo *node)
{
	WRITE_NODE_TYPE("PLACEHOLDERINFO");

	WRITE_UINT_FIELD(phid);
	WRITE_NODE_FIELD(ph_var);
	WRITE_BITMAPSET_FIELD(ph_eval_at);
	WRITE_BITMAPSET_FIELD(ph_lateral);
	WRITE_BITMAPSET_FIELD(ph_needed);
	WRITE_INT_FIELD(ph_width);
}

static void
_outMinMaxAggInfo(StringInfo str, const MinMaxAggInfo *node)
{
	WRITE_NODE_TYPE("MINMAXAGGINFO");

	WRITE_OID_FIELD(aggfnoid);
	WRITE_OID_FIELD(aggsortop);
	WRITE_NODE_FIELD(target);
	/* We intentionally omit subroot --- too large, not interesting enough */
	WRITE_NODE_FIELD(path);
	WRITE_FLOAT_FIELD(pathcost, "%.2f");
	WRITE_NODE_FIELD(param);
}

static void
_outPlannerParamItem(StringInfo str, const PlannerParamItem *node)
{
	WRITE_NODE_TYPE("PLANNERPARAMITEM");

	WRITE_NODE_FIELD(item);
	WRITE_INT_FIELD(paramId);
}

/*****************************************************************************
 *
 *	Stuff from extensible.h
 *
 *****************************************************************************/

static void
_outExtensibleNode(StringInfo str, const ExtensibleNode *node)
{
	const ExtensibleNodeMethods *methods;

	methods = GetExtensibleNodeMethods(node->extnodename, false);

	WRITE_NODE_TYPE("EXTENSIBLENODE");

	WRITE_STRING_FIELD(extnodename);

	/* serialize the private fields */
	methods->nodeOut(str, node);
}

/*****************************************************************************
 *
 *	Stuff from parsenodes.h.
 *
 *****************************************************************************/

/*
 * print the basic stuff of all nodes that inherit from CreateStmt
 */
static void
_outCreateStmtInfo(StringInfo str, const CreateStmt *node)
{
	WRITE_NODE_FIELD(relation);
	WRITE_NODE_FIELD(tableElts);
	WRITE_NODE_FIELD(inhRelations);
	WRITE_NODE_FIELD(partspec);
	WRITE_NODE_FIELD(partbound);
	WRITE_NODE_FIELD(ofTypename);
	WRITE_NODE_FIELD(constraints);
	WRITE_NODE_FIELD(options);
	WRITE_ENUM_FIELD(oncommit, OnCommitAction);
	WRITE_STRING_FIELD(tablespacename);
	WRITE_STRING_FIELD(accessMethod);
	WRITE_BOOL_FIELD(if_not_exists);
}

static void
_outCreateStmt(StringInfo str, const CreateStmt *node)
{
	WRITE_NODE_TYPE("CREATESTMT");

	_outCreateStmtInfo(str, (const CreateStmt *) node);
}

static void
_outCreateForeignTableStmt(StringInfo str, const CreateForeignTableStmt *node)
{
	WRITE_NODE_TYPE("CREATEFOREIGNTABLESTMT");

	_outCreateStmtInfo(str, (const CreateStmt *) node);

	WRITE_STRING_FIELD(servername);
	WRITE_NODE_FIELD(options);
}

static void
_outImportForeignSchemaStmt(StringInfo str, const ImportForeignSchemaStmt *node)
{
	WRITE_NODE_TYPE("IMPORTFOREIGNSCHEMASTMT");

	WRITE_STRING_FIELD(server_name);
	WRITE_STRING_FIELD(remote_schema);
	WRITE_STRING_FIELD(local_schema);
	WRITE_ENUM_FIELD(list_type, ImportForeignSchemaType);
	WRITE_NODE_FIELD(table_list);
	WRITE_NODE_FIELD(options);
}

static void
_outIndexStmt(StringInfo str, const IndexStmt *node)
{
	WRITE_NODE_TYPE("INDEXSTMT");

	WRITE_STRING_FIELD(idxname);
	WRITE_NODE_FIELD(relation);
	WRITE_STRING_FIELD(accessMethod);
	WRITE_STRING_FIELD(tableSpace);
	WRITE_NODE_FIELD(indexParams);
	WRITE_NODE_FIELD(indexIncludingParams);
	WRITE_NODE_FIELD(options);
	WRITE_NODE_FIELD(whereClause);
	WRITE_NODE_FIELD(excludeOpNames);
	WRITE_STRING_FIELD(idxcomment);
	WRITE_OID_FIELD(indexOid);
	WRITE_OID_FIELD(oldNode);
	WRITE_UINT_FIELD(oldCreateSubid);
	WRITE_UINT_FIELD(oldFirstRelfilenodeSubid);
	WRITE_BOOL_FIELD(unique);
	WRITE_BOOL_FIELD(primary);
	WRITE_BOOL_FIELD(isconstraint);
	WRITE_BOOL_FIELD(deferrable);
	WRITE_BOOL_FIELD(initdeferred);
	WRITE_BOOL_FIELD(transformed);
	WRITE_BOOL_FIELD(concurrent);
	WRITE_BOOL_FIELD(if_not_exists);
	WRITE_BOOL_FIELD(reset_default_tblspc);
}

static void
_outCreateStatsStmt(StringInfo str, const CreateStatsStmt *node)
{
	WRITE_NODE_TYPE("CREATESTATSSTMT");

	WRITE_NODE_FIELD(defnames);
	WRITE_NODE_FIELD(stat_types);
	WRITE_NODE_FIELD(exprs);
	WRITE_NODE_FIELD(relations);
	WRITE_STRING_FIELD(stxcomment);
	WRITE_BOOL_FIELD(transformed);
	WRITE_BOOL_FIELD(if_not_exists);
}

static void
_outAlterStatsStmt(StringInfo str, const AlterStatsStmt *node)
{
	WRITE_NODE_TYPE("ALTERSTATSSTMT");

	WRITE_NODE_FIELD(defnames);
	WRITE_INT_FIELD(stxstattarget);
	WRITE_BOOL_FIELD(missing_ok);
}

static void
_outNotifyStmt(StringInfo str, const NotifyStmt *node)
{
	WRITE_NODE_TYPE("NOTIFY");

	WRITE_STRING_FIELD(conditionname);
	WRITE_STRING_FIELD(payload);
}

static void
_outDeclareCursorStmt(StringInfo str, const DeclareCursorStmt *node)
{
	WRITE_NODE_TYPE("DECLARECURSOR");

	WRITE_STRING_FIELD(portalname);
	WRITE_INT_FIELD(options);
	WRITE_NODE_FIELD(query);
}

static void
_outSelectStmt(StringInfo str, const SelectStmt *node)
{
	WRITE_NODE_TYPE("SELECT");

	WRITE_NODE_FIELD(distinctClause);
	WRITE_NODE_FIELD(intoClause);
	WRITE_NODE_FIELD(targetList);
	WRITE_NODE_FIELD(fromClause);
	WRITE_NODE_FIELD(whereClause);
	WRITE_NODE_FIELD(groupClause);
	WRITE_BOOL_FIELD(groupDistinct);
	WRITE_NODE_FIELD(havingClause);
	WRITE_NODE_FIELD(windowClause);
	WRITE_NODE_FIELD(valuesLists);
	WRITE_NODE_FIELD(sortClause);
	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
	WRITE_NODE_FIELD(lockingClause);
	WRITE_NODE_FIELD(withClause);
	WRITE_ENUM_FIELD(op, SetOperation);
	WRITE_BOOL_FIELD(all);
	WRITE_NODE_FIELD(larg);
	WRITE_NODE_FIELD(rarg);
}

static void
_outReturnStmt(StringInfo str, const ReturnStmt *node)
{
	WRITE_NODE_TYPE("RETURN");

	WRITE_NODE_FIELD(returnval);
}

static void
_outPLAssignStmt(StringInfo str, const PLAssignStmt *node)
{
	WRITE_NODE_TYPE("PLASSIGN");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(indirection);
	WRITE_INT_FIELD(nnames);
	WRITE_NODE_FIELD(val);
	WRITE_LOCATION_FIELD(location);
}

static void
_outFuncCall(StringInfo str, const FuncCall *node)
{
	WRITE_NODE_TYPE("FUNCCALL");

	WRITE_NODE_FIELD(funcname);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(agg_order);
	WRITE_NODE_FIELD(agg_filter);
	WRITE_NODE_FIELD(over);
	WRITE_BOOL_FIELD(agg_within_group);
	WRITE_BOOL_FIELD(agg_star);
	WRITE_BOOL_FIELD(agg_distinct);
	WRITE_BOOL_FIELD(func_variadic);
	WRITE_ENUM_FIELD(funcformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outDefElem(StringInfo str, const DefElem *node)
{
	WRITE_NODE_TYPE("DEFELEM");

	WRITE_STRING_FIELD(defnamespace);
	WRITE_STRING_FIELD(defname);
	WRITE_NODE_FIELD(arg);
	WRITE_ENUM_FIELD(defaction, DefElemAction);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTableLikeClause(StringInfo str, const TableLikeClause *node)
{
	WRITE_NODE_TYPE("TABLELIKECLAUSE");

	WRITE_NODE_FIELD(relation);
	WRITE_UINT_FIELD(options);
	WRITE_OID_FIELD(relationOid);
}

static void
_outLockingClause(StringInfo str, const LockingClause *node)
{
	WRITE_NODE_TYPE("LOCKINGCLAUSE");

	WRITE_NODE_FIELD(lockedRels);
	WRITE_ENUM_FIELD(strength, LockClauseStrength);
	WRITE_ENUM_FIELD(waitPolicy, LockWaitPolicy);
}

static void
_outXmlSerialize(StringInfo str, const XmlSerialize *node)
{
	WRITE_NODE_TYPE("XMLSERIALIZE");

	WRITE_ENUM_FIELD(xmloption, XmlOptionType);
	WRITE_NODE_FIELD(expr);
	WRITE_NODE_FIELD(typeName);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTriggerTransition(StringInfo str, const TriggerTransition *node)
{
	WRITE_NODE_TYPE("TRIGGERTRANSITION");

	WRITE_STRING_FIELD(name);
	WRITE_BOOL_FIELD(isNew);
	WRITE_BOOL_FIELD(isTable);
}

static void
_outColumnDef(StringInfo str, const ColumnDef *node)
{
	WRITE_NODE_TYPE("COLUMNDEF");

	WRITE_STRING_FIELD(colname);
	WRITE_NODE_FIELD(typeName);
	WRITE_STRING_FIELD(compression);
	WRITE_INT_FIELD(inhcount);
	WRITE_BOOL_FIELD(is_local);
	WRITE_BOOL_FIELD(is_not_null);
	WRITE_BOOL_FIELD(is_from_type);
	WRITE_CHAR_FIELD(storage);
	WRITE_NODE_FIELD(raw_default);
	WRITE_NODE_FIELD(cooked_default);
	WRITE_CHAR_FIELD(identity);
	WRITE_NODE_FIELD(identitySequence);
	WRITE_CHAR_FIELD(generated);
	WRITE_NODE_FIELD(collClause);
	WRITE_OID_FIELD(collOid);
	WRITE_NODE_FIELD(constraints);
	WRITE_NODE_FIELD(fdwoptions);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTypeName(StringInfo str, const TypeName *node)
{
	WRITE_NODE_TYPE("TYPENAME");

	WRITE_NODE_FIELD(names);
	WRITE_OID_FIELD(typeOid);
	WRITE_BOOL_FIELD(setof);
	WRITE_BOOL_FIELD(pct_type);
	WRITE_NODE_FIELD(typmods);
	WRITE_INT_FIELD(typemod);
	WRITE_NODE_FIELD(arrayBounds);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTypeCast(StringInfo str, const TypeCast *node)
{
	WRITE_NODE_TYPE("TYPECAST");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(typeName);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCollateClause(StringInfo str, const CollateClause *node)
{
	WRITE_NODE_TYPE("COLLATECLAUSE");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(collname);
	WRITE_LOCATION_FIELD(location);
}

static void
_outIndexElem(StringInfo str, const IndexElem *node)
{
	WRITE_NODE_TYPE("INDEXELEM");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(expr);
	WRITE_STRING_FIELD(indexcolname);
	WRITE_NODE_FIELD(collation);
	WRITE_NODE_FIELD(opclass);
	WRITE_NODE_FIELD(opclassopts);
	WRITE_ENUM_FIELD(ordering, SortByDir);
	WRITE_ENUM_FIELD(nulls_ordering, SortByNulls);
}

static void
_outStatsElem(StringInfo str, const StatsElem *node)
{
	WRITE_NODE_TYPE("STATSELEM");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(expr);
}

static void
_outQuery(StringInfo str, const Query *node)
{
	WRITE_NODE_TYPE("QUERY");

	WRITE_ENUM_FIELD(commandType, CmdType);
	WRITE_ENUM_FIELD(querySource, QuerySource);
	/* we intentionally do not print the queryId field */
	WRITE_BOOL_FIELD(canSetTag);

	/*
	 * Hack to work around missing outfuncs routines for a lot of the
	 * utility-statement node types.  (The only one we actually *need* for
	 * rules support is NotifyStmt.)  Someday we ought to support 'em all, but
	 * for the meantime do this to avoid getting lots of warnings when running
	 * with debug_print_parse on.
	 */
	if (node->utilityStmt)
	{
		switch (nodeTag(node->utilityStmt))
		{
			case T_CreateStmt:
			case T_IndexStmt:
			case T_NotifyStmt:
			case T_DeclareCursorStmt:
				WRITE_NODE_FIELD(utilityStmt);
				break;
			default:
				appendStringInfoString(str, " :utilityStmt ?");
				break;
		}
	}
	else
		appendStringInfoString(str, " :utilityStmt <>");

	WRITE_INT_FIELD(resultRelation);
	WRITE_BOOL_FIELD(hasAggs);
	WRITE_BOOL_FIELD(hasWindowFuncs);
	WRITE_BOOL_FIELD(hasTargetSRFs);
	WRITE_BOOL_FIELD(hasSubLinks);
	WRITE_BOOL_FIELD(hasDistinctOn);
	WRITE_BOOL_FIELD(hasRecursive);
	WRITE_BOOL_FIELD(hasModifyingCTE);
	WRITE_BOOL_FIELD(hasForUpdate);
	WRITE_BOOL_FIELD(hasRowSecurity);
	WRITE_BOOL_FIELD(isReturn);
	WRITE_NODE_FIELD(cteList);
	WRITE_NODE_FIELD(rtable);
	WRITE_NODE_FIELD(jointree);
	WRITE_NODE_FIELD(targetList);
	WRITE_ENUM_FIELD(override, OverridingKind);
	WRITE_NODE_FIELD(onConflict);
	WRITE_NODE_FIELD(returningList);
	WRITE_NODE_FIELD(groupClause);
	WRITE_BOOL_FIELD(groupDistinct);
	WRITE_NODE_FIELD(groupingSets);
	WRITE_NODE_FIELD(havingQual);
	WRITE_NODE_FIELD(windowClause);
	WRITE_NODE_FIELD(distinctClause);
	WRITE_NODE_FIELD(sortClause);
	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(setOperations);
	WRITE_NODE_FIELD(constraintDeps);
	WRITE_NODE_FIELD(withCheckOptions);
	WRITE_LOCATION_FIELD(stmt_location);
	WRITE_INT_FIELD(stmt_len);
}

static void
_outWithCheckOption(StringInfo str, const WithCheckOption *node)
{
	WRITE_NODE_TYPE("WITHCHECKOPTION");

	WRITE_ENUM_FIELD(kind, WCOKind);
	WRITE_STRING_FIELD(relname);
	WRITE_STRING_FIELD(polname);
	WRITE_NODE_FIELD(qual);
	WRITE_BOOL_FIELD(cascaded);
}

static void
_outSortGroupClause(StringInfo str, const SortGroupClause *node)
{
	WRITE_NODE_TYPE("SORTGROUPCLAUSE");

	WRITE_UINT_FIELD(tleSortGroupRef);
	WRITE_OID_FIELD(eqop);
	WRITE_OID_FIELD(sortop);
	WRITE_BOOL_FIELD(nulls_first);
	WRITE_BOOL_FIELD(hashable);
}

static void
_outGroupingSet(StringInfo str, const GroupingSet *node)
{
	WRITE_NODE_TYPE("GROUPINGSET");

	WRITE_ENUM_FIELD(kind, GroupingSetKind);
	WRITE_NODE_FIELD(content);
	WRITE_LOCATION_FIELD(location);
}

static void
_outWindowClause(StringInfo str, const WindowClause *node)
{
	WRITE_NODE_TYPE("WINDOWCLAUSE");

	WRITE_STRING_FIELD(name);
	WRITE_STRING_FIELD(refname);
	WRITE_NODE_FIELD(partitionClause);
	WRITE_NODE_FIELD(orderClause);
	WRITE_INT_FIELD(frameOptions);
	WRITE_NODE_FIELD(startOffset);
	WRITE_NODE_FIELD(endOffset);
	WRITE_OID_FIELD(startInRangeFunc);
	WRITE_OID_FIELD(endInRangeFunc);
	WRITE_OID_FIELD(inRangeColl);
	WRITE_BOOL_FIELD(inRangeAsc);
	WRITE_BOOL_FIELD(inRangeNullsFirst);
	WRITE_UINT_FIELD(winref);
	WRITE_BOOL_FIELD(copiedOrder);
}

static void
_outRowMarkClause(StringInfo str, const RowMarkClause *node)
{
	WRITE_NODE_TYPE("ROWMARKCLAUSE");

	WRITE_UINT_FIELD(rti);
	WRITE_ENUM_FIELD(strength, LockClauseStrength);
	WRITE_ENUM_FIELD(waitPolicy, LockWaitPolicy);
	WRITE_BOOL_FIELD(pushedDown);
}

static void
_outWithClause(StringInfo str, const WithClause *node)
{
	WRITE_NODE_TYPE("WITHCLAUSE");

	WRITE_NODE_FIELD(ctes);
	WRITE_BOOL_FIELD(recursive);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCTESearchClause(StringInfo str, const CTESearchClause *node)
{
	WRITE_NODE_TYPE("CTESEARCHCLAUSE");

	WRITE_NODE_FIELD(search_col_list);
	WRITE_BOOL_FIELD(search_breadth_first);
	WRITE_STRING_FIELD(search_seq_column);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCTECycleClause(StringInfo str, const CTECycleClause *node)
{
	WRITE_NODE_TYPE("CTECYCLECLAUSE");

	WRITE_NODE_FIELD(cycle_col_list);
	WRITE_STRING_FIELD(cycle_mark_column);
	WRITE_NODE_FIELD(cycle_mark_value);
	WRITE_NODE_FIELD(cycle_mark_default);
	WRITE_STRING_FIELD(cycle_path_column);
	WRITE_LOCATION_FIELD(location);
	WRITE_OID_FIELD(cycle_mark_type);
	WRITE_INT_FIELD(cycle_mark_typmod);
	WRITE_OID_FIELD(cycle_mark_collation);
	WRITE_OID_FIELD(cycle_mark_neop);
}

static void
_outCommonTableExpr(StringInfo str, const CommonTableExpr *node)
{
	WRITE_NODE_TYPE("COMMONTABLEEXPR");

	WRITE_STRING_FIELD(ctename);
	WRITE_NODE_FIELD(aliascolnames);
	WRITE_ENUM_FIELD(ctematerialized, CTEMaterialize);
	WRITE_NODE_FIELD(ctequery);
	WRITE_NODE_FIELD(search_clause);
	WRITE_NODE_FIELD(cycle_clause);
	WRITE_LOCATION_FIELD(location);
	WRITE_BOOL_FIELD(cterecursive);
	WRITE_INT_FIELD(cterefcount);
	WRITE_NODE_FIELD(ctecolnames);
	WRITE_NODE_FIELD(ctecoltypes);
	WRITE_NODE_FIELD(ctecoltypmods);
	WRITE_NODE_FIELD(ctecolcollations);
}

static void
_outSetOperationStmt(StringInfo str, const SetOperationStmt *node)
{
	WRITE_NODE_TYPE("SETOPERATIONSTMT");

	WRITE_ENUM_FIELD(op, SetOperation);
	WRITE_BOOL_FIELD(all);
	WRITE_NODE_FIELD(larg);
	WRITE_NODE_FIELD(rarg);
	WRITE_NODE_FIELD(colTypes);
	WRITE_NODE_FIELD(colTypmods);
	WRITE_NODE_FIELD(colCollations);
	WRITE_NODE_FIELD(groupClauses);
}

static void
_outRangeTblEntry(StringInfo str, const RangeTblEntry *node)
{
	WRITE_NODE_TYPE("RTE");

	/* put alias + eref first to make dump more legible */
	WRITE_NODE_FIELD(alias);
	WRITE_NODE_FIELD(eref);
	WRITE_ENUM_FIELD(rtekind, RTEKind);

	switch (node->rtekind)
	{
		case RTE_RELATION:
			WRITE_OID_FIELD(relid);
			WRITE_CHAR_FIELD(relkind);
			WRITE_INT_FIELD(rellockmode);
			WRITE_NODE_FIELD(tablesample);
			break;
		case RTE_SUBQUERY:
			WRITE_NODE_FIELD(subquery);
			WRITE_BOOL_FIELD(security_barrier);
			break;
		case RTE_JOIN:
			WRITE_ENUM_FIELD(jointype, JoinType);
			WRITE_INT_FIELD(joinmergedcols);
			WRITE_NODE_FIELD(joinaliasvars);
			WRITE_NODE_FIELD(joinleftcols);
			WRITE_NODE_FIELD(joinrightcols);
			WRITE_NODE_FIELD(join_using_alias);
			break;
		case RTE_FUNCTION:
			WRITE_NODE_FIELD(functions);
			WRITE_BOOL_FIELD(funcordinality);
			break;
		case RTE_TABLEFUNC:
			WRITE_NODE_FIELD(tablefunc);
			break;
		case RTE_VALUES:
			WRITE_NODE_FIELD(values_lists);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_CTE:
			WRITE_STRING_FIELD(ctename);
			WRITE_UINT_FIELD(ctelevelsup);
			WRITE_BOOL_FIELD(self_reference);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_NAMEDTUPLESTORE:
			WRITE_STRING_FIELD(enrname);
			WRITE_FLOAT_FIELD(enrtuples, "%.0f");
			WRITE_OID_FIELD(relid);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_RESULT:
			/* no extra fields */
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) node->rtekind);
			break;
	}

	WRITE_BOOL_FIELD(lateral);
	WRITE_BOOL_FIELD(inh);
	WRITE_BOOL_FIELD(inFromCl);
	WRITE_UINT_FIELD(requiredPerms);
	WRITE_OID_FIELD(checkAsUser);
	WRITE_BITMAPSET_FIELD(selectedCols);
	WRITE_BITMAPSET_FIELD(insertedCols);
	WRITE_BITMAPSET_FIELD(updatedCols);
	WRITE_BITMAPSET_FIELD(extraUpdatedCols);
	WRITE_NODE_FIELD(securityQuals);
}

static void
_outRangeTblFunction(StringInfo str, const RangeTblFunction *node)
{
	WRITE_NODE_TYPE("RANGETBLFUNCTION");

	WRITE_NODE_FIELD(funcexpr);
	WRITE_INT_FIELD(funccolcount);
	WRITE_NODE_FIELD(funccolnames);
	WRITE_NODE_FIELD(funccoltypes);
	WRITE_NODE_FIELD(funccoltypmods);
	WRITE_NODE_FIELD(funccolcollations);
	WRITE_BITMAPSET_FIELD(funcparams);
}

static void
_outTableSampleClause(StringInfo str, const TableSampleClause *node)
{
	WRITE_NODE_TYPE("TABLESAMPLECLAUSE");

	WRITE_OID_FIELD(tsmhandler);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(repeatable);
}

static void
_outA_Expr(StringInfo str, const A_Expr *node)
{
	WRITE_NODE_TYPE("AEXPR");

	switch (node->kind)
	{
		case AEXPR_OP:
			appendStringInfoChar(str, ' ');
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_OP_ANY:
			appendStringInfoChar(str, ' ');
			WRITE_NODE_FIELD(name);
			appendStringInfoString(str, " ANY ");
			break;
		case AEXPR_OP_ALL:
			appendStringInfoChar(str, ' ');
			WRITE_NODE_FIELD(name);
			appendStringInfoString(str, " ALL ");
			break;
		case AEXPR_DISTINCT:
			appendStringInfoString(str, " DISTINCT ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_DISTINCT:
			appendStringInfoString(str, " NOT_DISTINCT ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NULLIF:
			appendStringInfoString(str, " NULLIF ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_IN:
			appendStringInfoString(str, " IN ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_LIKE:
			appendStringInfoString(str, " LIKE ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_ILIKE:
			appendStringInfoString(str, " ILIKE ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_SIMILAR:
			appendStringInfoString(str, " SIMILAR ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN:
			appendStringInfoString(str, " BETWEEN ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN:
			appendStringInfoString(str, " NOT_BETWEEN ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN_SYM:
			appendStringInfoString(str, " BETWEEN_SYM ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN_SYM:
			appendStringInfoString(str, " NOT_BETWEEN_SYM ");
			WRITE_NODE_FIELD(name);
			break;
		default:
			appendStringInfoString(str, " ??");
			break;
	}

	WRITE_NODE_FIELD(lexpr);
	WRITE_NODE_FIELD(rexpr);
	WRITE_LOCATION_FIELD(location);
}

static void
_outValue(StringInfo str, const Value *node)
{
	switch (node->type)
	{
		case T_Integer:
			appendStringInfo(str, "%d", node->val.ival);
			break;
		case T_Float:

			/*
			 * We assume the value is a valid numeric literal and so does not
			 * need quoting.
			 */
			appendStringInfoString(str, node->val.str);
			break;
		case T_String:

			/*
			 * We use outToken to provide escaping of the string's content,
			 * but we don't want it to do anything with an empty string.
			 */
			appendStringInfoChar(str, '"');
			if (node->val.str[0] != '\0')
				outToken(str, node->val.str);
			appendStringInfoChar(str, '"');
			break;
		case T_BitString:
			/* internal representation already has leading 'b' */
			appendStringInfoString(str, node->val.str);
			break;
		case T_Null:
			/* this is seen only within A_Const, not in transformed trees */
			appendStringInfoString(str, "NULL");
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) node->type);
			break;
	}
}

static void
_outColumnRef(StringInfo str, const ColumnRef *node)
{
	WRITE_NODE_TYPE("COLUMNREF");

	WRITE_NODE_FIELD(fields);
	WRITE_LOCATION_FIELD(location);
}

static void
_outParamRef(StringInfo str, const ParamRef *node)
{
	WRITE_NODE_TYPE("PARAMREF");

	WRITE_INT_FIELD(number);
	WRITE_LOCATION_FIELD(location);
}

/*
 * Node types found in raw parse trees (supported for debug purposes)
 */

static void
_outRawStmt(StringInfo str, const RawStmt *node)
{
	WRITE_NODE_TYPE("RAWSTMT");

	WRITE_NODE_FIELD(stmt);
	WRITE_LOCATION_FIELD(stmt_location);
	WRITE_INT_FIELD(stmt_len);
}

static void
_outA_Const(StringInfo str, const A_Const *node)
{
	WRITE_NODE_TYPE("A_CONST");

	appendStringInfoString(str, " :val ");
	_outValue(str, &(node->val));
	WRITE_LOCATION_FIELD(location);
}

static void
_outA_Star(StringInfo str, const A_Star *node)
{
	WRITE_NODE_TYPE("A_STAR");
}

static void
_outA_Indices(StringInfo str, const A_Indices *node)
{
	WRITE_NODE_TYPE("A_INDICES");

	WRITE_BOOL_FIELD(is_slice);
	WRITE_NODE_FIELD(lidx);
	WRITE_NODE_FIELD(uidx);
}

static void
_outA_Indirection(StringInfo str, const A_Indirection *node)
{
	WRITE_NODE_TYPE("A_INDIRECTION");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(indirection);
}

static void
_outA_ArrayExpr(StringInfo str, const A_ArrayExpr *node)
{
	WRITE_NODE_TYPE("A_ARRAYEXPR");

	WRITE_NODE_FIELD(elements);
	WRITE_LOCATION_FIELD(location);
}

static void
_outResTarget(StringInfo str, const ResTarget *node)
{
	WRITE_NODE_TYPE("RESTARGET");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(indirection);
	WRITE_NODE_FIELD(val);
	WRITE_LOCATION_FIELD(location);
}

static void
_outMultiAssignRef(StringInfo str, const MultiAssignRef *node)
{
	WRITE_NODE_TYPE("MULTIASSIGNREF");

	WRITE_NODE_FIELD(source);
	WRITE_INT_FIELD(colno);
	WRITE_INT_FIELD(ncolumns);
}

static void
_outSortBy(StringInfo str, const SortBy *node)
{
	WRITE_NODE_TYPE("SORTBY");

	WRITE_NODE_FIELD(node);
	WRITE_ENUM_FIELD(sortby_dir, SortByDir);
	WRITE_ENUM_FIELD(sortby_nulls, SortByNulls);
	WRITE_NODE_FIELD(useOp);
	WRITE_LOCATION_FIELD(location);
}

static void
_outWindowDef(StringInfo str, const WindowDef *node)
{
	WRITE_NODE_TYPE("WINDOWDEF");

	WRITE_STRING_FIELD(name);
	WRITE_STRING_FIELD(refname);
	WRITE_NODE_FIELD(partitionClause);
	WRITE_NODE_FIELD(orderClause);
	WRITE_INT_FIELD(frameOptions);
	WRITE_NODE_FIELD(startOffset);
	WRITE_NODE_FIELD(endOffset);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRangeSubselect(StringInfo str, const RangeSubselect *node)
{
	WRITE_NODE_TYPE("RANGESUBSELECT");

	WRITE_BOOL_FIELD(lateral);
	WRITE_NODE_FIELD(subquery);
	WRITE_NODE_FIELD(alias);
}

static void
_outRangeFunction(StringInfo str, const RangeFunction *node)
{
	WRITE_NODE_TYPE("RANGEFUNCTION");

	WRITE_BOOL_FIELD(lateral);
	WRITE_BOOL_FIELD(ordinality);
	WRITE_BOOL_FIELD(is_rowsfrom);
	WRITE_NODE_FIELD(functions);
	WRITE_NODE_FIELD(alias);
	WRITE_NODE_FIELD(coldeflist);
}

static void
_outRangeTableSample(StringInfo str, const RangeTableSample *node)
{
	WRITE_NODE_TYPE("RANGETABLESAMPLE");

	WRITE_NODE_FIELD(relation);
	WRITE_NODE_FIELD(method);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(repeatable);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRangeTableFunc(StringInfo str, const RangeTableFunc *node)
{
	WRITE_NODE_TYPE("RANGETABLEFUNC");

	WRITE_BOOL_FIELD(lateral);
	WRITE_NODE_FIELD(docexpr);
	WRITE_NODE_FIELD(rowexpr);
	WRITE_NODE_FIELD(namespaces);
	WRITE_NODE_FIELD(columns);
	WRITE_NODE_FIELD(alias);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRangeTableFuncCol(StringInfo str, const RangeTableFuncCol *node)
{
	WRITE_NODE_TYPE("RANGETABLEFUNCCOL");

	WRITE_STRING_FIELD(colname);
	WRITE_NODE_FIELD(typeName);
	WRITE_BOOL_FIELD(for_ordinality);
	WRITE_BOOL_FIELD(is_not_null);
	WRITE_NODE_FIELD(colexpr);
	WRITE_NODE_FIELD(coldefexpr);
	WRITE_LOCATION_FIELD(location);
}

static void
_outConstraint(StringInfo str, const Constraint *node)
{
	WRITE_NODE_TYPE("CONSTRAINT");

	WRITE_STRING_FIELD(conname);
	WRITE_BOOL_FIELD(deferrable);
	WRITE_BOOL_FIELD(initdeferred);
	WRITE_LOCATION_FIELD(location);

	appendStringInfoString(str, " :contype ");
	switch (node->contype)
	{
		case CONSTR_NULL:
			appendStringInfoString(str, "NULL");
			break;

		case CONSTR_NOTNULL:
			appendStringInfoString(str, "NOT_NULL");
			break;

		case CONSTR_DEFAULT:
			appendStringInfoString(str, "DEFAULT");
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			break;

		case CONSTR_IDENTITY:
			appendStringInfoString(str, "IDENTITY");
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			WRITE_CHAR_FIELD(generated_when);
			break;

		case CONSTR_GENERATED:
			appendStringInfoString(str, "GENERATED");
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			WRITE_CHAR_FIELD(generated_when);
			break;

		case CONSTR_CHECK:
			appendStringInfoString(str, "CHECK");
			WRITE_BOOL_FIELD(is_no_inherit);
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			break;

		case CONSTR_PRIMARY:
			appendStringInfoString(str, "PRIMARY_KEY");
			WRITE_NODE_FIELD(keys);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			/* access_method and where_clause not currently used */
			break;

		case CONSTR_UNIQUE:
			appendStringInfoString(str, "UNIQUE");
			WRITE_NODE_FIELD(keys);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			/* access_method and where_clause not currently used */
			break;

		case CONSTR_EXCLUSION:
			appendStringInfoString(str, "EXCLUSION");
			WRITE_NODE_FIELD(exclusions);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			WRITE_STRING_FIELD(access_method);
			WRITE_NODE_FIELD(where_clause);
			break;

		case CONSTR_FOREIGN:
			appendStringInfoString(str, "FOREIGN_KEY");
			WRITE_NODE_FIELD(pktable);
			WRITE_NODE_FIELD(fk_attrs);
			WRITE_NODE_FIELD(pk_attrs);
			WRITE_CHAR_FIELD(fk_matchtype);
			WRITE_CHAR_FIELD(fk_upd_action);
			WRITE_CHAR_FIELD(fk_del_action);
			WRITE_NODE_FIELD(old_conpfeqop);
			WRITE_OID_FIELD(old_pktable_oid);
			WRITE_BOOL_FIELD(skip_validation);
			WRITE_BOOL_FIELD(initially_valid);
			break;

		case CONSTR_ATTR_DEFERRABLE:
			appendStringInfoString(str, "ATTR_DEFERRABLE");
			break;

		case CONSTR_ATTR_NOT_DEFERRABLE:
			appendStringInfoString(str, "ATTR_NOT_DEFERRABLE");
			break;

		case CONSTR_ATTR_DEFERRED:
			appendStringInfoString(str, "ATTR_DEFERRED");
			break;

		case CONSTR_ATTR_IMMEDIATE:
			appendStringInfoString(str, "ATTR_IMMEDIATE");
			break;

		default:
			appendStringInfo(str, "<unrecognized_constraint %d>",
							 (int) node->contype);
			break;
	}
}

static void
_outForeignKeyCacheInfo(StringInfo str, const ForeignKeyCacheInfo *node)
{
	WRITE_NODE_TYPE("FOREIGNKEYCACHEINFO");

	WRITE_OID_FIELD(conoid);
	WRITE_OID_FIELD(conrelid);
	WRITE_OID_FIELD(confrelid);
	WRITE_INT_FIELD(nkeys);
	WRITE_ATTRNUMBER_ARRAY(conkey, node->nkeys);
	WRITE_ATTRNUMBER_ARRAY(confkey, node->nkeys);
	WRITE_OID_ARRAY(conpfeqop, node->nkeys);
}

static void
_outPartitionElem(StringInfo str, const PartitionElem *node)
{
	WRITE_NODE_TYPE("PARTITIONELEM");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(expr);
	WRITE_NODE_FIELD(collation);
	WRITE_NODE_FIELD(opclass);
	WRITE_LOCATION_FIELD(location);
}

static void
_outPartitionSpec(StringInfo str, const PartitionSpec *node)
{
	WRITE_NODE_TYPE("PARTITIONSPEC");

	WRITE_STRING_FIELD(strategy);
	WRITE_NODE_FIELD(partParams);
	WRITE_LOCATION_FIELD(location);
}

static void
_outPartitionBoundSpec(StringInfo str, const PartitionBoundSpec *node)
{
	WRITE_NODE_TYPE("PARTITIONBOUNDSPEC");

	WRITE_CHAR_FIELD(strategy);
	WRITE_BOOL_FIELD(is_default);
	WRITE_INT_FIELD(modulus);
	WRITE_INT_FIELD(remainder);
	WRITE_NODE_FIELD(listdatums);
	WRITE_NODE_FIELD(lowerdatums);
	WRITE_NODE_FIELD(upperdatums);
	WRITE_LOCATION_FIELD(location);
}

static void
_outPartitionRangeDatum(StringInfo str, const PartitionRangeDatum *node)
{
	WRITE_NODE_TYPE("PARTITIONRANGEDATUM");

	WRITE_ENUM_FIELD(kind, PartitionRangeDatumKind);
	WRITE_NODE_FIELD(value);
	WRITE_LOCATION_FIELD(location);
}

/*
 * outNode -
 *	  converts a Node into ascii string and append it to 'str'
 */
void
outNode(StringInfo str, const void *obj)
{
	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();
=======
	WRITE_NODE_TYPE("WINDOWAGG");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_UINT_FIELD(winref);
	WRITE_INT_FIELD(partNumCols);
	WRITE_ATTRNUMBER_ARRAY(partColIdx, node->partNumCols);
	WRITE_OID_ARRAY(partOperators, node->partNumCols);
	WRITE_OID_ARRAY(partCollations, node->partNumCols);
	WRITE_INT_FIELD(ordNumCols);
	WRITE_ATTRNUMBER_ARRAY(ordColIdx, node->ordNumCols);
	WRITE_OID_ARRAY(ordOperators, node->ordNumCols);
	WRITE_OID_ARRAY(ordCollations, node->ordNumCols);
	WRITE_INT_FIELD(frameOptions);
	WRITE_NODE_FIELD(startOffset);
	WRITE_NODE_FIELD(endOffset);
	WRITE_OID_FIELD(startInRangeFunc);
	WRITE_OID_FIELD(endInRangeFunc);
	WRITE_OID_FIELD(inRangeColl);
	WRITE_BOOL_FIELD(inRangeAsc);
	WRITE_BOOL_FIELD(inRangeNullsFirst);
}

static void
_outGroup(StringInfo str, const Group *node)
{
	WRITE_NODE_TYPE("GROUP");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(grpColIdx, node->numCols);
	WRITE_OID_ARRAY(grpOperators, node->numCols);
	WRITE_OID_ARRAY(grpCollations, node->numCols);
}

static void
_outMaterial(StringInfo str, const Material *node)
{
	WRITE_NODE_TYPE("MATERIAL");

	_outPlanInfo(str, (const Plan *) node);
}

static void
_outMemoize(StringInfo str, const Memoize *node)
{
	WRITE_NODE_TYPE("MEMOIZE");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numKeys);
	WRITE_OID_ARRAY(hashOperators, node->numKeys);
	WRITE_OID_ARRAY(collations, node->numKeys);
	WRITE_NODE_FIELD(param_exprs);
	WRITE_BOOL_FIELD(singlerow);
	WRITE_UINT_FIELD(est_entries);
}

static void
_outSortInfo(StringInfo str, const Sort *node)
{
	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(sortColIdx, node->numCols);
	WRITE_OID_ARRAY(sortOperators, node->numCols);
	WRITE_OID_ARRAY(collations, node->numCols);
	WRITE_BOOL_ARRAY(nullsFirst, node->numCols);
}

static void
_outSort(StringInfo str, const Sort *node)
{
	WRITE_NODE_TYPE("SORT");

	_outSortInfo(str, node);
}

static void
_outIncrementalSort(StringInfo str, const IncrementalSort *node)
{
	WRITE_NODE_TYPE("INCREMENTALSORT");

	_outSortInfo(str, (const Sort *) node);

	WRITE_INT_FIELD(nPresortedCols);
}

static void
_outUnique(StringInfo str, const Unique *node)
{
	WRITE_NODE_TYPE("UNIQUE");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(uniqColIdx, node->numCols);
	WRITE_OID_ARRAY(uniqOperators, node->numCols);
	WRITE_OID_ARRAY(uniqCollations, node->numCols);
}

static void
_outHash(StringInfo str, const Hash *node)
{
	WRITE_NODE_TYPE("HASH");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(hashkeys);
	WRITE_OID_FIELD(skewTable);
	WRITE_INT_FIELD(skewColumn);
	WRITE_BOOL_FIELD(skewInherit);
	WRITE_FLOAT_FIELD(rows_total, "%.0f");
}

static void
_outSetOp(StringInfo str, const SetOp *node)
{
	WRITE_NODE_TYPE("SETOP");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_ENUM_FIELD(cmd, SetOpCmd);
	WRITE_ENUM_FIELD(strategy, SetOpStrategy);
	WRITE_INT_FIELD(numCols);
	WRITE_ATTRNUMBER_ARRAY(dupColIdx, node->numCols);
	WRITE_OID_ARRAY(dupOperators, node->numCols);
	WRITE_OID_ARRAY(dupCollations, node->numCols);
	WRITE_INT_FIELD(flagColIdx);
	WRITE_INT_FIELD(firstFlag);
	WRITE_LONG_FIELD(numGroups);
}

static void
_outLockRows(StringInfo str, const LockRows *node)
{
	WRITE_NODE_TYPE("LOCKROWS");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(rowMarks);
	WRITE_INT_FIELD(epqParam);
}

static void
_outLimit(StringInfo str, const Limit *node)
{
	WRITE_NODE_TYPE("LIMIT");

	_outPlanInfo(str, (const Plan *) node);

	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
	WRITE_INT_FIELD(uniqNumCols);
	WRITE_ATTRNUMBER_ARRAY(uniqColIdx, node->uniqNumCols);
	WRITE_OID_ARRAY(uniqOperators, node->uniqNumCols);
	WRITE_OID_ARRAY(uniqCollations, node->uniqNumCols);
}

static void
_outNestLoopParam(StringInfo str, const NestLoopParam *node)
{
	WRITE_NODE_TYPE("NESTLOOPPARAM");

	WRITE_INT_FIELD(paramno);
	WRITE_NODE_FIELD(paramval);
}

static void
_outPlanRowMark(StringInfo str, const PlanRowMark *node)
{
	WRITE_NODE_TYPE("PLANROWMARK");

	WRITE_UINT_FIELD(rti);
	WRITE_UINT_FIELD(prti);
	WRITE_UINT_FIELD(rowmarkId);
	WRITE_ENUM_FIELD(markType, RowMarkType);
	WRITE_INT_FIELD(allMarkTypes);
	WRITE_ENUM_FIELD(strength, LockClauseStrength);
	WRITE_ENUM_FIELD(waitPolicy, LockWaitPolicy);
	WRITE_BOOL_FIELD(isParent);
}

static void
_outPartitionPruneInfo(StringInfo str, const PartitionPruneInfo *node)
{
	WRITE_NODE_TYPE("PARTITIONPRUNEINFO");

	WRITE_NODE_FIELD(prune_infos);
	WRITE_BITMAPSET_FIELD(other_subplans);
}

static void
_outPartitionedRelPruneInfo(StringInfo str, const PartitionedRelPruneInfo *node)
{
	WRITE_NODE_TYPE("PARTITIONEDRELPRUNEINFO");

	WRITE_UINT_FIELD(rtindex);
	WRITE_BITMAPSET_FIELD(present_parts);
	WRITE_INT_FIELD(nparts);
	WRITE_INT_ARRAY(subplan_map, node->nparts);
	WRITE_INT_ARRAY(subpart_map, node->nparts);
	WRITE_OID_ARRAY(relid_map, node->nparts);
	WRITE_NODE_FIELD(initial_pruning_steps);
	WRITE_NODE_FIELD(exec_pruning_steps);
	WRITE_BITMAPSET_FIELD(execparamids);
}

static void
_outPartitionPruneStepOp(StringInfo str, const PartitionPruneStepOp *node)
{
	WRITE_NODE_TYPE("PARTITIONPRUNESTEPOP");

	WRITE_INT_FIELD(step.step_id);
	WRITE_INT_FIELD(opstrategy);
	WRITE_NODE_FIELD(exprs);
	WRITE_NODE_FIELD(cmpfns);
	WRITE_BITMAPSET_FIELD(nullkeys);
}

static void
_outPartitionPruneStepCombine(StringInfo str, const PartitionPruneStepCombine *node)
{
	WRITE_NODE_TYPE("PARTITIONPRUNESTEPCOMBINE");

	WRITE_INT_FIELD(step.step_id);
	WRITE_ENUM_FIELD(combineOp, PartitionPruneCombineOp);
	WRITE_NODE_FIELD(source_stepids);
}

static void
_outPlanInvalItem(StringInfo str, const PlanInvalItem *node)
{
	WRITE_NODE_TYPE("PLANINVALITEM");

	WRITE_INT_FIELD(cacheId);
	WRITE_UINT_FIELD(hashValue);
}

/*****************************************************************************
 *
 *	Stuff from primnodes.h.
 *
 *****************************************************************************/

static void
_outAlias(StringInfo str, const Alias *node)
{
	WRITE_NODE_TYPE("ALIAS");

	WRITE_STRING_FIELD(aliasname);
	WRITE_NODE_FIELD(colnames);
}

static void
_outRangeVar(StringInfo str, const RangeVar *node)
{
	WRITE_NODE_TYPE("RANGEVAR");

	/*
	 * we deliberately ignore catalogname here, since it is presently not
	 * semantically meaningful
	 */
	WRITE_STRING_FIELD(schemaname);
	WRITE_STRING_FIELD(relname);
	WRITE_BOOL_FIELD(inh);
	WRITE_CHAR_FIELD(relpersistence);
	WRITE_NODE_FIELD(alias);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTableFunc(StringInfo str, const TableFunc *node)
{
	WRITE_NODE_TYPE("TABLEFUNC");

	WRITE_NODE_FIELD(ns_uris);
	WRITE_NODE_FIELD(ns_names);
	WRITE_NODE_FIELD(docexpr);
	WRITE_NODE_FIELD(rowexpr);
	WRITE_NODE_FIELD(colnames);
	WRITE_NODE_FIELD(coltypes);
	WRITE_NODE_FIELD(coltypmods);
	WRITE_NODE_FIELD(colcollations);
	WRITE_NODE_FIELD(colexprs);
	WRITE_NODE_FIELD(coldefexprs);
	WRITE_BITMAPSET_FIELD(notnulls);
	WRITE_INT_FIELD(ordinalitycol);
	WRITE_LOCATION_FIELD(location);
}

static void
_outIntoClause(StringInfo str, const IntoClause *node)
{
	WRITE_NODE_TYPE("INTOCLAUSE");

	WRITE_NODE_FIELD(rel);
	WRITE_NODE_FIELD(colNames);
	WRITE_STRING_FIELD(accessMethod);
	WRITE_NODE_FIELD(options);
	WRITE_ENUM_FIELD(onCommit, OnCommitAction);
	WRITE_STRING_FIELD(tableSpaceName);
	WRITE_NODE_FIELD(viewQuery);
	WRITE_BOOL_FIELD(skipData);
}

static void
_outVar(StringInfo str, const Var *node)
{
	WRITE_NODE_TYPE("VAR");

	WRITE_UINT_FIELD(varno);
	WRITE_INT_FIELD(varattno);
	WRITE_OID_FIELD(vartype);
	WRITE_INT_FIELD(vartypmod);
	WRITE_OID_FIELD(varcollid);
	WRITE_UINT_FIELD(varlevelsup);
	WRITE_UINT_FIELD(varnosyn);
	WRITE_INT_FIELD(varattnosyn);
	WRITE_LOCATION_FIELD(location);
}

static void
_outConst(StringInfo str, const Const *node)
{
	WRITE_NODE_TYPE("CONST");

	WRITE_OID_FIELD(consttype);
	WRITE_INT_FIELD(consttypmod);
	WRITE_OID_FIELD(constcollid);
	WRITE_INT_FIELD(constlen);
	WRITE_BOOL_FIELD(constbyval);
	WRITE_BOOL_FIELD(constisnull);
	WRITE_LOCATION_FIELD(location);

	appendStringInfoString(str, " :constvalue ");
	if (node->constisnull)
		appendStringInfoString(str, "<>");
	else
		outDatum(str, node->constvalue, node->constlen, node->constbyval);
}

static void
_outParam(StringInfo str, const Param *node)
{
	WRITE_NODE_TYPE("PARAM");

	WRITE_ENUM_FIELD(paramkind, ParamKind);
	WRITE_INT_FIELD(paramid);
	WRITE_OID_FIELD(paramtype);
	WRITE_INT_FIELD(paramtypmod);
	WRITE_OID_FIELD(paramcollid);
	WRITE_LOCATION_FIELD(location);
}

static void
_outAggref(StringInfo str, const Aggref *node)
{
	WRITE_NODE_TYPE("AGGREF");

	WRITE_OID_FIELD(aggfnoid);
	WRITE_OID_FIELD(aggtype);
	WRITE_OID_FIELD(aggcollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_OID_FIELD(aggtranstype);
	WRITE_NODE_FIELD(aggargtypes);
	WRITE_NODE_FIELD(aggdirectargs);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(aggorder);
	WRITE_NODE_FIELD(aggdistinct);
	WRITE_NODE_FIELD(aggfilter);
	WRITE_BOOL_FIELD(aggstar);
	WRITE_BOOL_FIELD(aggvariadic);
	WRITE_CHAR_FIELD(aggkind);
	WRITE_UINT_FIELD(agglevelsup);
	WRITE_ENUM_FIELD(aggsplit, AggSplit);
	WRITE_INT_FIELD(aggno);
	WRITE_INT_FIELD(aggtransno);
	WRITE_LOCATION_FIELD(location);
}

static void
_outGroupingFunc(StringInfo str, const GroupingFunc *node)
{
	WRITE_NODE_TYPE("GROUPINGFUNC");

	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(refs);
	WRITE_NODE_FIELD(cols);
	WRITE_UINT_FIELD(agglevelsup);
	WRITE_LOCATION_FIELD(location);
}

static void
_outWindowFunc(StringInfo str, const WindowFunc *node)
{
	WRITE_NODE_TYPE("WINDOWFUNC");

	WRITE_OID_FIELD(winfnoid);
	WRITE_OID_FIELD(wintype);
	WRITE_OID_FIELD(wincollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(aggfilter);
	WRITE_UINT_FIELD(winref);
	WRITE_BOOL_FIELD(winstar);
	WRITE_BOOL_FIELD(winagg);
	WRITE_LOCATION_FIELD(location);
}

static void
_outSubscriptingRef(StringInfo str, const SubscriptingRef *node)
{
	WRITE_NODE_TYPE("SUBSCRIPTINGREF");

	WRITE_OID_FIELD(refcontainertype);
	WRITE_OID_FIELD(refelemtype);
	WRITE_OID_FIELD(refrestype);
	WRITE_INT_FIELD(reftypmod);
	WRITE_OID_FIELD(refcollid);
	WRITE_NODE_FIELD(refupperindexpr);
	WRITE_NODE_FIELD(reflowerindexpr);
	WRITE_NODE_FIELD(refexpr);
	WRITE_NODE_FIELD(refassgnexpr);
}

static void
_outFuncExpr(StringInfo str, const FuncExpr *node)
{
	WRITE_NODE_TYPE("FUNCEXPR");

	WRITE_OID_FIELD(funcid);
	WRITE_OID_FIELD(funcresulttype);
	WRITE_BOOL_FIELD(funcretset);
	WRITE_BOOL_FIELD(funcvariadic);
	WRITE_ENUM_FIELD(funcformat, CoercionForm);
	WRITE_OID_FIELD(funccollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outNamedArgExpr(StringInfo str, const NamedArgExpr *node)
{
	WRITE_NODE_TYPE("NAMEDARGEXPR");

	WRITE_NODE_FIELD(arg);
	WRITE_STRING_FIELD(name);
	WRITE_INT_FIELD(argnumber);
	WRITE_LOCATION_FIELD(location);
}

static void
_outOpExpr(StringInfo str, const OpExpr *node)
{
	WRITE_NODE_TYPE("OPEXPR");

	WRITE_OID_FIELD(opno);
	WRITE_OID_FIELD(opfuncid);
	WRITE_OID_FIELD(opresulttype);
	WRITE_BOOL_FIELD(opretset);
	WRITE_OID_FIELD(opcollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outDistinctExpr(StringInfo str, const DistinctExpr *node)
{
	WRITE_NODE_TYPE("DISTINCTEXPR");

	WRITE_OID_FIELD(opno);
	WRITE_OID_FIELD(opfuncid);
	WRITE_OID_FIELD(opresulttype);
	WRITE_BOOL_FIELD(opretset);
	WRITE_OID_FIELD(opcollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outNullIfExpr(StringInfo str, const NullIfExpr *node)
{
	WRITE_NODE_TYPE("NULLIFEXPR");

	WRITE_OID_FIELD(opno);
	WRITE_OID_FIELD(opfuncid);
	WRITE_OID_FIELD(opresulttype);
	WRITE_BOOL_FIELD(opretset);
	WRITE_OID_FIELD(opcollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outScalarArrayOpExpr(StringInfo str, const ScalarArrayOpExpr *node)
{
	WRITE_NODE_TYPE("SCALARARRAYOPEXPR");

	WRITE_OID_FIELD(opno);
	WRITE_OID_FIELD(opfuncid);
	WRITE_OID_FIELD(hashfuncid);
	WRITE_OID_FIELD(negfuncid);
	WRITE_BOOL_FIELD(useOr);
	WRITE_OID_FIELD(inputcollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outBoolExpr(StringInfo str, const BoolExpr *node)
{
	char	   *opstr = NULL;

	WRITE_NODE_TYPE("BOOLEXPR");

	/* do-it-yourself enum representation */
	switch (node->boolop)
	{
		case AND_EXPR:
			opstr = "and";
			break;
		case OR_EXPR:
			opstr = "or";
			break;
		case NOT_EXPR:
			opstr = "not";
			break;
	}
	appendStringInfoString(str, " :boolop ");
	outToken(str, opstr);

	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outSubLink(StringInfo str, const SubLink *node)
{
	WRITE_NODE_TYPE("SUBLINK");

	WRITE_ENUM_FIELD(subLinkType, SubLinkType);
	WRITE_INT_FIELD(subLinkId);
	WRITE_NODE_FIELD(testexpr);
	WRITE_NODE_FIELD(operName);
	WRITE_NODE_FIELD(subselect);
	WRITE_LOCATION_FIELD(location);
}

static void
_outSubPlan(StringInfo str, const SubPlan *node)
{
	WRITE_NODE_TYPE("SUBPLAN");

	WRITE_ENUM_FIELD(subLinkType, SubLinkType);
	WRITE_NODE_FIELD(testexpr);
	WRITE_NODE_FIELD(paramIds);
	WRITE_INT_FIELD(plan_id);
	WRITE_STRING_FIELD(plan_name);
	WRITE_OID_FIELD(firstColType);
	WRITE_INT_FIELD(firstColTypmod);
	WRITE_OID_FIELD(firstColCollation);
	WRITE_BOOL_FIELD(useHashTable);
	WRITE_BOOL_FIELD(unknownEqFalse);
	WRITE_BOOL_FIELD(parallel_safe);
	WRITE_NODE_FIELD(setParam);
	WRITE_NODE_FIELD(parParam);
	WRITE_NODE_FIELD(args);
	WRITE_FLOAT_FIELD(startup_cost, "%.2f");
	WRITE_FLOAT_FIELD(per_call_cost, "%.2f");
}

static void
_outAlternativeSubPlan(StringInfo str, const AlternativeSubPlan *node)
{
	WRITE_NODE_TYPE("ALTERNATIVESUBPLAN");

	WRITE_NODE_FIELD(subplans);
}

static void
_outFieldSelect(StringInfo str, const FieldSelect *node)
{
	WRITE_NODE_TYPE("FIELDSELECT");

	WRITE_NODE_FIELD(arg);
	WRITE_INT_FIELD(fieldnum);
	WRITE_OID_FIELD(resulttype);
	WRITE_INT_FIELD(resulttypmod);
	WRITE_OID_FIELD(resultcollid);
}

static void
_outFieldStore(StringInfo str, const FieldStore *node)
{
	WRITE_NODE_TYPE("FIELDSTORE");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(newvals);
	WRITE_NODE_FIELD(fieldnums);
	WRITE_OID_FIELD(resulttype);
}

static void
_outRelabelType(StringInfo str, const RelabelType *node)
{
	WRITE_NODE_TYPE("RELABELTYPE");

	WRITE_NODE_FIELD(arg);
	WRITE_OID_FIELD(resulttype);
	WRITE_INT_FIELD(resulttypmod);
	WRITE_OID_FIELD(resultcollid);
	WRITE_ENUM_FIELD(relabelformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCoerceViaIO(StringInfo str, const CoerceViaIO *node)
{
	WRITE_NODE_TYPE("COERCEVIAIO");

	WRITE_NODE_FIELD(arg);
	WRITE_OID_FIELD(resulttype);
	WRITE_OID_FIELD(resultcollid);
	WRITE_ENUM_FIELD(coerceformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outArrayCoerceExpr(StringInfo str, const ArrayCoerceExpr *node)
{
	WRITE_NODE_TYPE("ARRAYCOERCEEXPR");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(elemexpr);
	WRITE_OID_FIELD(resulttype);
	WRITE_INT_FIELD(resulttypmod);
	WRITE_OID_FIELD(resultcollid);
	WRITE_ENUM_FIELD(coerceformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outConvertRowtypeExpr(StringInfo str, const ConvertRowtypeExpr *node)
{
	WRITE_NODE_TYPE("CONVERTROWTYPEEXPR");

	WRITE_NODE_FIELD(arg);
	WRITE_OID_FIELD(resulttype);
	WRITE_ENUM_FIELD(convertformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCollateExpr(StringInfo str, const CollateExpr *node)
{
	WRITE_NODE_TYPE("COLLATE");

	WRITE_NODE_FIELD(arg);
	WRITE_OID_FIELD(collOid);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCaseExpr(StringInfo str, const CaseExpr *node)
{
	WRITE_NODE_TYPE("CASE");

	WRITE_OID_FIELD(casetype);
	WRITE_OID_FIELD(casecollid);
	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(defresult);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCaseWhen(StringInfo str, const CaseWhen *node)
{
	WRITE_NODE_TYPE("WHEN");

	WRITE_NODE_FIELD(expr);
	WRITE_NODE_FIELD(result);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCaseTestExpr(StringInfo str, const CaseTestExpr *node)
{
	WRITE_NODE_TYPE("CASETESTEXPR");

	WRITE_OID_FIELD(typeId);
	WRITE_INT_FIELD(typeMod);
	WRITE_OID_FIELD(collation);
}

static void
_outArrayExpr(StringInfo str, const ArrayExpr *node)
{
	WRITE_NODE_TYPE("ARRAY");

	WRITE_OID_FIELD(array_typeid);
	WRITE_OID_FIELD(array_collid);
	WRITE_OID_FIELD(element_typeid);
	WRITE_NODE_FIELD(elements);
	WRITE_BOOL_FIELD(multidims);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRowExpr(StringInfo str, const RowExpr *node)
{
	WRITE_NODE_TYPE("ROW");

	WRITE_NODE_FIELD(args);
	WRITE_OID_FIELD(row_typeid);
	WRITE_ENUM_FIELD(row_format, CoercionForm);
	WRITE_NODE_FIELD(colnames);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRowCompareExpr(StringInfo str, const RowCompareExpr *node)
{
	WRITE_NODE_TYPE("ROWCOMPARE");

	WRITE_ENUM_FIELD(rctype, RowCompareType);
	WRITE_NODE_FIELD(opnos);
	WRITE_NODE_FIELD(opfamilies);
	WRITE_NODE_FIELD(inputcollids);
	WRITE_NODE_FIELD(largs);
	WRITE_NODE_FIELD(rargs);
}

static void
_outCoalesceExpr(StringInfo str, const CoalesceExpr *node)
{
	WRITE_NODE_TYPE("COALESCE");

	WRITE_OID_FIELD(coalescetype);
	WRITE_OID_FIELD(coalescecollid);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outMinMaxExpr(StringInfo str, const MinMaxExpr *node)
{
	WRITE_NODE_TYPE("MINMAX");

	WRITE_OID_FIELD(minmaxtype);
	WRITE_OID_FIELD(minmaxcollid);
	WRITE_OID_FIELD(inputcollid);
	WRITE_ENUM_FIELD(op, MinMaxOp);
	WRITE_NODE_FIELD(args);
	WRITE_LOCATION_FIELD(location);
}

static void
_outSQLValueFunction(StringInfo str, const SQLValueFunction *node)
{
	WRITE_NODE_TYPE("SQLVALUEFUNCTION");

	WRITE_ENUM_FIELD(op, SQLValueFunctionOp);
	WRITE_OID_FIELD(type);
	WRITE_INT_FIELD(typmod);
	WRITE_LOCATION_FIELD(location);
}

static void
_outXmlExpr(StringInfo str, const XmlExpr *node)
{
	WRITE_NODE_TYPE("XMLEXPR");

	WRITE_ENUM_FIELD(op, XmlExprOp);
	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(named_args);
	WRITE_NODE_FIELD(arg_names);
	WRITE_NODE_FIELD(args);
	WRITE_ENUM_FIELD(xmloption, XmlOptionType);
	WRITE_OID_FIELD(type);
	WRITE_INT_FIELD(typmod);
	WRITE_LOCATION_FIELD(location);
}

static void
_outNullTest(StringInfo str, const NullTest *node)
{
	WRITE_NODE_TYPE("NULLTEST");

	WRITE_NODE_FIELD(arg);
	WRITE_ENUM_FIELD(nulltesttype, NullTestType);
	WRITE_BOOL_FIELD(argisrow);
	WRITE_LOCATION_FIELD(location);
}

static void
_outBooleanTest(StringInfo str, const BooleanTest *node)
{
	WRITE_NODE_TYPE("BOOLEANTEST");

	WRITE_NODE_FIELD(arg);
	WRITE_ENUM_FIELD(booltesttype, BoolTestType);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCoerceToDomain(StringInfo str, const CoerceToDomain *node)
{
	WRITE_NODE_TYPE("COERCETODOMAIN");

	WRITE_NODE_FIELD(arg);
	WRITE_OID_FIELD(resulttype);
	WRITE_INT_FIELD(resulttypmod);
	WRITE_OID_FIELD(resultcollid);
	WRITE_ENUM_FIELD(coercionformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCoerceToDomainValue(StringInfo str, const CoerceToDomainValue *node)
{
	WRITE_NODE_TYPE("COERCETODOMAINVALUE");

	WRITE_OID_FIELD(typeId);
	WRITE_INT_FIELD(typeMod);
	WRITE_OID_FIELD(collation);
	WRITE_LOCATION_FIELD(location);
}

static void
_outSetToDefault(StringInfo str, const SetToDefault *node)
{
	WRITE_NODE_TYPE("SETTODEFAULT");

	WRITE_OID_FIELD(typeId);
	WRITE_INT_FIELD(typeMod);
	WRITE_OID_FIELD(collation);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCurrentOfExpr(StringInfo str, const CurrentOfExpr *node)
{
	WRITE_NODE_TYPE("CURRENTOFEXPR");

	WRITE_UINT_FIELD(cvarno);
	WRITE_STRING_FIELD(cursor_name);
	WRITE_INT_FIELD(cursor_param);
}

static void
_outNextValueExpr(StringInfo str, const NextValueExpr *node)
{
	WRITE_NODE_TYPE("NEXTVALUEEXPR");

	WRITE_OID_FIELD(seqid);
	WRITE_OID_FIELD(typeId);
}

static void
_outInferenceElem(StringInfo str, const InferenceElem *node)
{
	WRITE_NODE_TYPE("INFERENCEELEM");

	WRITE_NODE_FIELD(expr);
	WRITE_OID_FIELD(infercollid);
	WRITE_OID_FIELD(inferopclass);
}

static void
_outTargetEntry(StringInfo str, const TargetEntry *node)
{
	WRITE_NODE_TYPE("TARGETENTRY");

	WRITE_NODE_FIELD(expr);
	WRITE_INT_FIELD(resno);
	WRITE_STRING_FIELD(resname);
	WRITE_UINT_FIELD(ressortgroupref);
	WRITE_OID_FIELD(resorigtbl);
	WRITE_INT_FIELD(resorigcol);
	WRITE_BOOL_FIELD(resjunk);
}

static void
_outRangeTblRef(StringInfo str, const RangeTblRef *node)
{
	WRITE_NODE_TYPE("RANGETBLREF");

	WRITE_INT_FIELD(rtindex);
}

static void
_outJoinExpr(StringInfo str, const JoinExpr *node)
{
	WRITE_NODE_TYPE("JOINEXPR");

	WRITE_ENUM_FIELD(jointype, JoinType);
	WRITE_BOOL_FIELD(isNatural);
	WRITE_NODE_FIELD(larg);
	WRITE_NODE_FIELD(rarg);
	WRITE_NODE_FIELD(usingClause);
	WRITE_NODE_FIELD(join_using_alias);
	WRITE_NODE_FIELD(quals);
	WRITE_NODE_FIELD(alias);
	WRITE_INT_FIELD(rtindex);
}

static void
_outFromExpr(StringInfo str, const FromExpr *node)
{
	WRITE_NODE_TYPE("FROMEXPR");

	WRITE_NODE_FIELD(fromlist);
	WRITE_NODE_FIELD(quals);
}

static void
_outOnConflictExpr(StringInfo str, const OnConflictExpr *node)
{
	WRITE_NODE_TYPE("ONCONFLICTEXPR");

	WRITE_ENUM_FIELD(action, OnConflictAction);
	WRITE_NODE_FIELD(arbiterElems);
	WRITE_NODE_FIELD(arbiterWhere);
	WRITE_OID_FIELD(constraint);
	WRITE_NODE_FIELD(onConflictSet);
	WRITE_NODE_FIELD(onConflictWhere);
	WRITE_INT_FIELD(exclRelIndex);
	WRITE_NODE_FIELD(exclRelTlist);
}

/*****************************************************************************
 *
 *	Stuff from pathnodes.h.
 *
 *****************************************************************************/

/*
 * print the basic stuff of all nodes that inherit from Path
 *
 * Note we do NOT print the parent, else we'd be in infinite recursion.
 * We can print the parent's relids for identification purposes, though.
 * We print the pathtarget only if it's not the default one for the rel.
 * We also do not print the whole of param_info, since it's printed by
 * _outRelOptInfo; it's sufficient and less cluttering to print just the
 * required outer relids.
 */
static void
_outPathInfo(StringInfo str, const Path *node)
{
	WRITE_ENUM_FIELD(pathtype, NodeTag);
	appendStringInfoString(str, " :parent_relids ");
	outBitmapset(str, node->parent->relids);
	if (node->pathtarget != node->parent->reltarget)
		WRITE_NODE_FIELD(pathtarget);
	appendStringInfoString(str, " :required_outer ");
	if (node->param_info)
		outBitmapset(str, node->param_info->ppi_req_outer);
	else
		outBitmapset(str, NULL);
	WRITE_BOOL_FIELD(parallel_aware);
	WRITE_BOOL_FIELD(parallel_safe);
	WRITE_INT_FIELD(parallel_workers);
	WRITE_FLOAT_FIELD(rows, "%.0f");
	WRITE_FLOAT_FIELD(startup_cost, "%.2f");
	WRITE_FLOAT_FIELD(total_cost, "%.2f");
	WRITE_NODE_FIELD(pathkeys);
}

/*
 * print the basic stuff of all nodes that inherit from JoinPath
 */
static void
_outJoinPathInfo(StringInfo str, const JoinPath *node)
{
	_outPathInfo(str, (const Path *) node);

	WRITE_ENUM_FIELD(jointype, JoinType);
	WRITE_BOOL_FIELD(inner_unique);
	WRITE_NODE_FIELD(outerjoinpath);
	WRITE_NODE_FIELD(innerjoinpath);
	WRITE_NODE_FIELD(joinrestrictinfo);
}

static void
_outPath(StringInfo str, const Path *node)
{
	WRITE_NODE_TYPE("PATH");

	_outPathInfo(str, (const Path *) node);
}

static void
_outIndexPath(StringInfo str, const IndexPath *node)
{
	WRITE_NODE_TYPE("INDEXPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(indexinfo);
	WRITE_NODE_FIELD(indexclauses);
	WRITE_NODE_FIELD(indexorderbys);
	WRITE_NODE_FIELD(indexorderbycols);
	WRITE_ENUM_FIELD(indexscandir, ScanDirection);
	WRITE_FLOAT_FIELD(indextotalcost, "%.2f");
	WRITE_FLOAT_FIELD(indexselectivity, "%.4f");
}

static void
_outBitmapHeapPath(StringInfo str, const BitmapHeapPath *node)
{
	WRITE_NODE_TYPE("BITMAPHEAPPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(bitmapqual);
}

static void
_outBitmapAndPath(StringInfo str, const BitmapAndPath *node)
{
	WRITE_NODE_TYPE("BITMAPANDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(bitmapquals);
	WRITE_FLOAT_FIELD(bitmapselectivity, "%.4f");
}

static void
_outBitmapOrPath(StringInfo str, const BitmapOrPath *node)
{
	WRITE_NODE_TYPE("BITMAPORPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(bitmapquals);
	WRITE_FLOAT_FIELD(bitmapselectivity, "%.4f");
}

static void
_outTidPath(StringInfo str, const TidPath *node)
{
	WRITE_NODE_TYPE("TIDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(tidquals);
}

static void
_outTidRangePath(StringInfo str, const TidRangePath *node)
{
	WRITE_NODE_TYPE("TIDRANGEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(tidrangequals);
}

static void
_outSubqueryScanPath(StringInfo str, const SubqueryScanPath *node)
{
	WRITE_NODE_TYPE("SUBQUERYSCANPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outForeignPath(StringInfo str, const ForeignPath *node)
{
	WRITE_NODE_TYPE("FOREIGNPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(fdw_outerpath);
	WRITE_NODE_FIELD(fdw_private);
}

static void
_outCustomPath(StringInfo str, const CustomPath *node)
{
	WRITE_NODE_TYPE("CUSTOMPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_UINT_FIELD(flags);
	WRITE_NODE_FIELD(custom_paths);
	WRITE_NODE_FIELD(custom_private);
	appendStringInfoString(str, " :methods ");
	outToken(str, node->methods->CustomName);
}

static void
_outAppendPath(StringInfo str, const AppendPath *node)
{
	WRITE_NODE_TYPE("APPENDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpaths);
	WRITE_INT_FIELD(first_partial_path);
	WRITE_FLOAT_FIELD(limit_tuples, "%.0f");
}

static void
_outMergeAppendPath(StringInfo str, const MergeAppendPath *node)
{
	WRITE_NODE_TYPE("MERGEAPPENDPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpaths);
	WRITE_FLOAT_FIELD(limit_tuples, "%.0f");
}

static void
_outGroupResultPath(StringInfo str, const GroupResultPath *node)
{
	WRITE_NODE_TYPE("GROUPRESULTPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(quals);
}

static void
_outMaterialPath(StringInfo str, const MaterialPath *node)
{
	WRITE_NODE_TYPE("MATERIALPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outMemoizePath(StringInfo str, const MemoizePath *node)
{
	WRITE_NODE_TYPE("MEMOIZEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(hash_operators);
	WRITE_NODE_FIELD(param_exprs);
	WRITE_BOOL_FIELD(singlerow);
	WRITE_FLOAT_FIELD(calls, "%.0f");
	WRITE_UINT_FIELD(est_entries);
}

static void
_outUniquePath(StringInfo str, const UniquePath *node)
{
	WRITE_NODE_TYPE("UNIQUEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(umethod, UniquePathMethod);
	WRITE_NODE_FIELD(in_operators);
	WRITE_NODE_FIELD(uniq_exprs);
}

static void
_outGatherPath(StringInfo str, const GatherPath *node)
{
	WRITE_NODE_TYPE("GATHERPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_BOOL_FIELD(single_copy);
	WRITE_INT_FIELD(num_workers);
}

static void
_outProjectionPath(StringInfo str, const ProjectionPath *node)
{
	WRITE_NODE_TYPE("PROJECTIONPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_BOOL_FIELD(dummypp);
}

static void
_outProjectSetPath(StringInfo str, const ProjectSetPath *node)
{
	WRITE_NODE_TYPE("PROJECTSETPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outSortPathInfo(StringInfo str, const SortPath *node)
{
	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
}

static void
_outSortPath(StringInfo str, const SortPath *node)
{
	WRITE_NODE_TYPE("SORTPATH");

	_outSortPathInfo(str, node);
}

static void
_outIncrementalSortPath(StringInfo str, const IncrementalSortPath *node)
{
	WRITE_NODE_TYPE("INCREMENTALSORTPATH");

	_outSortPathInfo(str, (const SortPath *) node);

	WRITE_INT_FIELD(nPresortedCols);
}

static void
_outGroupPath(StringInfo str, const GroupPath *node)
{
	WRITE_NODE_TYPE("GROUPPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(groupClause);
	WRITE_NODE_FIELD(qual);
}

static void
_outUpperUniquePath(StringInfo str, const UpperUniquePath *node)
{
	WRITE_NODE_TYPE("UPPERUNIQUEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_INT_FIELD(numkeys);
}

static void
_outAggPath(StringInfo str, const AggPath *node)
{
	WRITE_NODE_TYPE("AGGPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(aggstrategy, AggStrategy);
	WRITE_ENUM_FIELD(aggsplit, AggSplit);
	WRITE_FLOAT_FIELD(numGroups, "%.0f");
	WRITE_UINT64_FIELD(transitionSpace);
	WRITE_NODE_FIELD(groupClause);
	WRITE_NODE_FIELD(qual);
}

static void
_outRollupData(StringInfo str, const RollupData *node)
{
	WRITE_NODE_TYPE("ROLLUP");

	WRITE_NODE_FIELD(groupClause);
	WRITE_NODE_FIELD(gsets);
	WRITE_NODE_FIELD(gsets_data);
	WRITE_FLOAT_FIELD(numGroups, "%.0f");
	WRITE_BOOL_FIELD(hashable);
	WRITE_BOOL_FIELD(is_hashed);
}

static void
_outGroupingSetData(StringInfo str, const GroupingSetData *node)
{
	WRITE_NODE_TYPE("GSDATA");

	WRITE_NODE_FIELD(set);
	WRITE_FLOAT_FIELD(numGroups, "%.0f");
}

static void
_outGroupingSetsPath(StringInfo str, const GroupingSetsPath *node)
{
	WRITE_NODE_TYPE("GROUPINGSETSPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(aggstrategy, AggStrategy);
	WRITE_NODE_FIELD(rollups);
	WRITE_NODE_FIELD(qual);
	WRITE_UINT64_FIELD(transitionSpace);
}

static void
_outMinMaxAggPath(StringInfo str, const MinMaxAggPath *node)
{
	WRITE_NODE_TYPE("MINMAXAGGPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(mmaggregates);
	WRITE_NODE_FIELD(quals);
}

static void
_outWindowAggPath(StringInfo str, const WindowAggPath *node)
{
	WRITE_NODE_TYPE("WINDOWAGGPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(winclause);
}

static void
_outSetOpPath(StringInfo str, const SetOpPath *node)
{
	WRITE_NODE_TYPE("SETOPPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(cmd, SetOpCmd);
	WRITE_ENUM_FIELD(strategy, SetOpStrategy);
	WRITE_NODE_FIELD(distinctList);
	WRITE_INT_FIELD(flagColIdx);
	WRITE_INT_FIELD(firstFlag);
	WRITE_FLOAT_FIELD(numGroups, "%.0f");
}

static void
_outRecursiveUnionPath(StringInfo str, const RecursiveUnionPath *node)
{
	WRITE_NODE_TYPE("RECURSIVEUNIONPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(leftpath);
	WRITE_NODE_FIELD(rightpath);
	WRITE_NODE_FIELD(distinctList);
	WRITE_INT_FIELD(wtParam);
	WRITE_FLOAT_FIELD(numGroups, "%.0f");
}

static void
_outLockRowsPath(StringInfo str, const LockRowsPath *node)
{
	WRITE_NODE_TYPE("LOCKROWSPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_INT_FIELD(epqParam);
}

static void
_outModifyTablePath(StringInfo str, const ModifyTablePath *node)
{
	WRITE_NODE_TYPE("MODIFYTABLEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_ENUM_FIELD(operation, CmdType);
	WRITE_BOOL_FIELD(canSetTag);
	WRITE_UINT_FIELD(nominalRelation);
	WRITE_UINT_FIELD(rootRelation);
	WRITE_BOOL_FIELD(partColsUpdated);
	WRITE_NODE_FIELD(resultRelations);
	WRITE_NODE_FIELD(updateColnosLists);
	WRITE_NODE_FIELD(withCheckOptionLists);
	WRITE_NODE_FIELD(returningLists);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(onconflict);
	WRITE_INT_FIELD(epqParam);
}

static void
_outLimitPath(StringInfo str, const LimitPath *node)
{
	WRITE_NODE_TYPE("LIMITPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
}

static void
_outGatherMergePath(StringInfo str, const GatherMergePath *node)
{
	WRITE_NODE_TYPE("GATHERMERGEPATH");

	_outPathInfo(str, (const Path *) node);

	WRITE_NODE_FIELD(subpath);
	WRITE_INT_FIELD(num_workers);
}

static void
_outNestPath(StringInfo str, const NestPath *node)
{
	WRITE_NODE_TYPE("NESTPATH");

	_outJoinPathInfo(str, (const JoinPath *) node);
}

static void
_outMergePath(StringInfo str, const MergePath *node)
{
	WRITE_NODE_TYPE("MERGEPATH");

	_outJoinPathInfo(str, (const JoinPath *) node);

	WRITE_NODE_FIELD(path_mergeclauses);
	WRITE_NODE_FIELD(outersortkeys);
	WRITE_NODE_FIELD(innersortkeys);
	WRITE_BOOL_FIELD(skip_mark_restore);
	WRITE_BOOL_FIELD(materialize_inner);
}

static void
_outHashPath(StringInfo str, const HashPath *node)
{
	WRITE_NODE_TYPE("HASHPATH");

	_outJoinPathInfo(str, (const JoinPath *) node);

	WRITE_NODE_FIELD(path_hashclauses);
	WRITE_INT_FIELD(num_batches);
	WRITE_FLOAT_FIELD(inner_rows_total, "%.0f");
}

static void
_outPlannerGlobal(StringInfo str, const PlannerGlobal *node)
{
	WRITE_NODE_TYPE("PLANNERGLOBAL");

	/* NB: this isn't a complete set of fields */
	WRITE_NODE_FIELD(subplans);
	WRITE_BITMAPSET_FIELD(rewindPlanIDs);
	WRITE_NODE_FIELD(finalrtable);
	WRITE_NODE_FIELD(finalrowmarks);
	WRITE_NODE_FIELD(resultRelations);
	WRITE_NODE_FIELD(appendRelations);
	WRITE_NODE_FIELD(relationOids);
	WRITE_NODE_FIELD(invalItems);
	WRITE_NODE_FIELD(paramExecTypes);
	WRITE_UINT_FIELD(lastPHId);
	WRITE_UINT_FIELD(lastRowMarkId);
	WRITE_INT_FIELD(lastPlanNodeId);
	WRITE_BOOL_FIELD(transientPlan);
	WRITE_BOOL_FIELD(dependsOnRole);
	WRITE_BOOL_FIELD(parallelModeOK);
	WRITE_BOOL_FIELD(parallelModeNeeded);
	WRITE_CHAR_FIELD(maxParallelHazard);
}

static void
_outPlannerInfo(StringInfo str, const PlannerInfo *node)
{
	WRITE_NODE_TYPE("PLANNERINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_NODE_FIELD(parse);
	WRITE_NODE_FIELD(glob);
	WRITE_UINT_FIELD(query_level);
	WRITE_NODE_FIELD(plan_params);
	WRITE_BITMAPSET_FIELD(outer_params);
	WRITE_BITMAPSET_FIELD(all_baserels);
	WRITE_BITMAPSET_FIELD(nullable_baserels);
	WRITE_NODE_FIELD(join_rel_list);
	WRITE_INT_FIELD(join_cur_level);
	WRITE_NODE_FIELD(init_plans);
	WRITE_NODE_FIELD(cte_plan_ids);
	WRITE_NODE_FIELD(multiexpr_params);
	WRITE_NODE_FIELD(eq_classes);
	WRITE_BOOL_FIELD(ec_merging_done);
	WRITE_NODE_FIELD(canon_pathkeys);
	WRITE_NODE_FIELD(left_join_clauses);
	WRITE_NODE_FIELD(right_join_clauses);
	WRITE_NODE_FIELD(full_join_clauses);
	WRITE_NODE_FIELD(join_info_list);
	WRITE_BITMAPSET_FIELD(all_result_relids);
	WRITE_BITMAPSET_FIELD(leaf_result_relids);
	WRITE_NODE_FIELD(append_rel_list);
	WRITE_NODE_FIELD(row_identity_vars);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(placeholder_list);
	WRITE_NODE_FIELD(fkey_list);
	WRITE_NODE_FIELD(query_pathkeys);
	WRITE_NODE_FIELD(group_pathkeys);
	WRITE_NODE_FIELD(window_pathkeys);
	WRITE_NODE_FIELD(distinct_pathkeys);
	WRITE_NODE_FIELD(sort_pathkeys);
	WRITE_NODE_FIELD(processed_tlist);
	WRITE_NODE_FIELD(update_colnos);
	WRITE_NODE_FIELD(minmax_aggs);
	WRITE_FLOAT_FIELD(total_table_pages, "%.0f");
	WRITE_FLOAT_FIELD(tuple_fraction, "%.4f");
	WRITE_FLOAT_FIELD(limit_tuples, "%.0f");
	WRITE_UINT_FIELD(qual_security_level);
	WRITE_BOOL_FIELD(hasJoinRTEs);
	WRITE_BOOL_FIELD(hasLateralRTEs);
	WRITE_BOOL_FIELD(hasHavingQual);
	WRITE_BOOL_FIELD(hasPseudoConstantQuals);
	WRITE_BOOL_FIELD(hasAlternativeSubPlans);
	WRITE_BOOL_FIELD(hasRecursion);
	WRITE_INT_FIELD(wt_param_id);
	WRITE_BITMAPSET_FIELD(curOuterRels);
	WRITE_NODE_FIELD(curOuterParams);
	WRITE_BOOL_FIELD(partColsUpdated);
}

static void
_outRelOptInfo(StringInfo str, const RelOptInfo *node)
{
	WRITE_NODE_TYPE("RELOPTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_ENUM_FIELD(reloptkind, RelOptKind);
	WRITE_BITMAPSET_FIELD(relids);
	WRITE_FLOAT_FIELD(rows, "%.0f");
	WRITE_BOOL_FIELD(consider_startup);
	WRITE_BOOL_FIELD(consider_param_startup);
	WRITE_BOOL_FIELD(consider_parallel);
	WRITE_NODE_FIELD(reltarget);
	WRITE_NODE_FIELD(pathlist);
	WRITE_NODE_FIELD(ppilist);
	WRITE_NODE_FIELD(partial_pathlist);
	WRITE_NODE_FIELD(cheapest_startup_path);
	WRITE_NODE_FIELD(cheapest_total_path);
	WRITE_NODE_FIELD(cheapest_unique_path);
	WRITE_NODE_FIELD(cheapest_parameterized_paths);
	WRITE_BITMAPSET_FIELD(direct_lateral_relids);
	WRITE_BITMAPSET_FIELD(lateral_relids);
	WRITE_UINT_FIELD(relid);
	WRITE_OID_FIELD(reltablespace);
	WRITE_ENUM_FIELD(rtekind, RTEKind);
	WRITE_INT_FIELD(min_attr);
	WRITE_INT_FIELD(max_attr);
	WRITE_NODE_FIELD(lateral_vars);
	WRITE_BITMAPSET_FIELD(lateral_referencers);
	WRITE_NODE_FIELD(indexlist);
	WRITE_NODE_FIELD(statlist);
	WRITE_UINT_FIELD(pages);
	WRITE_FLOAT_FIELD(tuples, "%.0f");
	WRITE_FLOAT_FIELD(allvisfrac, "%.6f");
	WRITE_BITMAPSET_FIELD(eclass_indexes);
	WRITE_NODE_FIELD(subroot);
	WRITE_NODE_FIELD(subplan_params);
	WRITE_INT_FIELD(rel_parallel_workers);
	WRITE_UINT_FIELD(amflags);
	WRITE_OID_FIELD(serverid);
	WRITE_OID_FIELD(userid);
	WRITE_BOOL_FIELD(useridiscurrent);
	/* we don't try to print fdwroutine or fdw_private */
	/* can't print unique_for_rels/non_unique_for_rels; BMSes aren't Nodes */
	WRITE_NODE_FIELD(baserestrictinfo);
	WRITE_UINT_FIELD(baserestrict_min_security);
	WRITE_NODE_FIELD(joininfo);
	WRITE_BOOL_FIELD(has_eclass_joins);
	WRITE_BOOL_FIELD(consider_partitionwise_join);
	WRITE_BITMAPSET_FIELD(top_parent_relids);
	WRITE_BOOL_FIELD(partbounds_merged);
	WRITE_BITMAPSET_FIELD(live_parts);
	WRITE_BITMAPSET_FIELD(all_partrels);
}

static void
_outIndexOptInfo(StringInfo str, const IndexOptInfo *node)
{
	WRITE_NODE_TYPE("INDEXOPTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_OID_FIELD(indexoid);
	/* Do NOT print rel field, else infinite recursion */
	WRITE_UINT_FIELD(pages);
	WRITE_FLOAT_FIELD(tuples, "%.0f");
	WRITE_INT_FIELD(tree_height);
	WRITE_INT_FIELD(ncolumns);
	/* array fields aren't really worth the trouble to print */
	WRITE_OID_FIELD(relam);
	/* indexprs is redundant since we print indextlist */
	WRITE_NODE_FIELD(indpred);
	WRITE_NODE_FIELD(indextlist);
	WRITE_NODE_FIELD(indrestrictinfo);
	WRITE_BOOL_FIELD(predOK);
	WRITE_BOOL_FIELD(unique);
	WRITE_BOOL_FIELD(immediate);
	WRITE_BOOL_FIELD(hypothetical);
	/* we don't bother with fields copied from the index AM's API struct */
}

static void
_outForeignKeyOptInfo(StringInfo str, const ForeignKeyOptInfo *node)
{
	int			i;

	WRITE_NODE_TYPE("FOREIGNKEYOPTINFO");

	WRITE_UINT_FIELD(con_relid);
	WRITE_UINT_FIELD(ref_relid);
	WRITE_INT_FIELD(nkeys);
	WRITE_ATTRNUMBER_ARRAY(conkey, node->nkeys);
	WRITE_ATTRNUMBER_ARRAY(confkey, node->nkeys);
	WRITE_OID_ARRAY(conpfeqop, node->nkeys);
	WRITE_INT_FIELD(nmatched_ec);
	WRITE_INT_FIELD(nconst_ec);
	WRITE_INT_FIELD(nmatched_rcols);
	WRITE_INT_FIELD(nmatched_ri);
	/* for compactness, just print the number of matches per column: */
	appendStringInfoString(str, " :eclass");
	for (i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", (node->eclass[i] != NULL));
	appendStringInfoString(str, " :rinfos");
	for (i = 0; i < node->nkeys; i++)
		appendStringInfo(str, " %d", list_length(node->rinfos[i]));
}

static void
_outStatisticExtInfo(StringInfo str, const StatisticExtInfo *node)
{
	WRITE_NODE_TYPE("STATISTICEXTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_OID_FIELD(statOid);
	/* don't write rel, leads to infinite recursion in plan tree dump */
	WRITE_CHAR_FIELD(kind);
	WRITE_BITMAPSET_FIELD(keys);
}

static void
_outEquivalenceClass(StringInfo str, const EquivalenceClass *node)
{
	/*
	 * To simplify reading, we just chase up to the topmost merged EC and
	 * print that, without bothering to show the merge-ees separately.
	 */
	while (node->ec_merged)
		node = node->ec_merged;

	WRITE_NODE_TYPE("EQUIVALENCECLASS");

	WRITE_NODE_FIELD(ec_opfamilies);
	WRITE_OID_FIELD(ec_collation);
	WRITE_NODE_FIELD(ec_members);
	WRITE_NODE_FIELD(ec_sources);
	WRITE_NODE_FIELD(ec_derives);
	WRITE_BITMAPSET_FIELD(ec_relids);
	WRITE_BOOL_FIELD(ec_has_const);
	WRITE_BOOL_FIELD(ec_has_volatile);
	WRITE_BOOL_FIELD(ec_below_outer_join);
	WRITE_BOOL_FIELD(ec_broken);
	WRITE_UINT_FIELD(ec_sortref);
	WRITE_UINT_FIELD(ec_min_security);
	WRITE_UINT_FIELD(ec_max_security);
}

static void
_outEquivalenceMember(StringInfo str, const EquivalenceMember *node)
{
	WRITE_NODE_TYPE("EQUIVALENCEMEMBER");

	WRITE_NODE_FIELD(em_expr);
	WRITE_BITMAPSET_FIELD(em_relids);
	WRITE_BITMAPSET_FIELD(em_nullable_relids);
	WRITE_BOOL_FIELD(em_is_const);
	WRITE_BOOL_FIELD(em_is_child);
	WRITE_OID_FIELD(em_datatype);
}

static void
_outPathKey(StringInfo str, const PathKey *node)
{
	WRITE_NODE_TYPE("PATHKEY");

	WRITE_NODE_FIELD(pk_eclass);
	WRITE_OID_FIELD(pk_opfamily);
	WRITE_INT_FIELD(pk_strategy);
	WRITE_BOOL_FIELD(pk_nulls_first);
}

static void
_outPathTarget(StringInfo str, const PathTarget *node)
{
	WRITE_NODE_TYPE("PATHTARGET");

	WRITE_NODE_FIELD(exprs);
	if (node->sortgrouprefs)
	{
		int			i;

		appendStringInfoString(str, " :sortgrouprefs");
		for (i = 0; i < list_length(node->exprs); i++)
			appendStringInfo(str, " %u", node->sortgrouprefs[i]);
	}
	WRITE_FLOAT_FIELD(cost.startup, "%.2f");
	WRITE_FLOAT_FIELD(cost.per_tuple, "%.2f");
	WRITE_INT_FIELD(width);
	WRITE_ENUM_FIELD(has_volatile_expr, VolatileFunctionStatus);
}

static void
_outParamPathInfo(StringInfo str, const ParamPathInfo *node)
{
	WRITE_NODE_TYPE("PARAMPATHINFO");

	WRITE_BITMAPSET_FIELD(ppi_req_outer);
	WRITE_FLOAT_FIELD(ppi_rows, "%.0f");
	WRITE_NODE_FIELD(ppi_clauses);
}

static void
_outRestrictInfo(StringInfo str, const RestrictInfo *node)
{
	WRITE_NODE_TYPE("RESTRICTINFO");

	/* NB: this isn't a complete set of fields */
	WRITE_NODE_FIELD(clause);
	WRITE_BOOL_FIELD(is_pushed_down);
	WRITE_BOOL_FIELD(outerjoin_delayed);
	WRITE_BOOL_FIELD(can_join);
	WRITE_BOOL_FIELD(pseudoconstant);
	WRITE_BOOL_FIELD(leakproof);
	WRITE_ENUM_FIELD(has_volatile, VolatileFunctionStatus);
	WRITE_UINT_FIELD(security_level);
	WRITE_BITMAPSET_FIELD(clause_relids);
	WRITE_BITMAPSET_FIELD(required_relids);
	WRITE_BITMAPSET_FIELD(outer_relids);
	WRITE_BITMAPSET_FIELD(nullable_relids);
	WRITE_BITMAPSET_FIELD(left_relids);
	WRITE_BITMAPSET_FIELD(right_relids);
	WRITE_NODE_FIELD(orclause);
	/* don't write parent_ec, leads to infinite recursion in plan tree dump */
	WRITE_FLOAT_FIELD(norm_selec, "%.4f");
	WRITE_FLOAT_FIELD(outer_selec, "%.4f");
	WRITE_NODE_FIELD(mergeopfamilies);
	/* don't write left_ec, leads to infinite recursion in plan tree dump */
	/* don't write right_ec, leads to infinite recursion in plan tree dump */
	WRITE_NODE_FIELD(left_em);
	WRITE_NODE_FIELD(right_em);
	WRITE_BOOL_FIELD(outer_is_left);
	WRITE_OID_FIELD(hashjoinoperator);
	WRITE_OID_FIELD(hasheqoperator);
}

static void
_outIndexClause(StringInfo str, const IndexClause *node)
{
	WRITE_NODE_TYPE("INDEXCLAUSE");

	WRITE_NODE_FIELD(rinfo);
	WRITE_NODE_FIELD(indexquals);
	WRITE_BOOL_FIELD(lossy);
	WRITE_INT_FIELD(indexcol);
	WRITE_NODE_FIELD(indexcols);
}

static void
_outPlaceHolderVar(StringInfo str, const PlaceHolderVar *node)
{
	WRITE_NODE_TYPE("PLACEHOLDERVAR");

	WRITE_NODE_FIELD(phexpr);
	WRITE_BITMAPSET_FIELD(phrels);
	WRITE_UINT_FIELD(phid);
	WRITE_UINT_FIELD(phlevelsup);
}

static void
_outSpecialJoinInfo(StringInfo str, const SpecialJoinInfo *node)
{
	WRITE_NODE_TYPE("SPECIALJOININFO");

	WRITE_BITMAPSET_FIELD(min_lefthand);
	WRITE_BITMAPSET_FIELD(min_righthand);
	WRITE_BITMAPSET_FIELD(syn_lefthand);
	WRITE_BITMAPSET_FIELD(syn_righthand);
	WRITE_ENUM_FIELD(jointype, JoinType);
	WRITE_BOOL_FIELD(lhs_strict);
	WRITE_BOOL_FIELD(delay_upper_joins);
	WRITE_BOOL_FIELD(semi_can_btree);
	WRITE_BOOL_FIELD(semi_can_hash);
	WRITE_NODE_FIELD(semi_operators);
	WRITE_NODE_FIELD(semi_rhs_exprs);
}

static void
_outAppendRelInfo(StringInfo str, const AppendRelInfo *node)
{
	WRITE_NODE_TYPE("APPENDRELINFO");

	WRITE_UINT_FIELD(parent_relid);
	WRITE_UINT_FIELD(child_relid);
	WRITE_OID_FIELD(parent_reltype);
	WRITE_OID_FIELD(child_reltype);
	WRITE_NODE_FIELD(translated_vars);
	WRITE_INT_FIELD(num_child_cols);
	WRITE_ATTRNUMBER_ARRAY(parent_colnos, node->num_child_cols);
	WRITE_OID_FIELD(parent_reloid);
}

static void
_outRowIdentityVarInfo(StringInfo str, const RowIdentityVarInfo *node)
{
	WRITE_NODE_TYPE("ROWIDENTITYVARINFO");

	WRITE_NODE_FIELD(rowidvar);
	WRITE_INT_FIELD(rowidwidth);
	WRITE_STRING_FIELD(rowidname);
	WRITE_BITMAPSET_FIELD(rowidrels);
}

static void
_outPlaceHolderInfo(StringInfo str, const PlaceHolderInfo *node)
{
	WRITE_NODE_TYPE("PLACEHOLDERINFO");

	WRITE_UINT_FIELD(phid);
	WRITE_NODE_FIELD(ph_var);
	WRITE_BITMAPSET_FIELD(ph_eval_at);
	WRITE_BITMAPSET_FIELD(ph_lateral);
	WRITE_BITMAPSET_FIELD(ph_needed);
	WRITE_INT_FIELD(ph_width);
}

static void
_outMinMaxAggInfo(StringInfo str, const MinMaxAggInfo *node)
{
	WRITE_NODE_TYPE("MINMAXAGGINFO");

	WRITE_OID_FIELD(aggfnoid);
	WRITE_OID_FIELD(aggsortop);
	WRITE_NODE_FIELD(target);
	/* We intentionally omit subroot --- too large, not interesting enough */
	WRITE_NODE_FIELD(path);
	WRITE_FLOAT_FIELD(pathcost, "%.2f");
	WRITE_NODE_FIELD(param);
}

static void
_outPlannerParamItem(StringInfo str, const PlannerParamItem *node)
{
	WRITE_NODE_TYPE("PLANNERPARAMITEM");

	WRITE_NODE_FIELD(item);
	WRITE_INT_FIELD(paramId);
}

/*****************************************************************************
 *
 *	Stuff from extensible.h
 *
 *****************************************************************************/

static void
_outExtensibleNode(StringInfo str, const ExtensibleNode *node)
{
	const ExtensibleNodeMethods *methods;

	methods = GetExtensibleNodeMethods(node->extnodename, false);

	WRITE_NODE_TYPE("EXTENSIBLENODE");

	WRITE_STRING_FIELD(extnodename);

	/* serialize the private fields */
	methods->nodeOut(str, node);
}

/*****************************************************************************
 *
 *	Stuff from parsenodes.h.
 *
 *****************************************************************************/

/*
 * print the basic stuff of all nodes that inherit from CreateStmt
 */
static void
_outCreateStmtInfo(StringInfo str, const CreateStmt *node)
{
	WRITE_NODE_FIELD(relation);
	WRITE_NODE_FIELD(tableElts);
	WRITE_NODE_FIELD(inhRelations);
	WRITE_NODE_FIELD(partspec);
	WRITE_NODE_FIELD(partbound);
	WRITE_NODE_FIELD(ofTypename);
	WRITE_NODE_FIELD(constraints);
	WRITE_NODE_FIELD(options);
	WRITE_ENUM_FIELD(oncommit, OnCommitAction);
	WRITE_STRING_FIELD(tablespacename);
	WRITE_STRING_FIELD(accessMethod);
	WRITE_BOOL_FIELD(if_not_exists);
	WRITE_BOOL_FIELD(systemVersioning);
}

static void
_outCreateStmt(StringInfo str, const CreateStmt *node)
{
	WRITE_NODE_TYPE("CREATESTMT");

	_outCreateStmtInfo(str, (const CreateStmt *) node);
}

static void
_outCreateForeignTableStmt(StringInfo str, const CreateForeignTableStmt *node)
{
	WRITE_NODE_TYPE("CREATEFOREIGNTABLESTMT");

	_outCreateStmtInfo(str, (const CreateStmt *) node);

	WRITE_STRING_FIELD(servername);
	WRITE_NODE_FIELD(options);
}

static void
_outImportForeignSchemaStmt(StringInfo str, const ImportForeignSchemaStmt *node)
{
	WRITE_NODE_TYPE("IMPORTFOREIGNSCHEMASTMT");

	WRITE_STRING_FIELD(server_name);
	WRITE_STRING_FIELD(remote_schema);
	WRITE_STRING_FIELD(local_schema);
	WRITE_ENUM_FIELD(list_type, ImportForeignSchemaType);
	WRITE_NODE_FIELD(table_list);
	WRITE_NODE_FIELD(options);
}

static void
_outIndexStmt(StringInfo str, const IndexStmt *node)
{
	WRITE_NODE_TYPE("INDEXSTMT");

	WRITE_STRING_FIELD(idxname);
	WRITE_NODE_FIELD(relation);
	WRITE_STRING_FIELD(accessMethod);
	WRITE_STRING_FIELD(tableSpace);
	WRITE_NODE_FIELD(indexParams);
	WRITE_NODE_FIELD(indexIncludingParams);
	WRITE_NODE_FIELD(options);
	WRITE_NODE_FIELD(whereClause);
	WRITE_NODE_FIELD(excludeOpNames);
	WRITE_STRING_FIELD(idxcomment);
	WRITE_OID_FIELD(indexOid);
	WRITE_OID_FIELD(oldNode);
	WRITE_UINT_FIELD(oldCreateSubid);
	WRITE_UINT_FIELD(oldFirstRelfilenodeSubid);
	WRITE_BOOL_FIELD(unique);
	WRITE_BOOL_FIELD(primary);
	WRITE_BOOL_FIELD(isconstraint);
	WRITE_BOOL_FIELD(deferrable);
	WRITE_BOOL_FIELD(initdeferred);
	WRITE_BOOL_FIELD(transformed);
	WRITE_BOOL_FIELD(concurrent);
	WRITE_BOOL_FIELD(if_not_exists);
	WRITE_BOOL_FIELD(reset_default_tblspc);
}

static void
_outCreateStatsStmt(StringInfo str, const CreateStatsStmt *node)
{
	WRITE_NODE_TYPE("CREATESTATSSTMT");

	WRITE_NODE_FIELD(defnames);
	WRITE_NODE_FIELD(stat_types);
	WRITE_NODE_FIELD(exprs);
	WRITE_NODE_FIELD(relations);
	WRITE_STRING_FIELD(stxcomment);
	WRITE_BOOL_FIELD(transformed);
	WRITE_BOOL_FIELD(if_not_exists);
}

static void
_outAlterStatsStmt(StringInfo str, const AlterStatsStmt *node)
{
	WRITE_NODE_TYPE("ALTERSTATSSTMT");

	WRITE_NODE_FIELD(defnames);
	WRITE_INT_FIELD(stxstattarget);
	WRITE_BOOL_FIELD(missing_ok);
}

static void
_outNotifyStmt(StringInfo str, const NotifyStmt *node)
{
	WRITE_NODE_TYPE("NOTIFY");

	WRITE_STRING_FIELD(conditionname);
	WRITE_STRING_FIELD(payload);
}

static void
_outDeclareCursorStmt(StringInfo str, const DeclareCursorStmt *node)
{
	WRITE_NODE_TYPE("DECLARECURSOR");

	WRITE_STRING_FIELD(portalname);
	WRITE_INT_FIELD(options);
	WRITE_NODE_FIELD(query);
}

static void
_outSelectStmt(StringInfo str, const SelectStmt *node)
{
	WRITE_NODE_TYPE("SELECT");

	WRITE_NODE_FIELD(distinctClause);
	WRITE_NODE_FIELD(intoClause);
	WRITE_NODE_FIELD(targetList);
	WRITE_NODE_FIELD(fromClause);
	WRITE_NODE_FIELD(whereClause);
	WRITE_NODE_FIELD(groupClause);
	WRITE_BOOL_FIELD(groupDistinct);
	WRITE_NODE_FIELD(havingClause);
	WRITE_NODE_FIELD(windowClause);
	WRITE_NODE_FIELD(valuesLists);
	WRITE_NODE_FIELD(sortClause);
	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
	WRITE_NODE_FIELD(lockingClause);
	WRITE_NODE_FIELD(withClause);
	WRITE_ENUM_FIELD(op, SetOperation);
	WRITE_BOOL_FIELD(all);
	WRITE_NODE_FIELD(larg);
	WRITE_NODE_FIELD(rarg);
}

static void
_outReturnStmt(StringInfo str, const ReturnStmt *node)
{
	WRITE_NODE_TYPE("RETURN");

	WRITE_NODE_FIELD(returnval);
}

static void
_outPLAssignStmt(StringInfo str, const PLAssignStmt *node)
{
	WRITE_NODE_TYPE("PLASSIGN");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(indirection);
	WRITE_INT_FIELD(nnames);
	WRITE_NODE_FIELD(val);
	WRITE_LOCATION_FIELD(location);
}

static void
_outFuncCall(StringInfo str, const FuncCall *node)
{
	WRITE_NODE_TYPE("FUNCCALL");

	WRITE_NODE_FIELD(funcname);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(agg_order);
	WRITE_NODE_FIELD(agg_filter);
	WRITE_NODE_FIELD(over);
	WRITE_BOOL_FIELD(agg_within_group);
	WRITE_BOOL_FIELD(agg_star);
	WRITE_BOOL_FIELD(agg_distinct);
	WRITE_BOOL_FIELD(func_variadic);
	WRITE_ENUM_FIELD(funcformat, CoercionForm);
	WRITE_LOCATION_FIELD(location);
}

static void
_outDefElem(StringInfo str, const DefElem *node)
{
	WRITE_NODE_TYPE("DEFELEM");

	WRITE_STRING_FIELD(defnamespace);
	WRITE_STRING_FIELD(defname);
	WRITE_NODE_FIELD(arg);
	WRITE_ENUM_FIELD(defaction, DefElemAction);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTableLikeClause(StringInfo str, const TableLikeClause *node)
{
	WRITE_NODE_TYPE("TABLELIKECLAUSE");

	WRITE_NODE_FIELD(relation);
	WRITE_UINT_FIELD(options);
	WRITE_OID_FIELD(relationOid);
}

static void
_outLockingClause(StringInfo str, const LockingClause *node)
{
	WRITE_NODE_TYPE("LOCKINGCLAUSE");

	WRITE_NODE_FIELD(lockedRels);
	WRITE_ENUM_FIELD(strength, LockClauseStrength);
	WRITE_ENUM_FIELD(waitPolicy, LockWaitPolicy);
}

static void
_outXmlSerialize(StringInfo str, const XmlSerialize *node)
{
	WRITE_NODE_TYPE("XMLSERIALIZE");

	WRITE_ENUM_FIELD(xmloption, XmlOptionType);
	WRITE_NODE_FIELD(expr);
	WRITE_NODE_FIELD(typeName);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTriggerTransition(StringInfo str, const TriggerTransition *node)
{
	WRITE_NODE_TYPE("TRIGGERTRANSITION");

	WRITE_STRING_FIELD(name);
	WRITE_BOOL_FIELD(isNew);
	WRITE_BOOL_FIELD(isTable);
}

static void
_outColumnDef(StringInfo str, const ColumnDef *node)
{
	WRITE_NODE_TYPE("COLUMNDEF");

	WRITE_STRING_FIELD(colname);
	WRITE_NODE_FIELD(typeName);
	WRITE_STRING_FIELD(compression);
	WRITE_INT_FIELD(inhcount);
	WRITE_BOOL_FIELD(is_local);
	WRITE_BOOL_FIELD(is_not_null);
	WRITE_BOOL_FIELD(is_from_type);
	WRITE_CHAR_FIELD(storage);
	WRITE_NODE_FIELD(raw_default);
	WRITE_NODE_FIELD(cooked_default);
	WRITE_CHAR_FIELD(identity);
	WRITE_NODE_FIELD(identitySequence);
	WRITE_CHAR_FIELD(generated);
	WRITE_NODE_FIELD(collClause);
	WRITE_OID_FIELD(collOid);
	WRITE_NODE_FIELD(constraints);
	WRITE_NODE_FIELD(fdwoptions);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTypeName(StringInfo str, const TypeName *node)
{
	WRITE_NODE_TYPE("TYPENAME");

	WRITE_NODE_FIELD(names);
	WRITE_OID_FIELD(typeOid);
	WRITE_BOOL_FIELD(setof);
	WRITE_BOOL_FIELD(pct_type);
	WRITE_NODE_FIELD(typmods);
	WRITE_INT_FIELD(typemod);
	WRITE_NODE_FIELD(arrayBounds);
	WRITE_LOCATION_FIELD(location);
}

static void
_outTypeCast(StringInfo str, const TypeCast *node)
{
	WRITE_NODE_TYPE("TYPECAST");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(typeName);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCollateClause(StringInfo str, const CollateClause *node)
{
	WRITE_NODE_TYPE("COLLATECLAUSE");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(collname);
	WRITE_LOCATION_FIELD(location);
}

static void
_outIndexElem(StringInfo str, const IndexElem *node)
{
	WRITE_NODE_TYPE("INDEXELEM");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(expr);
	WRITE_STRING_FIELD(indexcolname);
	WRITE_NODE_FIELD(collation);
	WRITE_NODE_FIELD(opclass);
	WRITE_NODE_FIELD(opclassopts);
	WRITE_ENUM_FIELD(ordering, SortByDir);
	WRITE_ENUM_FIELD(nulls_ordering, SortByNulls);
}

static void
_outStatsElem(StringInfo str, const StatsElem *node)
{
	WRITE_NODE_TYPE("STATSELEM");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(expr);
}

static void
_outQuery(StringInfo str, const Query *node)
{
	WRITE_NODE_TYPE("QUERY");

	WRITE_ENUM_FIELD(commandType, CmdType);
	WRITE_ENUM_FIELD(querySource, QuerySource);
	/* we intentionally do not print the queryId field */
	WRITE_BOOL_FIELD(canSetTag);

	/*
	 * Hack to work around missing outfuncs routines for a lot of the
	 * utility-statement node types.  (The only one we actually *need* for
	 * rules support is NotifyStmt.)  Someday we ought to support 'em all, but
	 * for the meantime do this to avoid getting lots of warnings when running
	 * with debug_print_parse on.
	 */
	if (node->utilityStmt)
	{
		switch (nodeTag(node->utilityStmt))
		{
			case T_CreateStmt:
			case T_IndexStmt:
			case T_NotifyStmt:
			case T_DeclareCursorStmt:
				WRITE_NODE_FIELD(utilityStmt);
				break;
			default:
				appendStringInfoString(str, " :utilityStmt ?");
				break;
		}
	}
	else
		appendStringInfoString(str, " :utilityStmt <>");

	WRITE_INT_FIELD(resultRelation);
	WRITE_BOOL_FIELD(hasAggs);
	WRITE_BOOL_FIELD(hasWindowFuncs);
	WRITE_BOOL_FIELD(hasTargetSRFs);
	WRITE_BOOL_FIELD(hasSubLinks);
	WRITE_BOOL_FIELD(hasDistinctOn);
	WRITE_BOOL_FIELD(hasRecursive);
	WRITE_BOOL_FIELD(hasModifyingCTE);
	WRITE_BOOL_FIELD(hasForUpdate);
	WRITE_BOOL_FIELD(hasRowSecurity);
	WRITE_BOOL_FIELD(isReturn);
	WRITE_NODE_FIELD(cteList);
	WRITE_NODE_FIELD(rtable);
	WRITE_NODE_FIELD(jointree);
	WRITE_NODE_FIELD(targetList);
	WRITE_ENUM_FIELD(override, OverridingKind);
	WRITE_NODE_FIELD(onConflict);
	WRITE_NODE_FIELD(returningList);
	WRITE_NODE_FIELD(groupClause);
	WRITE_BOOL_FIELD(groupDistinct);
	WRITE_NODE_FIELD(groupingSets);
	WRITE_NODE_FIELD(havingQual);
	WRITE_NODE_FIELD(windowClause);
	WRITE_NODE_FIELD(distinctClause);
	WRITE_NODE_FIELD(sortClause);
	WRITE_NODE_FIELD(limitOffset);
	WRITE_NODE_FIELD(limitCount);
	WRITE_ENUM_FIELD(limitOption, LimitOption);
	WRITE_NODE_FIELD(rowMarks);
	WRITE_NODE_FIELD(setOperations);
	WRITE_NODE_FIELD(constraintDeps);
	WRITE_NODE_FIELD(withCheckOptions);
	WRITE_LOCATION_FIELD(stmt_location);
	WRITE_INT_FIELD(stmt_len);
}

static void
_outWithCheckOption(StringInfo str, const WithCheckOption *node)
{
	WRITE_NODE_TYPE("WITHCHECKOPTION");

	WRITE_ENUM_FIELD(kind, WCOKind);
	WRITE_STRING_FIELD(relname);
	WRITE_STRING_FIELD(polname);
	WRITE_NODE_FIELD(qual);
	WRITE_BOOL_FIELD(cascaded);
}

static void
_outSortGroupClause(StringInfo str, const SortGroupClause *node)
{
	WRITE_NODE_TYPE("SORTGROUPCLAUSE");

	WRITE_UINT_FIELD(tleSortGroupRef);
	WRITE_OID_FIELD(eqop);
	WRITE_OID_FIELD(sortop);
	WRITE_BOOL_FIELD(nulls_first);
	WRITE_BOOL_FIELD(hashable);
}

static void
_outGroupingSet(StringInfo str, const GroupingSet *node)
{
	WRITE_NODE_TYPE("GROUPINGSET");

	WRITE_ENUM_FIELD(kind, GroupingSetKind);
	WRITE_NODE_FIELD(content);
	WRITE_LOCATION_FIELD(location);
}

static void
_outWindowClause(StringInfo str, const WindowClause *node)
{
	WRITE_NODE_TYPE("WINDOWCLAUSE");

	WRITE_STRING_FIELD(name);
	WRITE_STRING_FIELD(refname);
	WRITE_NODE_FIELD(partitionClause);
	WRITE_NODE_FIELD(orderClause);
	WRITE_INT_FIELD(frameOptions);
	WRITE_NODE_FIELD(startOffset);
	WRITE_NODE_FIELD(endOffset);
	WRITE_OID_FIELD(startInRangeFunc);
	WRITE_OID_FIELD(endInRangeFunc);
	WRITE_OID_FIELD(inRangeColl);
	WRITE_BOOL_FIELD(inRangeAsc);
	WRITE_BOOL_FIELD(inRangeNullsFirst);
	WRITE_UINT_FIELD(winref);
	WRITE_BOOL_FIELD(copiedOrder);
}

static void
_outRowMarkClause(StringInfo str, const RowMarkClause *node)
{
	WRITE_NODE_TYPE("ROWMARKCLAUSE");

	WRITE_UINT_FIELD(rti);
	WRITE_ENUM_FIELD(strength, LockClauseStrength);
	WRITE_ENUM_FIELD(waitPolicy, LockWaitPolicy);
	WRITE_BOOL_FIELD(pushedDown);
}

static void
_outWithClause(StringInfo str, const WithClause *node)
{
	WRITE_NODE_TYPE("WITHCLAUSE");

	WRITE_NODE_FIELD(ctes);
	WRITE_BOOL_FIELD(recursive);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCTESearchClause(StringInfo str, const CTESearchClause *node)
{
	WRITE_NODE_TYPE("CTESEARCHCLAUSE");

	WRITE_NODE_FIELD(search_col_list);
	WRITE_BOOL_FIELD(search_breadth_first);
	WRITE_STRING_FIELD(search_seq_column);
	WRITE_LOCATION_FIELD(location);
}

static void
_outCTECycleClause(StringInfo str, const CTECycleClause *node)
{
	WRITE_NODE_TYPE("CTECYCLECLAUSE");

	WRITE_NODE_FIELD(cycle_col_list);
	WRITE_STRING_FIELD(cycle_mark_column);
	WRITE_NODE_FIELD(cycle_mark_value);
	WRITE_NODE_FIELD(cycle_mark_default);
	WRITE_STRING_FIELD(cycle_path_column);
	WRITE_LOCATION_FIELD(location);
	WRITE_OID_FIELD(cycle_mark_type);
	WRITE_INT_FIELD(cycle_mark_typmod);
	WRITE_OID_FIELD(cycle_mark_collation);
	WRITE_OID_FIELD(cycle_mark_neop);
}

static void
_outCommonTableExpr(StringInfo str, const CommonTableExpr *node)
{
	WRITE_NODE_TYPE("COMMONTABLEEXPR");

	WRITE_STRING_FIELD(ctename);
	WRITE_NODE_FIELD(aliascolnames);
	WRITE_ENUM_FIELD(ctematerialized, CTEMaterialize);
	WRITE_NODE_FIELD(ctequery);
	WRITE_NODE_FIELD(search_clause);
	WRITE_NODE_FIELD(cycle_clause);
	WRITE_LOCATION_FIELD(location);
	WRITE_BOOL_FIELD(cterecursive);
	WRITE_INT_FIELD(cterefcount);
	WRITE_NODE_FIELD(ctecolnames);
	WRITE_NODE_FIELD(ctecoltypes);
	WRITE_NODE_FIELD(ctecoltypmods);
	WRITE_NODE_FIELD(ctecolcollations);
}

static void
_outSetOperationStmt(StringInfo str, const SetOperationStmt *node)
{
	WRITE_NODE_TYPE("SETOPERATIONSTMT");

	WRITE_ENUM_FIELD(op, SetOperation);
	WRITE_BOOL_FIELD(all);
	WRITE_NODE_FIELD(larg);
	WRITE_NODE_FIELD(rarg);
	WRITE_NODE_FIELD(colTypes);
	WRITE_NODE_FIELD(colTypmods);
	WRITE_NODE_FIELD(colCollations);
	WRITE_NODE_FIELD(groupClauses);
}

static void
_outRangeTblEntry(StringInfo str, const RangeTblEntry *node)
{
	WRITE_NODE_TYPE("RTE");

	/* put alias + eref first to make dump more legible */
	WRITE_NODE_FIELD(alias);
	WRITE_NODE_FIELD(eref);
	WRITE_ENUM_FIELD(rtekind, RTEKind);

	switch (node->rtekind)
	{
		case RTE_RELATION:
			WRITE_OID_FIELD(relid);
			WRITE_CHAR_FIELD(relkind);
			WRITE_INT_FIELD(rellockmode);
			WRITE_NODE_FIELD(tablesample);
			break;
		case RTE_SUBQUERY:
			WRITE_NODE_FIELD(subquery);
			WRITE_BOOL_FIELD(security_barrier);
			break;
		case RTE_JOIN:
			WRITE_ENUM_FIELD(jointype, JoinType);
			WRITE_INT_FIELD(joinmergedcols);
			WRITE_NODE_FIELD(joinaliasvars);
			WRITE_NODE_FIELD(joinleftcols);
			WRITE_NODE_FIELD(joinrightcols);
			WRITE_NODE_FIELD(join_using_alias);
			break;
		case RTE_FUNCTION:
			WRITE_NODE_FIELD(functions);
			WRITE_BOOL_FIELD(funcordinality);
			break;
		case RTE_TABLEFUNC:
			WRITE_NODE_FIELD(tablefunc);
			break;
		case RTE_VALUES:
			WRITE_NODE_FIELD(values_lists);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_CTE:
			WRITE_STRING_FIELD(ctename);
			WRITE_UINT_FIELD(ctelevelsup);
			WRITE_BOOL_FIELD(self_reference);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_NAMEDTUPLESTORE:
			WRITE_STRING_FIELD(enrname);
			WRITE_FLOAT_FIELD(enrtuples, "%.0f");
			WRITE_OID_FIELD(relid);
			WRITE_NODE_FIELD(coltypes);
			WRITE_NODE_FIELD(coltypmods);
			WRITE_NODE_FIELD(colcollations);
			break;
		case RTE_RESULT:
			/* no extra fields */
			break;
		default:
			elog(ERROR, "unrecognized RTE kind: %d", (int) node->rtekind);
			break;
	}

	WRITE_BOOL_FIELD(lateral);
	WRITE_BOOL_FIELD(inh);
	WRITE_BOOL_FIELD(inFromCl);
	WRITE_UINT_FIELD(requiredPerms);
	WRITE_OID_FIELD(checkAsUser);
	WRITE_BITMAPSET_FIELD(selectedCols);
	WRITE_BITMAPSET_FIELD(insertedCols);
	WRITE_BITMAPSET_FIELD(updatedCols);
	WRITE_BITMAPSET_FIELD(extraUpdatedCols);
	WRITE_NODE_FIELD(securityQuals);
}

static void
_outRangeTblFunction(StringInfo str, const RangeTblFunction *node)
{
	WRITE_NODE_TYPE("RANGETBLFUNCTION");

	WRITE_NODE_FIELD(funcexpr);
	WRITE_INT_FIELD(funccolcount);
	WRITE_NODE_FIELD(funccolnames);
	WRITE_NODE_FIELD(funccoltypes);
	WRITE_NODE_FIELD(funccoltypmods);
	WRITE_NODE_FIELD(funccolcollations);
	WRITE_BITMAPSET_FIELD(funcparams);
}

static void
_outTableSampleClause(StringInfo str, const TableSampleClause *node)
{
	WRITE_NODE_TYPE("TABLESAMPLECLAUSE");

	WRITE_OID_FIELD(tsmhandler);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(repeatable);
}

static void
_outA_Expr(StringInfo str, const A_Expr *node)
{
	WRITE_NODE_TYPE("AEXPR");

	switch (node->kind)
	{
		case AEXPR_OP:
			appendStringInfoChar(str, ' ');
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_OP_ANY:
			appendStringInfoChar(str, ' ');
			WRITE_NODE_FIELD(name);
			appendStringInfoString(str, " ANY ");
			break;
		case AEXPR_OP_ALL:
			appendStringInfoChar(str, ' ');
			WRITE_NODE_FIELD(name);
			appendStringInfoString(str, " ALL ");
			break;
		case AEXPR_DISTINCT:
			appendStringInfoString(str, " DISTINCT ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_DISTINCT:
			appendStringInfoString(str, " NOT_DISTINCT ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NULLIF:
			appendStringInfoString(str, " NULLIF ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_IN:
			appendStringInfoString(str, " IN ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_LIKE:
			appendStringInfoString(str, " LIKE ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_ILIKE:
			appendStringInfoString(str, " ILIKE ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_SIMILAR:
			appendStringInfoString(str, " SIMILAR ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN:
			appendStringInfoString(str, " BETWEEN ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN:
			appendStringInfoString(str, " NOT_BETWEEN ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_BETWEEN_SYM:
			appendStringInfoString(str, " BETWEEN_SYM ");
			WRITE_NODE_FIELD(name);
			break;
		case AEXPR_NOT_BETWEEN_SYM:
			appendStringInfoString(str, " NOT_BETWEEN_SYM ");
			WRITE_NODE_FIELD(name);
			break;
		default:
			appendStringInfoString(str, " ??");
			break;
	}

	WRITE_NODE_FIELD(lexpr);
	WRITE_NODE_FIELD(rexpr);
	WRITE_LOCATION_FIELD(location);
}

static void
_outValue(StringInfo str, const Value *node)
{
	switch (node->type)
	{
		case T_Integer:
			appendStringInfo(str, "%d", node->val.ival);
			break;
		case T_Float:

			/*
			 * We assume the value is a valid numeric literal and so does not
			 * need quoting.
			 */
			appendStringInfoString(str, node->val.str);
			break;
		case T_String:

			/*
			 * We use outToken to provide escaping of the string's content,
			 * but we don't want it to do anything with an empty string.
			 */
			appendStringInfoChar(str, '"');
			if (node->val.str[0] != '\0')
				outToken(str, node->val.str);
			appendStringInfoChar(str, '"');
			break;
		case T_BitString:
			/* internal representation already has leading 'b' */
			appendStringInfoString(str, node->val.str);
			break;
		case T_Null:
			/* this is seen only within A_Const, not in transformed trees */
			appendStringInfoString(str, "NULL");
			break;
		default:
			elog(ERROR, "unrecognized node type: %d", (int) node->type);
			break;
	}
}

static void
_outColumnRef(StringInfo str, const ColumnRef *node)
{
	WRITE_NODE_TYPE("COLUMNREF");

	WRITE_NODE_FIELD(fields);
	WRITE_LOCATION_FIELD(location);
}

static void
_outParamRef(StringInfo str, const ParamRef *node)
{
	WRITE_NODE_TYPE("PARAMREF");

	WRITE_INT_FIELD(number);
	WRITE_LOCATION_FIELD(location);
}

/*
 * Node types found in raw parse trees (supported for debug purposes)
 */

static void
_outRawStmt(StringInfo str, const RawStmt *node)
{
	WRITE_NODE_TYPE("RAWSTMT");

	WRITE_NODE_FIELD(stmt);
	WRITE_LOCATION_FIELD(stmt_location);
	WRITE_INT_FIELD(stmt_len);
}

static void
_outA_Const(StringInfo str, const A_Const *node)
{
	WRITE_NODE_TYPE("A_CONST");

	appendStringInfoString(str, " :val ");
	_outValue(str, &(node->val));
	WRITE_LOCATION_FIELD(location);
}

static void
_outA_Star(StringInfo str, const A_Star *node)
{
	WRITE_NODE_TYPE("A_STAR");
}

static void
_outA_Indices(StringInfo str, const A_Indices *node)
{
	WRITE_NODE_TYPE("A_INDICES");

	WRITE_BOOL_FIELD(is_slice);
	WRITE_NODE_FIELD(lidx);
	WRITE_NODE_FIELD(uidx);
}

static void
_outA_Indirection(StringInfo str, const A_Indirection *node)
{
	WRITE_NODE_TYPE("A_INDIRECTION");

	WRITE_NODE_FIELD(arg);
	WRITE_NODE_FIELD(indirection);
}

static void
_outA_ArrayExpr(StringInfo str, const A_ArrayExpr *node)
{
	WRITE_NODE_TYPE("A_ARRAYEXPR");

	WRITE_NODE_FIELD(elements);
	WRITE_LOCATION_FIELD(location);
}

static void
_outResTarget(StringInfo str, const ResTarget *node)
{
	WRITE_NODE_TYPE("RESTARGET");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(indirection);
	WRITE_NODE_FIELD(val);
	WRITE_LOCATION_FIELD(location);
}

static void
_outMultiAssignRef(StringInfo str, const MultiAssignRef *node)
{
	WRITE_NODE_TYPE("MULTIASSIGNREF");

	WRITE_NODE_FIELD(source);
	WRITE_INT_FIELD(colno);
	WRITE_INT_FIELD(ncolumns);
}

static void
_outSortBy(StringInfo str, const SortBy *node)
{
	WRITE_NODE_TYPE("SORTBY");

	WRITE_NODE_FIELD(node);
	WRITE_ENUM_FIELD(sortby_dir, SortByDir);
	WRITE_ENUM_FIELD(sortby_nulls, SortByNulls);
	WRITE_NODE_FIELD(useOp);
	WRITE_LOCATION_FIELD(location);
}

static void
_outWindowDef(StringInfo str, const WindowDef *node)
{
	WRITE_NODE_TYPE("WINDOWDEF");

	WRITE_STRING_FIELD(name);
	WRITE_STRING_FIELD(refname);
	WRITE_NODE_FIELD(partitionClause);
	WRITE_NODE_FIELD(orderClause);
	WRITE_INT_FIELD(frameOptions);
	WRITE_NODE_FIELD(startOffset);
	WRITE_NODE_FIELD(endOffset);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRangeSubselect(StringInfo str, const RangeSubselect *node)
{
	WRITE_NODE_TYPE("RANGESUBSELECT");

	WRITE_BOOL_FIELD(lateral);
	WRITE_NODE_FIELD(subquery);
	WRITE_NODE_FIELD(alias);
}

static void
_outRangeFunction(StringInfo str, const RangeFunction *node)
{
	WRITE_NODE_TYPE("RANGEFUNCTION");

	WRITE_BOOL_FIELD(lateral);
	WRITE_BOOL_FIELD(ordinality);
	WRITE_BOOL_FIELD(is_rowsfrom);
	WRITE_NODE_FIELD(functions);
	WRITE_NODE_FIELD(alias);
	WRITE_NODE_FIELD(coldeflist);
}

static void
_outRangeTableSample(StringInfo str, const RangeTableSample *node)
{
	WRITE_NODE_TYPE("RANGETABLESAMPLE");

	WRITE_NODE_FIELD(relation);
	WRITE_NODE_FIELD(method);
	WRITE_NODE_FIELD(args);
	WRITE_NODE_FIELD(repeatable);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRangeTableFunc(StringInfo str, const RangeTableFunc *node)
{
	WRITE_NODE_TYPE("RANGETABLEFUNC");

	WRITE_BOOL_FIELD(lateral);
	WRITE_NODE_FIELD(docexpr);
	WRITE_NODE_FIELD(rowexpr);
	WRITE_NODE_FIELD(namespaces);
	WRITE_NODE_FIELD(columns);
	WRITE_NODE_FIELD(alias);
	WRITE_LOCATION_FIELD(location);
}

static void
_outRangeTableFuncCol(StringInfo str, const RangeTableFuncCol *node)
{
	WRITE_NODE_TYPE("RANGETABLEFUNCCOL");

	WRITE_STRING_FIELD(colname);
	WRITE_NODE_FIELD(typeName);
	WRITE_BOOL_FIELD(for_ordinality);
	WRITE_BOOL_FIELD(is_not_null);
	WRITE_NODE_FIELD(colexpr);
	WRITE_NODE_FIELD(coldefexpr);
	WRITE_LOCATION_FIELD(location);
}

static void
_outConstraint(StringInfo str, const Constraint *node)
{
	WRITE_NODE_TYPE("CONSTRAINT");

	WRITE_STRING_FIELD(conname);
	WRITE_BOOL_FIELD(deferrable);
	WRITE_BOOL_FIELD(initdeferred);
	WRITE_LOCATION_FIELD(location);

	appendStringInfoString(str, " :contype ");
	switch (node->contype)
	{
		case CONSTR_NULL:
			appendStringInfoString(str, "NULL");
			break;

		case CONSTR_NOTNULL:
			appendStringInfoString(str, "NOT_NULL");
			break;

		case CONSTR_DEFAULT:
			appendStringInfoString(str, "DEFAULT");
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			break;

		case CONSTR_IDENTITY:
			appendStringInfoString(str, "IDENTITY");
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			WRITE_CHAR_FIELD(generated_when);
			break;

		case CONSTR_GENERATED:
			appendStringInfoString(str, "GENERATED");
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			WRITE_CHAR_FIELD(generated_when);
			break;

		case CONSTR_CHECK:
			appendStringInfoString(str, "CHECK");
			WRITE_BOOL_FIELD(is_no_inherit);
			WRITE_NODE_FIELD(raw_expr);
			WRITE_STRING_FIELD(cooked_expr);
			break;

		case CONSTR_PRIMARY:
			appendStringInfoString(str, "PRIMARY_KEY");
			WRITE_NODE_FIELD(keys);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			/* access_method and where_clause not currently used */
			break;

		case CONSTR_UNIQUE:
			appendStringInfoString(str, "UNIQUE");
			WRITE_NODE_FIELD(keys);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			/* access_method and where_clause not currently used */
			break;

		case CONSTR_EXCLUSION:
			appendStringInfoString(str, "EXCLUSION");
			WRITE_NODE_FIELD(exclusions);
			WRITE_NODE_FIELD(including);
			WRITE_NODE_FIELD(options);
			WRITE_STRING_FIELD(indexname);
			WRITE_STRING_FIELD(indexspace);
			WRITE_BOOL_FIELD(reset_default_tblspc);
			WRITE_STRING_FIELD(access_method);
			WRITE_NODE_FIELD(where_clause);
			break;

		case CONSTR_FOREIGN:
			appendStringInfoString(str, "FOREIGN_KEY");
			WRITE_NODE_FIELD(pktable);
			WRITE_NODE_FIELD(fk_attrs);
			WRITE_NODE_FIELD(pk_attrs);
			WRITE_CHAR_FIELD(fk_matchtype);
			WRITE_CHAR_FIELD(fk_upd_action);
			WRITE_CHAR_FIELD(fk_del_action);
			WRITE_NODE_FIELD(old_conpfeqop);
			WRITE_OID_FIELD(old_pktable_oid);
			WRITE_BOOL_FIELD(skip_validation);
			WRITE_BOOL_FIELD(initially_valid);
			break;

		case CONSTR_ATTR_DEFERRABLE:
			appendStringInfoString(str, "ATTR_DEFERRABLE");
			break;

		case CONSTR_ATTR_NOT_DEFERRABLE:
			appendStringInfoString(str, "ATTR_NOT_DEFERRABLE");
			break;

		case CONSTR_ATTR_DEFERRED:
			appendStringInfoString(str, "ATTR_DEFERRED");
			break;

		case CONSTR_ATTR_IMMEDIATE:
			appendStringInfoString(str, "ATTR_IMMEDIATE");
			break;

		default:
			appendStringInfo(str, "<unrecognized_constraint %d>",
							 (int) node->contype);
			break;
	}
}

static void
_outForeignKeyCacheInfo(StringInfo str, const ForeignKeyCacheInfo *node)
{
	WRITE_NODE_TYPE("FOREIGNKEYCACHEINFO");

	WRITE_OID_FIELD(conoid);
	WRITE_OID_FIELD(conrelid);
	WRITE_OID_FIELD(confrelid);
	WRITE_INT_FIELD(nkeys);
	WRITE_ATTRNUMBER_ARRAY(conkey, node->nkeys);
	WRITE_ATTRNUMBER_ARRAY(confkey, node->nkeys);
	WRITE_OID_ARRAY(conpfeqop, node->nkeys);
}

static void
_outPartitionElem(StringInfo str, const PartitionElem *node)
{
	WRITE_NODE_TYPE("PARTITIONELEM");

	WRITE_STRING_FIELD(name);
	WRITE_NODE_FIELD(expr);
	WRITE_NODE_FIELD(collation);
	WRITE_NODE_FIELD(opclass);
	WRITE_LOCATION_FIELD(location);
}

static void
_outPartitionSpec(StringInfo str, const PartitionSpec *node)
{
	WRITE_NODE_TYPE("PARTITIONSPEC");

	WRITE_STRING_FIELD(strategy);
	WRITE_NODE_FIELD(partParams);
	WRITE_LOCATION_FIELD(location);
}

static void
_outPartitionBoundSpec(StringInfo str, const PartitionBoundSpec *node)
{
	WRITE_NODE_TYPE("PARTITIONBOUNDSPEC");

	WRITE_CHAR_FIELD(strategy);
	WRITE_BOOL_FIELD(is_default);
	WRITE_INT_FIELD(modulus);
	WRITE_INT_FIELD(remainder);
	WRITE_NODE_FIELD(listdatums);
	WRITE_NODE_FIELD(lowerdatums);
	WRITE_NODE_FIELD(upperdatums);
	WRITE_LOCATION_FIELD(location);
}

static void
_outPartitionRangeDatum(StringInfo str, const PartitionRangeDatum *node)
{
	WRITE_NODE_TYPE("PARTITIONRANGEDATUM");

	WRITE_ENUM_FIELD(kind, PartitionRangeDatumKind);
	WRITE_NODE_FIELD(value);
	WRITE_LOCATION_FIELD(location);
}

/*
 * outNode -
 *	  converts a Node into ascii string and append it to 'str'
 */
void
outNode(StringInfo str, const void *obj)
{
	/* Guard against stack overflow due to overly complex expressions */
	check_stack_depth();
>>>>>>> theirs

	if (obj == NULL)
		appendStringInfoString(str, "<>");
	else if (IsA(obj, List) || IsA(obj, IntList) || IsA(obj, OidList) ||
			 IsA(obj, XidList))
		_outList(str, obj);
	/* nodeRead does not want to see { } around these! */
	else if (IsA(obj, Integer))
		_outInteger(str, (Integer *) obj);
	else if (IsA(obj, Float))
		_outFloat(str, (Float *) obj);
	else if (IsA(obj, Boolean))
		_outBoolean(str, (Boolean *) obj);
	else if (IsA(obj, String))
		_outString(str, (String *) obj);
	else if (IsA(obj, BitString))
		_outBitString(str, (BitString *) obj);
	else if (IsA(obj, Bitmapset))
		outBitmapset(str, (Bitmapset *) obj);
	else
	{
		appendStringInfoChar(str, '{');
		switch (nodeTag(obj))
		{
#include "outfuncs.switch.c"

			default:

				/*
				 * This should be an ERROR, but it's too useful to be able to
				 * dump structures that outNode only understands part of.
				 */
				elog(WARNING, "could not dump unrecognized node type: %d",
					 (int) nodeTag(obj));
				break;
		}
		appendStringInfoChar(str, '}');
	}
}

/*
 * nodeToString -
 *	   returns the ascii representation of the Node as a palloc'd string
 */
char *
nodeToString(const void *obj)
{
	StringInfoData str;

	/* see stringinfo.h for an explanation of this maneuver */
	initStringInfo(&str);
	outNode(&str, obj);
	return str.data;
}

/*
 * bmsToString -
 *	   returns the ascii representation of the Bitmapset as a palloc'd string
 */
char *
bmsToString(const Bitmapset *bms)
{
	StringInfoData str;

	/* see stringinfo.h for an explanation of this maneuver */
	initStringInfo(&str);
	outBitmapset(&str, bms);
	return str.data;
}
