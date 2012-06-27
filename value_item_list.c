#include "redis.h"

value_item_list* createValueItemList() {
    struct value_item_list *list;
    list = zmalloc(sizeof(*list));
    if(list == NULL) {
        return NULL;
    }
    list->head = NULL;
    list->tail = NULL;
    list->len = 0;
    return list;
}

void freeValueItemList(value_item_list* list) {
    value_item_node* tmp = NULL;
    if(list != NULL) {
        while(list->head != NULL) {
            tmp = list->head->next;
            freeValueItemNode(list->head);
            list->head = tmp;
            list->len--;
        }
        //assert(list->len == 0);
        list->head = NULL;
        list->tail = NULL;
        zfree(list);
        list = NULL;
    }
}

value_item_node* createDoubleValueItemNode(double score) {
    value_item_node* node = (value_item_node*)zmalloc(sizeof(value_item_node));
    if(node == NULL) {
        return NULL;
    }

    node->size = 0;
    node->obj.dnum = score;
    node->pre = NULL;
    node->next = NULL;
    node->type = NODE_TYPE_DOUBLE;

    return node;
}

value_item_node* createLongLongValueItemNode(long long llnum) {
    value_item_node* node = (value_item_node*)zmalloc(sizeof(value_item_node));
    if(node == NULL) {
        return NULL;
    }

    node->size = 0;
    node->obj.llnum = llnum;
    node->pre = NULL;
    node->next = NULL;
    node->type = NODE_TYPE_LONGLONG;

    return node;
}

value_item_node* createGenericValueItemNode(void* buffer,uint32_t size,int type) {
    /* Notes: double type don't use it */
    value_item_node* node = (value_item_node*)zmalloc(sizeof(value_item_node));
    if(node == NULL) {
        return NULL;
    }

    node->size = size;
    if(type == NODE_TYPE_ROBJ ||
            type == NODE_TYPE_BUFFER) {
        node->obj.obj = buffer;
    } else if(type == NODE_TYPE_LONGLONG) {
        node->obj.llnum = (long long)buffer;
    }

    node->pre = NULL;
    node->next = NULL;
    node->type = type;

    return node;
}

value_item_node* createValueItemNode(robj* obj) {
    //Note: here no copy
    return createGenericValueItemNode((void*)obj,0,NODE_TYPE_ROBJ);
}

void freeValueItemNode(value_item_node* node) {
    //Note: here will free data, so use it be careful,
    //we only use it at redis return data to above layer
    if(node != NULL) {
        if(node->obj.obj != NULL) {
            if(node->type == NODE_TYPE_ROBJ) { 
                decrRefCount(node->obj.obj);
            } else if(node->type == NODE_TYPE_BUFFER) {
                //zfree(node->obj.obj);
            }
            node->obj.obj = NULL;
        }
        node->pre = NULL;
        node->next = NULL;
        zfree(node);
        node = NULL;
    }
}

int rpushValueItemNode(value_item_list* list, robj* obj) {
    return rpushGenericValueItemNode(list,(void*)obj,0,NODE_TYPE_ROBJ);
}

int rpushDoubleValueItemNode(value_item_list* list,double score) {
    if(list == NULL) {
        return 0;
    }
    
    value_item_node* node = createDoubleValueItemNode(score);
    if(node == NULL) {
        return list->len;
    }
    if(list->head == NULL) {
        list->head = node;
        list->tail = node;
    } else {
        node->pre = list->tail;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list->len;
}

int rpushLongLongValueItemNode(value_item_list* list,long long llnum) {
    if(list == NULL) {
        return 0;
    }
    
    value_item_node* node = createLongLongValueItemNode(llnum);
    if(node == NULL) {
        return list->len;
    }
    if(list->head == NULL) {
        list->head = node;
        list->tail = node;
    } else {
        node->pre = list->tail;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list->len;
}

int rpushGenericValueItemNode(value_item_list* list,void* obj,uint32_t size,int type) {
    if(list == NULL) {
        return 0;
    }
    
    value_item_node* node = createGenericValueItemNode(obj,size,type);
    if(node == NULL) {
        return list->len;
    }
    if(list->head == NULL) {
        list->head = node;
        list->tail = node;
    } else {
        node->pre = list->tail;
        list->tail->next = node;
        list->tail = node;
    }
    list->len++;
    return list->len;
}

int lpushDoubleValueItemNode(value_item_list* list,double score) {
    if(list == NULL) {
        return 0;
    }

    value_item_node* node = createDoubleValueItemNode(score);
    if(node == NULL) {
        return list->len;
    }
    if(list->head == NULL) {
        list->head = node;
        list->tail = node;
    } else {
        node->next =list->head;
        list->head->pre = node;
        list->head = node;
    }
    list->len++;
    return list->len;
}

int lpushLongLongValueItemNode(value_item_list* list,long long llnum) {
    if(list == NULL) {
        return 0;
    }

    value_item_node* node = createDoubleValueItemNode(llnum);
    if(node == NULL) {
        return list->len;
    }
    if(list->head == NULL) {
        list->head = node;
        list->tail = node;
    } else {
        node->next =list->head;
        list->head->pre = node;
        list->head = node;
    }
    list->len++;
    return list->len;
}

int lpushGenericValueItemNode(value_item_list* list,void* obj,uint32_t size,int type) {
    if(list == NULL) {
        return 0;
    }

    value_item_node* node = createGenericValueItemNode(obj,size,type);
    if(node == NULL) {
        return list->len;
    }
    if(list->head == NULL) {
        list->head = node;
        list->tail = node;
    } else {
        node->next =list->head;
        list->head->pre = node;
        list->head = node;
    }
    list->len++;
    return list->len;
}

int lpushValueItemNode(value_item_list* list, robj* obj) {
    return lpushGenericValueItemNode(list,(void*)obj,0,NODE_TYPE_ROBJ);
}

void removeValueItemNode(value_item_node* node) {
    //Note it's function did not free the node, just remove from list
    if(node == NULL) {
        return;
    }

    if(node->pre != NULL) {
        node->pre->next = node->next;
    }
    if(node->next != NULL) {
        node->next->pre = node->pre;
    }
}

value_item_node* lpopValueItemNode(value_item_list* list) {
    if(list == NULL) {
        return NULL;
    }

    removeValueItemNode(list->head);
    value_item_node* node = list->head;
    list->head = list->head->next;
    list->len--;
    return node;
}

value_item_node* rpopValueItemNode(value_item_list* list) {
    if(list == NULL) {
        return NULL;
    }

    removeValueItemNode(list->tail);
    value_item_node* node = list->tail;
    list->tail = list->tail->pre;
    return node;
}

value_item_iterator* createValueItemIterator(value_item_list* list) {
    if(list == NULL || list->head == NULL) {
        return NULL;
    }
    value_item_iterator* it = (value_item_iterator*)zmalloc(sizeof(value_item_iterator));
    it->next = list->head;
    it->now = 0;
    return it;
}

value_item_node* nextValueItemNode(value_item_iterator** it) {
    if((*it)->next == NULL) {
        return NULL;
    }
    value_item_node* node = (*it)->next;
    (*it)->next = (*it)->next->next;
    (*it)->now++;
    return node;
}

void freeValueItemIterator(value_item_iterator** it) {
    if((*it) != NULL) {
        zfree(*it);
        *it = NULL;
    }
}

int getValueItemNodeType(value_item_node* node) {
    if(node == NULL) {
        return NODE_TYPE_NULL;
    }
    return node->type;
}

int getValueItemNodeSize(value_item_node* node) {
    if(node == NULL) {
        return 0;
    }
    return node->size;
}

