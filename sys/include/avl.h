#pragma	lib	"libavl.a"
#pragma src "/sys/src/libavl"

typedef struct Avl	Avl;
typedef struct Avltree	Avltree;
typedef struct Avlwalk	Avlwalk;

#pragma incomplete Avltree
#pragma incomplete Avlwalk

struct Avl
{
	Avl *p;	/* parent */
	Avl *n[2];	/* children */
	int bal;	/* balance bits */
};

Avltree *mkavltree(int(*cmp)(Avl*, Avl*));
void insertavl(Avltree *tree, Avl *new, Avl **oldp); 
Avl *lookupavl(Avltree *tree, Avl *key);
void deleteavl(Avltree *tree, Avl *key, Avl **oldp);
Avlwalk *avlwalk(Avltree *tree);
Avl *avlnext(Avlwalk *walk);
Avl	*avlprev(Avlwalk *walk);
void endwalk(Avlwalk *walk);
