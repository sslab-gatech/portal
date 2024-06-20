#ifndef RBTREE_H
#define RBTREE_H

#include <stdio.h>
#include <stdlib.h>
#include <portal.h>

#define MAX_NODES 50 //dirty hack


typedef enum {RED, BLACK} node_color;

typedef struct {
        unsigned long base;
        size_t size;
	enum portal_dev_state state;
	struct rd *owner; 
        char dev_name[50];
} device_info;


typedef struct rb_node {
        device_info dev_info;
        node_color color;
        struct rb_node *left, *right, *parent;
} rb_node;

typedef struct memory_pool {
    rb_node pool[MAX_NODES]; // Pre-allocated pool of nodes
    int used[MAX_NODES]; // Array to track used nodes
    size_t next_free; // Next free index in the pool
} memory_pool;

typedef struct rb_tree {
        rb_node *root;
        rb_node *NIL;
	memory_pool pool;
} rb_tree;



void rb_insert(rb_tree *tree, device_info dev_info);
void init_rb_tree(rb_tree *tree);
rb_node *search_rb_tree(rb_tree *tree, unsigned long base);

//make rb_dev_tree globally accessible through multiple files.
extern rb_tree rb_dev_tree; 
#endif 
