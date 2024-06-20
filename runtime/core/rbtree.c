#include <stdio.h>
#include <stdlib.h>

typedef enum {RED, BLACK} node_color;

typedef struct {
	unsigned long base;
	size_t size;
	char *dev_name;
} device_info;

typedef struct rb_node {
	device_info dev_info;
	node_color color;
	struct rb_node *left, *right, *parent; 
} rb_node;

typedef struct rb_tree {
	rb_node *root;
	rb_node *NIL;
} rb_tree;

void left_rotate(rb_tree *tree, rb_node *x) {
	rb_node *y = x->right;
	x->right = y->left;
	if (y->left != tree->NIL) {
		y->left->parent = x;
	}
	y->parent = x->parent;
	if (x->parent == tree->NIL) {
		tree->root = y;
	} else if (x == x->parent->left) {
		x->parent->left = y;
	} else {
		x->parent->right = y;
	}
	y->left = x;
	x->parent = y;
}

void right_rotate(rb_tree *tree, rb_node *y) {
	rb_node *x = y->left;
	y->left = x->right;
	if (x->right != tree->NIL) {
		x->right->parent = y;
	}
	x->parent = y->parent;
	if (y->parent == tree->NIL) {
		tree->root = x;
	} else if (y == y->parent->right) {
		y->parent->right = x;
	} else {
		y->parent->left = x;
	}
	x->right = y;
	y->parent = x;
}


void rb_insert_fixup(rb_tree *tree, rb_node *z) {
	while (z->parent->color == RED) {
		if (z->parent == z->parent->parent->left) {
			rb_node *y = z->parent->parent->right;
			if (y->color == RED) {
				z->parent->color = BLACK;
				y->color = BLACK;
				z->parent->parent->color = RED;
				z = z->parent->parent;
			} else {
				if (z == z->parent->right) {
					z = z->parent;
					left_rotate(tree, z);
				}
				z->parent->color = BLACK;
				z->parent->parent->color = RED;
				right_rotate(tree, z->parent->parent);
			}
		} else {
			rb_node *y = z->parent->parent->left;
			if (y->color == RED) {
				z->parent->color = BLACK;
				y->color = BLACK;
				z->parent->parent->color = RED;
				z = z->parent->parent;
			} else {
				if (z == z->parent->left) {
					z = z->parent;
					right_rotate(tree, z);
				}
				z->parent->color = BLACK;
				z->parent->parent->color = RED;
				left_rotate(tree, z->parent->parent);
			}
		}
	}
	tree->root->color = BLACK;
}

rb_node *create_node (rb_tree *tree, device_info dev_info) {
	rb_node *node = (rb_node *)malloc(sizeof(rb_node));
	node->dev_info.base = dev_info.base;
	node->dev_info.size = dev_info.size;
	node->dev_info.name = strdup(dev_info.name);
	node->color = RED;
	node->left = tree->NIL;
	node->right = tree->NIL;
	node->parent = tree->NIL;
	return node;
}

void rb_insert(rb_tree *tree, device_info dev_info) {
	rb_node *z = create_node(tree, dev_info);

	rb_node *y = tree->NIL;
	rb_node *x = tree->root;

	while (x != tree->NIL) {
		y = x;
		if (z->dev_info.base < x->dev_info.base) {
			x = x->left;
		} else {
			x = x->right;
		}
	}

	z->parent = y;
	if (y == tree->NIL) {
		tree->root = z;
	} else if (z->dev_info.base < y->dev_info.base) {
		y->left = z;
	} else {
		y->right = z;
	}

	rb_insert_fixup(tree, z);
}


rb_tree* create_rb_tree() {
	rb_tree *tree = (rb_tree *) malloc(sizeof(rb_tree));
	tree->NIL = (rb_node *) malloc(sizeof(rb_node));
	tree->NIL->color = BLACK;
	tree->root = tree->NIL;
	return tree;
}


void free_rb_tree_nodes(rb_tree *tree, rb_node *node) {
	if (node != tree->NIL) {
		free_rb_tree_nodes(tree, node->left);
		free_rb_tree_nodes(tree, node->right);
		free(node->dev_info.name);
		free(node);
	}
}

void free_rb_tree(rb_tree *tree) {
	if (tree->root != tree->NIL) {
		free_rb_tree_nodes(tree, tree->root);
	}

	free(tree->NIL);
	free(tree);
}


rb_node *search_rb_tree(rbtree *tree, unsigned long base)
{
	rb_node *current = tree->root;

	while (current != tree->nil && base != current->dev_info.base) {
		if (base < current->dev_info.base) {
			current = currnet->left;
		} else {
			current = current->right;
		}
	}

	return current;
}
