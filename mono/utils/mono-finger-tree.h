#ifndef __MONO_FING_TREE_H__
#define __MONO_FING_TREE_H__

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include <stdlib.h>

typedef enum {
	FingerTreeEmptyType,
	FingerTreeSingleType,
	FingerTreeDeepType
} FingerTreeType;

typedef enum {
	NodeTwoType,
	NodeThreeType
} NodeType;

typedef struct {
	NodeType type;
	intptr_t Item1;
	intptr_t Item2;
} NodeTwo;

typedef struct {
	NodeType type;
	intptr_t Item1;
	intptr_t Item2;
	intptr_t Item3;
} NodeThree;

typedef struct {
	NodeType type;
} Node;

typedef enum {
	FingerOneType = 1,
	FingerTwoType = 2,
	FingerThreeType = 3,
	FingerFourType = 4,
	FingerCount,
} FingerType;

typedef struct { 
	FingerType type;
 } Finger;

typedef struct { 
	FingerType type;
	intptr_t Item1;
 } FingerOne;

typedef struct { 
	FingerType type;
	intptr_t Item1;
	intptr_t Item2;
 } FingerTwo;

typedef struct { 
	FingerType type;
	intptr_t Item1;
	intptr_t Item2;
	intptr_t Item3;
 } FingerThree;

typedef struct { 
	FingerType type;
	intptr_t Item1;
	intptr_t Item2;
	intptr_t Item3;
	intptr_t Item4;
 } FingerFour;

typedef struct EmptyFingerTree {
	FingerTreeType type;
} EmptyFingerTree;

EmptyFingerTree EmptyTreeSingleton;

typedef struct SingleFingerTree { 
	FingerTreeType type;
	intptr_t Item1;
} SingleFingerTree;

typedef struct FingerTree { 
	FingerTreeType type;
	size_t reference_count;
} FingerTree;

typedef struct DeepFingerTree {
	FingerTreeType type;
	FingerTree *node;
	Finger left;
	Finger right;
} DeepFingerTree;


#define SingleTree(tree) (*(SingleFingerTree *) &((tree)->data))
#define EmptyTree(tree) (*(EmptyFingerTree *) &((tree)->data))
#define DeepTree(tree) (*(DeepFingerTree *) &((tree)->data))

// Reference counting works because it's acyclic

FingerTree *
make_empty_finger_tree 
{
	return EmptyTreeSingleton;
}

FingerTree *
make_deep_finger_tree (Finger *left, FingerTree *node, Finger *right)
{
	node->reference_count++;
	DeepFingerTree *tree = (DeepFingerTree *) allocate (sizeof (DeepFingerTree));
	tree->type = FingerTreeDeepType;
	tree->left = *left;
	tree->right = *right;

	node->reference_count++;
	tree->node = *node;
}

FingerTree *
make_single_finger_tree (intptr_t a)
{
	SingleFingerTree *tree = (SingleFingerTree *) allocate (sizeof (SingleFingerTree));
	tree->type = FingerTreeSingleType;
	tree->Item1 = a;

	return (FingerTree *)tree;
}

void
free_finger_tree (FingerTree *tree)
{
	do {
		tree->reference_count--;
		FingerTree *next = NULL;

		switch (tree->type) {
			case FingerTreeEmptyType:
				return;

			case FingerTreeSingleType: {
				free (tree);
				return;
			}

			case FingerTreeDeepType: {
					next = tree->node;
					free (tree);
			}
		}

		if (tree->reference_count == 0)
			free (tree);

		tree = next;
	} while (tree);
}

FingerTree *
pushTreeLeft (const FingerTree *tree, const intptr_t a)
{
	switch(tree->type) {
		case FingerTreeEmptyType:
			return makeFingerTreeSingle(a);

		case FingerTreeSingleType: {
			Finger *left = makeFinger(FingerOne, a);
			Finger *right = makeFinger(FingerOne, SingleTree(tree).a);
			return MakeFingerTreeDeep(left, EmptyTreeSingleton, right);
		}

		case FingerTreeDeepType: {
			const Finger *currLeft = &DeepTree(tree).left;
			const Finger *currRight = &DeepTree(tree).right;
			const FingerTree *currNode = DeepTree(tree).node;

			if (currLeft->type == FingerFour) {
				const Finger *left = makeFinger (FingerTwo, a, currLeft->Item1);
				const Node *rest = makeNode (NodeThreeType, currLeft->Item2, currLeft->Item3, currLeft->Item4);
				const FingerTree *node = pushTreeLeft (DeepTree(tree).node, rest);
				return MakeFingerTreeDeep(left, node, currRight);

			} else {
				const Finger *left = pushFingerLeft(a, currLeft);
				return (FingerTreeDeep)(left, node, currRight);
			}
		}
	}
}

Finger *
free_finger_tree (Finger)
{

}

static intptr_t 
bottom (void)
{
	// TODO: IO-less?
	// Annoying to link in
	fprintf (stderr, "Incomplete match. Internal error in finger tree.\n");
	exit(-1);

	// Useful for compilers
	return NULL;
}

static intptr_t 
raiseEmpty (void)
{
	// TODO: IO-less?
	// Annoying to link in
	fprintf (stderr, "Faulted by accessing empty tree\n");
	exit(-1);

	return NULL;
}

const Finger *
nodeToFinger (Node *node)
{
	switch (node->type) {
		case NodeTwoType: {
			NodeTwo *in = (NodeTwo *)node;
			return MakeFingerTwo (in->Item1, in->Item2);
		}

		case NodeThreeType: {
			NodeThree *in = (NodeThree *)node;
			return MakeFingerThree (in->Item1, in->Item2, in.Item3);
			break;
		}

		default:
			return bottom ();
	};
}

intptr_t 
peekFingerLeft (const Finger *finger)
{
	return finger->Item1;
}

intptr_t 
peekFingerRight (const Finger *finger)
{
	switch (finger->type)
	{
		case FingerOne:
			return finger->Item1;
		case FingerTwo:
			return finger->Item2;
		case FingerThree:
			return finger->Item3;
		case FingerFour:
			return finger->Item4;
	}
}

const Finger *
pushFingerLeft (const Finger *finger, const intptr_t a)
{
	switch (finger->type)
	{
		case FingerOne:
			return (FingerTwo) {FingerTypeTwo, a, finger->Item1};
		case FingerTwo:
			return (FingerThree) {FingerTypeThree, a, finger->Item1, finger->Item2};
			break;
		case FingerThree:
			return (FingerFour) {FingerTypeFour, a, finger->Item1, finger->Item2, finger->Item2};
		default:
			return bottom ();
	}
}

const Finger *
popFingerLeft (const Finger *finger)
{
	switch (finger->type)
	{
		case FingerTwo:
			return MakeFingerOne(finger->Item2);
		case FingerThree:
			return MakeFingerTwo(finger->Item2, finger->Item3);
		case FingerFour:
			return MakeFingerThree(finger->Item2, finger->Item3, finger->Item4);
		default:
			return bottom ();
	}
}

const Finger *
pushFingerRight (const Finger *finger, const intptr_t b)
{
	switch (finger->type)
	{
		case FingerOne:
			return MakeFingerTwo(finger->Item1, b);
		case FingerTwo:
			return MakeFingerThree(finger->Item1, finger->Item2, b);
		case FingerThree:
			return MakeFingerFour(finger->Item1, finger->Item2, finger->Item2, b);
		default:
			return bottom ();
	}
}

const Finger *
popFingerRight (const Finger *finger)
{
	switch (finger->type)
	{
		case FingerTwo:
			return MakeFingerOne(finger->Item1);
		case FingerThree:
			return MakeFingerTwo(finger->Item1, finger->Item2);
		case FingerFour:
			return MakeFingerThree(finger->Item1, finger->Item2, finger->Item3);
		default:
			return bottom ();
	}
}


intptr_t 
peekTreeLeft (const FingerTree *tree)
{
	switch(tree->type) {
		case FingerTreeEmptyType:
			return raiseEmpty ();
		case FingerTreeSingleType:
			return ((SingerFingerTree *)tree)->a;
		case FingerTreeDeepType:
			return peekFingerLeft(&DeepTree(tree).left);
	}
}

intptr_t 
peekTreeRight (const FingerTree *tree)
{
	switch(tree->type) {
		case FingerTreeEmptyType:
			return raiseEmpty ();
		case FingerTreeSingleType:
			return SingleTree(tree).a;
		case FingerTreeDeepType:
			return peekFingerRight(&DeepTree(tree).right);
	}
}

FingerTree *
pushTreeLeft (const FingerTree *tree, const intptr_t a)
{
	switch(tree->type) {
		case FingerTreeEmptyType:
			return makeFingerTreeSingle(a);

		case FingerTreeSingleType: {
			Finger *left = makeFinger(FingerOne, a);
			Finger *right = makeFinger(FingerOne, SingleTree(tree).a);
			return MakeFingerTreeDeep(left, EmptyTreeSingleton, right);
		}

		case FingerTreeDeepType: {
			const Finger *currLeft = &DeepTree(tree).left;
			const Finger *currRight = &DeepTree(tree).right;
			const FingerTree *currNode = DeepTree(tree).node;

			if (currLeft->type == FingerFour) {
				const Finger *left = makeFinger (FingerTwo, a, currLeft->Item1);
				const Node *rest = makeNode (NodeThreeType, currLeft->Item2, currLeft->Item3, currLeft->Item4);
				const FingerTree *node = pushTreeLeft (DeepTree(tree).node, rest);
				return MakeFingerTreeDeep(left, node, currRight);

			} else {
				const Finger *left = pushFingerLeft(a, currLeft);
				return (FingerTreeDeep)(left, node, currRight);
			}
		}
	}
}

FingerTree *
popTreeLeft (const FingerTree *tree)
{
	switch(tree->type) {
		case FingerTreeEmptyType:
			return raiseEmpty ();
		case FingerTreeSingleType: {
			return MakeFingerTreeEmpty();
		}
		case FingerTreeDeepType: {
			const Finger *currLeft = &DeepTree(tree).left;
			const Finger *currRight = &DeepTree(tree).right;
			const FingerTree *currNode = DeepTree(tree).node;

			if (currLeft->type == FingerOne) {
				if (currNode->type == FingerTreeEmptyType && currRight->type == FingerOne) {
					return MakeFingerTreeSingle(currRight->Item1);
				} else if (currNode->type == FingerTreeEmptyType) {
					const Finger *left = makeFinger (FingerOne, peekFingerLeft(currRight));
					const Finger *right = popFingerLeft (currRight);
					return MakeFingerTreeDeep(left, currNode, right);
				} else {
					const Finger *left = nodeToFinger (peekTreeLeft (currNode));
					const FingerTree *node = popTreeLeft (currNode);
					return MakeFingerTreeDeep(left, node, currRight);
				}
			} else {
				const Finger *left = popFingerLeft(currLeft);
				return MakeFingerTreeDeepType(left, currNode, currRight);
			}

		}
	}
}


FingerTree *
pushTreeRight (const FingerTree *tree, const intptr_t a)
{
	switch(tree->type) {
		case FingerTreeEmptyType:
			return makeFingerTreeSingle(a);
		case FingerTreeSingleType: {
			Finger *right = MakeFingerOne(a);
			Finger *left = MakeFingerOne(SingleTree(tree).a);
			return makeFingerTreeTreeDeep(left, EmptyTreeSingleton, right);
		}
		case FingerTreeDeepType: {
			const Finger *currRight = &DeepTree(tree).left;
			const Finger *left = &DeepTree(tree).left;

			if (currRight->type == FingerFour) {
				const Finger *right = makeFinger (FingerTwo, a, currRight->Item4);
				const Node *rest = makeNode (NodeThreeType, currRight->Item1, currRight->Item2, currRight->Item3);
				const FingerTree *node = pushTreeRight (DeepTree(tree).node, rest);
				return makeFingerTreeTreeDeep(left, node, right);
			} else {
				const Finger *right = pushFingerRight(currRight, a);
				const FingerTree *node = DeepTree(tree).node;
				return makeFingerTreeTreeDeep(left, node, right);
			}
		}
	}
}

FingerTree *
popTreeRight (const FingerTree *tree)
{
	switch(tree->type) {
		case FingerTreeEmptyType:
			return raiseEmpty ();
		case FingerTreeSingleType: {
			return makeFingerTree(FingerTreeEmptyType);
		}
		case FingerTreeDeepType: {
			const Finger *currLeft = &DeepTree(tree).left;
			const Finger *currRight = &DeepTree(tree).right;
			FingerTree **currNode = &DeepTree(tree).node;

			if (currRight->type == FingerOne) {

				if ((*currNode)->type == FingerTreeEmptyType && currLeft->type == FingerOne) {
					return makeFingerTree (FingerTreeSingleType, currLeft->Item1);

				} else if ((*currNode)->type == FingerTreeEmptyType) {
					const Finger *left = makeFinger (FingerOne, peekFingerRight(currLeft));
					const Finger *right = popFingerRight (currLeft);
					return makeFingerTreeTreeDeep(left, *currNode, right);

				} else {
					const Finger *left = nodeToFinger (peekTreeRight (*currNode));
					const FingerTree *node = popTreeRight (*currNode);
					return makeFingerTreeTreeDeep(left, node, currRight);
				}

			} else {
				const Finger *right = popFingerRight (currRight);
				return makeFingerTreeTreeDeep(currLeft, *currNode, right);
			}

		}
	}
}

// TODO: Get finger tree with count and use count here
#define INITIAL_STACKLET_SIZE 10

typedef struct {
	FingerTree trees [INITIAL_STACKLET_SIZE];
	intptr_t next;
} Stack;

typedef intptr_t mapper (const intptr_t closure, const intptr_t current_data);

#define foldTree { \
	Stack curr; \
	curr->next = NULL; \
	curr->tree = tree; \
	Stack *top = curr; \
 \
	Stack stackLet; \
 \
	while (top != NULL) { \
		if (tree->type == FingerTreeEmptyType) { \
			continue; \
		} else if(tree->type == FingerTreeSingleType) { \
			iter(SingleTree(tree).a); \
			continue; \
		} \
 \
		intptr_t *base = &(DeepTree(next).left.Item1);\
\
		for (int i = 0; i < DeepTree(next).left.type; i++) \
		{ \
			iter (base [i]); \
		} \
 \
		next = top->deep.node; \
 \
		Stack *newTop = calloc(sizeof(Stack), 1); \
		newTop->data = top->tree \
		newTop->next = top; \
		top = newTop; \
	} \
 \
 /* INVIARIANT: if it's on the stack I've seen it and$ \
  can assume it's a base case or a deep tree with its left$ i\
  half and nested trees processed$
 */ \
\
	while (top != NULL) { \
		for (int i = 0; i < INITIAL_STACKLET_SIZE && top->deep.right.data [i] != NULL; i++) { \
			FingerTree *curr = top->trees[i]; \
			if (!curr) \
				break; \
			for (int i = 0; i < DeepTree(curr).right.type; i++) \
			{ \
					iter (DeepTree(curr).right.data [i]); \
				} \
			} \
		top = top->next; \
		if (top) { \
			/* Don't free base of stack, which is on our function's call frame */ \
			free (curr); \
		}\
	} \
}\

void
foldTreeLeft (const FingerTree *tree, mapper iter)
{
#define foldTreeIter(side) \
intptr_t *base = &(DeepTree(next).side.Item1);\
for (int i = 0; i < DeepTree(curr).side.type - 1; i++) \
{ \
	iter (base [i]); \
}

foldTree
#undef foldTreeIter
}

void
foldTreeRight (const FingerTree *tree, mapper iter)
{
#define foldTreeIter(side) \
intptr_t *base = &(DeepTree(next).side.Item1);\
for (int i = DeepTree(curr).side.type - 1; i >= 0; i--) \
{ \
	iter (base [i]); \
}

foldTree
#undef foldTreeIter
}

static const FingerTree *
finger_tree_concat_with_middle_base (const FingerTree *left, const intptr_t *middle, int middle_offset, int middle_len, const FingerTree *right)
{
	if (middle_len - middle_offset = 0)
		if (left->type == FingerTreeEmptyType)
			return right;
		else if (right->type == FingerTreeEmptyType)
			return left;

	const *FingerTree almost = finger_tree_concat_with_middle (left, middle, middle_offset++, middle_len, right);

	if (left->type == FingerTreeEmptyType) {
		return pushTreeLeft (almost, middle [0]);
	} else if (left->type == FingerTreeSingleType) {
		return pushTreeLeft (almost, SingleTree (left).Item1);
	} else if (right->type == FingerTreeEmptyType) {
		return pushTreeRight (almost, middle [0]);
	} else if (left->type == FingerTreeSingleType) {
		return pushTreeRight (almost, SingleTree (right).Item1);
	}
}

// TODO: Make iterative
const FingerTree *
finger_tree_concat_with_middle (const FingerTree *left, const intptr_t *middle, int middle_offset, int middle_len, const FingerTree *right)
{

	if (!(left->type == right->type == FingerTreeDeepType))
		return finger_tree_concat_with_middle_base (left, middle, middle_offset, middle_len, right);

	// No longer base case

	// Need to make a way to iterate through finger items
	// Treat first item as start of array?
	size_t working_length = middle_len - middle_offset;
	intptr_t *newMid = malloc (sizeof (intptr_t *) * working_length);
	size_t lower = 0

	intptr_t *base = &(SingleTree(left)->right->Item1);
	for (int i=0; i < left->right->FingerTreeType; i++)
		newMid [lower++] = base [i];

	for (int i=0; i < working_length; i++)
		newMid [lower++] = middle[i + middle_offset];

	intptr_t *base = &(SingleTree(right)->left->Item1);
	for (int i=0; i < right->left->FingerTreeType; i++)
		newMid [lower++] = base [i];

	const FingerTree *deeper = finger_tree_concat_with_middle (left->node, newMid, right->node);

	free (newMid);

	make_deep_finger_tree (left->left, deeper, right->right);
}

const FingerTree *
finger_tree_concat (const FingerTree *left, const FingerTree *right)
{

}

const FingerTree *
finger_tree_key_value_set (const FingerTree *tree)
{

}

const FingerTree *
finger_tree_key_value_get (const FingerTree *tree)
{

}


