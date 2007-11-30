/****************************************************************************
 *              Aho-Corasick for uniform-length DNA dictionary              *
 *                           Author: Herve Pages                            *
 *                                                                          *
 * Note: a uniform-length dictionary is a non-empty set of non-empty        *
 * strings of the same length.                                              *
 ****************************************************************************/
#include "Biostrings.h"
#include <S.h> /* for Srealloc() */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/****************************************************************************/
static int debug = 0;

SEXP match_ULdna_debug()
{
#ifdef DEBUG_BIOSTRINGS
	debug = !debug;
	Rprintf("Debug mode turned %s in 'match_ULdna.c'\n", debug ? "on" : "off");
#else
	Rprintf("Debug mode not available in 'match_ULdna.c'\n");
#endif
	return R_NilValue;
}

typedef struct pattern {
	const char *P;
	int nP;
} Pattern;


/****************************************************************************
 * Manipulation of the buffer of duplicates
 */

typedef struct dupsbuf_line {
	int *vals;
	int maxcount;
	int count;
} DupsBufLine;

static DupsBufLine *dupsbuf;
static int dupsbuf_maxcount, dupsbuf_count;

static void dupsbuf_reset()
{
	/* No memory leak here, because we use transient storage allocation */
	dupsbuf = NULL;
	dupsbuf_maxcount = dupsbuf_count = 0;
	return;
}

static int dupsbuf_appendtoline(DupsBufLine *line, int P_id)
{
	long new_maxcount;

	if (line->count >= line->maxcount) {
		if (line->maxcount == 0)
			new_maxcount = 1000;
		else
			new_maxcount = 2 * line->maxcount;
		line->vals = Srealloc((char *) line->vals, new_maxcount,
				(long) line->maxcount, int);
		line->maxcount = new_maxcount;
	}
	line->vals[line->count++] = P_id;
	return line->count;
}

static int dupsbuf_append(int P_id1, int P_id2)
{
	int i;
	DupsBufLine *line;
	long new_maxcount;

	for (i = 0, line = dupsbuf; i < dupsbuf_count; i++, line++) {
		if (line->vals[0] == P_id1)
			return dupsbuf_appendtoline(line, P_id2);
	}
	if (dupsbuf_count >= dupsbuf_maxcount) {
		/* Buffer is full */
		if (dupsbuf_maxcount == 0)
			new_maxcount = 1000;
		else
			new_maxcount = 2 * dupsbuf_maxcount;
		dupsbuf = Srealloc((char *) dupsbuf, new_maxcount,
					(long) dupsbuf_maxcount, DupsBufLine);
		dupsbuf_maxcount = new_maxcount;
	}
	line = dupsbuf + dupsbuf_count++;
	line->maxcount = line->count = 0;
	line->vals = NULL;
	dupsbuf_appendtoline(line, P_id1);
	return dupsbuf_appendtoline(line, P_id2);
}


/****************************************************************************
 * Initialization of the Aho-Corasick 4-ary tree
 * =============================================
 *
 * For this Aho-Corasick implementation, we take advantage of 2
 * important specifities of the dictionary (aka pattern set):
 *   1. it's a uniform length dictionary (all words have the same length)
 *   2. it's based on a 4-letter alphabet
 * Because of this, the Aho-Corasick tree (which is in fact a graph if we
 * consider the failure links) can be stored in an array of ACNode elements.
 * This has the following advantages:
 *   - Speed: no need to call alloc for each new node (in the other hand
 *     memory usage is no optimal, there will be a lot of unused space in the
 *     array, this would need to be quantified though).
 *   - Can be stored in an integer vector in R: this is because the size of
 *     a node (ACNode struct) is a multiple of the size of an int (1 node = 6
 *     ints). If this was not the case, we could still use a raw vector.
 *   - Easy to serialize.
 *   - Easy to reallocate.
 * Note that the id of an ACNode element is just its offset in the array.
 */

typedef struct ac_node {
        int ac_node1_id;
        int ac_node2_id;
        int ac_node3_id;
        int ac_node4_id;
	int flink;
        int P_id;
} ACNode;

#define INTS_PER_ACNODE (sizeof(ACNode) / sizeof(int))

static ACNode *ACnodebuf;
static int ACnodebuf_maxcount, ACnodebuf_count;
static int ACnodebuf_pattern_length;
static int ACnodebuf_base_codes[4];

void ACnodebuf_reset()
{
	int i;

	/* No memory leak here, because we use transient storage allocation */
	ACnodebuf = NULL;
	ACnodebuf_maxcount = ACnodebuf_count = 0;
	ACnodebuf_pattern_length = -1;
	for (i = 0; i < 4; i++)
		ACnodebuf_base_codes[i] = -1;
	return;
}

static int ACnodebuf_newNode()
{
	long new_maxcount;
	ACNode *node;

	if (ACnodebuf_count >= ACnodebuf_maxcount) {
		/* Buffer is full */
		if (ACnodebuf_maxcount == 0)
			new_maxcount = 50000;
		else
			new_maxcount = 2 * ACnodebuf_maxcount;
		ACnodebuf = Srealloc((char *) ACnodebuf, new_maxcount,
					(long) ACnodebuf_maxcount, ACNode);
		ACnodebuf_maxcount = new_maxcount;
	}
	node = ACnodebuf + ACnodebuf_count;
	node->ac_node1_id = -1;
	node->ac_node2_id = -1;
	node->ac_node3_id = -1;
	node->ac_node4_id = -1;
	node->flink = -1;
	node->P_id = -1;
	return ACnodebuf_count++;
}

static ACNode *ACnodebuf_tryMovingToChild(char c, int childslot, int *ac_node_id)
{
	if (ACnodebuf_base_codes[childslot] == -1) {
		ACnodebuf_base_codes[childslot] = c;
	} else {
		if (c != ACnodebuf_base_codes[childslot])
			return NULL;
	}
	if (*ac_node_id == -1)
		*ac_node_id = ACnodebuf_newNode();
	return ACnodebuf + *ac_node_id;
}

static void ACnodebuf_addPattern(const char *P, int nP, int P_id)
{
	ACNode *node, *child_node;
	int n;

	if (ACnodebuf_pattern_length == -1) {
		if (nP == 0)
			error("dictionary contains empty patterns");
		ACnodebuf_pattern_length = nP;
	} else {
		if (nP != ACnodebuf_pattern_length)
			error("all patterns in dictionary must have the same length");
	}
	for (n = 0, node = ACnodebuf; n < nP; n++, node = child_node) {
		child_node = ACnodebuf_tryMovingToChild(P[n], 0, &(node->ac_node1_id));
		if (child_node != NULL)
			continue;
		child_node = ACnodebuf_tryMovingToChild(P[n], 1, &(node->ac_node2_id));
		if (child_node != NULL)
			continue;
		child_node = ACnodebuf_tryMovingToChild(P[n], 2, &(node->ac_node3_id));
		if (child_node != NULL)
			continue;
		child_node = ACnodebuf_tryMovingToChild(P[n], 3, &(node->ac_node4_id));
		if (child_node != NULL)
			continue;
		error("dictionary contains more than 4 distinct letters");
	}
	if (node->P_id == -1)
		node->P_id = P_id;
	else
		dupsbuf_append(node->P_id, P_id);
	return;
}

static void ULdna_init(const Pattern *patterns, int dict_length)
{
	const Pattern *p;
	int i;

	dupsbuf_reset();
	ACnodebuf_reset();
	ACnodebuf_newNode();
	for (i = 0, p = patterns; i < dict_length; i++, p++)
		ACnodebuf_addPattern(p->P, p->nP, i + 1);
	return;
}

static SEXP ULdna_init_mkSEXP()
{
	SEXP ans, ans_names, ans_elt, tag, ans_elt_elt;
	DupsBufLine *line;
	int tag_length, i;

	PROTECT(ans = NEW_LIST(3));

	/* set the names */
	PROTECT(ans_names = NEW_CHARACTER(3));
	SET_STRING_ELT(ans_names, 0, mkChar("AC_tree_xp"));
	SET_STRING_ELT(ans_names, 1, mkChar("AC_base_codes"));
	SET_STRING_ELT(ans_names, 2, mkChar("dups"));
	SET_NAMES(ans, ans_names);
	UNPROTECT(1);

	/* set the "AC_tree_xp" element */
	PROTECT(ans_elt = R_MakeExternalPtr(NULL, R_NilValue, R_NilValue));
	tag_length = ACnodebuf_count * INTS_PER_ACNODE;
	PROTECT(tag = NEW_INTEGER(tag_length));
	memcpy((char *) INTEGER(tag), ACnodebuf, tag_length * sizeof(int));
	R_SetExternalPtrTag(ans_elt, tag);
	UNPROTECT(1);
	SET_ELEMENT(ans, 0, ans_elt);
	UNPROTECT(1);

	/* set the "AC_base_codes" element */
	PROTECT(ans_elt = NEW_INTEGER(4));
	for (i = 0; i < 4; i++)
		INTEGER(ans_elt)[i] = ACnodebuf_base_codes[i];
	SET_ELEMENT(ans, 1, ans_elt);
	UNPROTECT(1);

	/* set the "dups" element */
	PROTECT(ans_elt = NEW_LIST(dupsbuf_count));
	for (i = 0, line = dupsbuf; i < dupsbuf_count; i++, line++) {
		PROTECT(ans_elt_elt = NEW_INTEGER(line->count));
		memcpy((char *) INTEGER(ans_elt_elt), line->vals, line->count * sizeof(int));
		SET_ELEMENT(ans_elt, i, ans_elt_elt);
		UNPROTECT(1);
	}
	SET_ELEMENT(ans, 2, ans_elt);
	UNPROTECT(1);

	UNPROTECT(1);
	return ans;
}

/****************************************************************************
 * Exact matching
 * ======================================================
 */

static int ULdna_exact_search()
{
	return 0;
}


/****************************************************************************
 * .Call entry points: "ULdna_init_with_StrVect"
 *                 and "ULdna_init_with_BStringList"
 *
 * Arguments:
 *   'dict': a string vector (aka character vector) containing the
 *           uniform-length dictionary for ULdna_init_with_StrVect.
 *           A list of (pattern@data@xp, pattern@offset, pattern@length)
 *           triplets containing the uniform-length dictionary for
 *           ULdna_init_with_BStringList.
 *
 * Return an R list with the following elements:
 *   - AC_tree_xp: "externalptr" object pointing to the Aho-Corasick 4-ary
 *         tree built from 'dict'.
 *   - AC_base_codes: integer vector containing the 4 character codes (ASCII)
 *         attached to the 4 child slots of any node in the tree pointed by
 *         AC_tree_xp.
 *   - dups: an unnamed (and eventually empty) list of integer vectors
 *         containing the indices of the duplicated words found in 'dict'.
 *
 ****************************************************************************/

SEXP ULdna_init_with_StrVect(SEXP dict)
{
	Pattern *patterns, *p;
	SEXP dict_elt;
	int i;

	patterns = Salloc((long) LENGTH(dict), Pattern);
	for (i = 0, p = patterns; i < LENGTH(dict); i++, p++) {
		dict_elt = STRING_ELT(dict, i);
		p->P = CHAR(dict_elt);
		p->nP = LENGTH(dict_elt);
	}
	ULdna_init(patterns, LENGTH(dict));
	return ULdna_init_mkSEXP();
}

SEXP ULdna_init_with_BStringList(SEXP dict)
{
	SEXP ans;

	error("Not ready yet!\n");
	return ans;
}

