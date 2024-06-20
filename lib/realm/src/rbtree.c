#include <rbtree.h>
#include <debug.h>
#include <stdlib.h>
#include <string.h>

/* Implementation is dirty because malloc is not supported in rmm
 * No free for the memory pool */
// Allocate a node from the memory pool
rb_node *pool_alloc(memory_pool *pool) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (pool->used[i] == 0) {
            pool->used[i] = 1;
            return &pool->pool[i];
        }
    }
    panic();
    return NULL; // No free node available
}

void init_memory_pool(memory_pool *pool) {
    for (int i = 0; i < MAX_NODES; i++) {
        pool->used[i] = 0; // Mark all nodes as unused
    }
    pool->next_free = 0;
}

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
	rb_node *node = pool_alloc(&tree->pool);
	node->dev_info.base = dev_info.base;
	node->dev_info.size = dev_info.size;
	node->dev_info.state = dev_info.state;
	node->dev_info.owner = dev_info.owner; 

	strlcpy(node->dev_info.dev_name, dev_info.dev_name,
		sizeof(node->dev_info.dev_name));
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

void init_rb_tree(rb_tree *tree) {
	//initialize memory pool
	init_memory_pool(&tree->pool);

	tree->NIL = pool_alloc(&tree->pool);
	tree->NIL->color = BLACK;
	tree->root = tree->NIL;
	return;
}

rb_node *search_rb_tree(rb_tree *tree, unsigned long base)
{
	rb_node *current = tree->root;

	while (current != tree->NIL && base != current->dev_info.base) {
		if (base < current->dev_info.base) {
			current = current->left;
		} else {
			current = current->right;
		}
	}

	return current;
}

