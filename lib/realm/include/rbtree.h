#ifndef RBTREE_H
#define RBTREE_H

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


void rb_insert(rb_tree *tree, device_info dev_info);
rb_tree* create_rb_tree(void);
void free_rb_tree(rb_tree *tree);
rb_node *search_rb_tree(rb_tree *tree, unsigned long base);
#endif 
