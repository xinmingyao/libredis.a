#include "redis.h"
/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/
#define CHECK_LIST_LENGTH(lobj) do {                    \
    unsigned long list_len = listTypeLength(lobj);      \
    if(list_len + c->argc - 2 > c->server->list_max_size) {        \
        c->retvalue.llnum = 0;                          \
        c->returncode = REDIS_ERR_DATA_LEN_LIMITED;     \
        return;                                         \
    }                                                   \
} while(0)

/* Check the argument length to see if it requires us to convert the ziplist
 * to a real list. Only check raw-encoded objects because integer encoded
 * objects are never too long. */
void listTypeTryConversion(redisClient *c, robj *subject, robj *value) {
    if (subject->encoding != REDIS_ENCODING_ZIPLIST) return;
    if (value->encoding == REDIS_ENCODING_RAW &&
        sdslen(value->ptr) > c->server->list_max_ziplist_value)
            listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);
}

void listTypePush(redisClient *c, robj *subject, robj *value, int where) {
    /* Check if we need to convert the ziplist */
    listTypeTryConversion(c, subject,value);
    if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
        ziplistLen(subject->ptr) >= c->server->list_max_ziplist_entries)
            listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);

    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        int pos = (where == REDIS_HEAD) ? ZIPLIST_HEAD : ZIPLIST_TAIL;
        value = getDecodedObject(value);
        subject->ptr = ziplistPush(subject->ptr,value->ptr,sdslen(value->ptr),pos);
        decrRefCount(value);
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        if (where == REDIS_HEAD) {
            listAddNodeHead(subject->ptr,value);
        } else {
            listAddNodeTail(subject->ptr,value);
        }
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
}

robj *listTypePop(robj *subject, int where) {
    robj *value = NULL;
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        int pos = (where == REDIS_HEAD) ? 0 : -1;
        p = ziplistIndex(subject->ptr,pos);
        if (ziplistGet(p,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen,0,0);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            /* We only need to delete an element when it exists */
            subject->ptr = ziplistDelete(subject->ptr,&p);
        }
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        list *list = subject->ptr;
        listNode *ln;
        if (where == REDIS_HEAD) {
            ln = listFirst(list);
        } else {
            ln = listLast(list);
        }
        if (ln != NULL) {
            value = listNodeValue(ln);
            incrRefCount(value);
            listDelNode(list,ln);
        }
    } else {
        redisPanic("Unknown list encoding");
    }
    return value;
}

unsigned long listTypeLength(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_ZIPLIST) {
        return ziplistLen(subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_LINKEDLIST) {
        return listLength((list*)subject->ptr);
    } else {
        redisPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
listTypeIterator *listTypeInitIterator(robj *subject, int index, unsigned char direction) {
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        li->zi = ziplistIndex(subject->ptr,index);
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        li->ln = listIndex(subject->ptr,index);
    } else {
        redisPanic("Unknown list encoding");
    }
    return li;
}

/* Clean up the iterator. */
void listTypeReleaseIterator(listTypeIterator *li) {
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    redisAssert(li->subject->encoding == li->encoding);

    entry->li = li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        entry->zi = li->zi;
        if (entry->zi != NULL) {
            if (li->direction == REDIS_TAIL)
                li->zi = ziplistNext(li->subject->ptr,li->zi);
            else
                li->zi = ziplistPrev(li->subject->ptr,li->zi);
            return 1;
        }
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        entry->ln = li->ln;
        if (entry->ln != NULL) {
            if (li->direction == REDIS_TAIL)
                li->ln = li->ln->next;
            else
                li->ln = li->ln->prev;
            return 1;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
    return 0;
}

/* Return entry or NULL at the current position of the iterator. */
robj *listTypeGet(listTypeEntry *entry) {
    listTypeIterator *li = entry->li;
    robj *value = NULL;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        redisAssert(entry->zi != NULL);
        if (ziplistGet(entry->zi,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen,0,0);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
        }
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        redisAssert(entry->ln != NULL);
        value = listNodeValue(entry->ln);
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
    return value;
}

void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    robj *subject = entry->li->subject;
    if (entry->li->encoding == REDIS_ENCODING_ZIPLIST) {
        value = getDecodedObject(value);
        if (where == REDIS_TAIL) {
            unsigned char *next = ziplistNext(subject->ptr,entry->zi);

            /* When we insert after the current element, but the current element
             * is the tail of the list, we need to do a push. */
            if (next == NULL) {
                subject->ptr = ziplistPush(subject->ptr,value->ptr,sdslen(value->ptr),REDIS_TAIL);
            } else {
                subject->ptr = ziplistInsert(subject->ptr,next,value->ptr,sdslen(value->ptr));
            }
        } else {
            subject->ptr = ziplistInsert(subject->ptr,entry->zi,value->ptr,sdslen(value->ptr));
        }
        decrRefCount(value);
    } else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST) {
        if (where == REDIS_TAIL) {
            listInsertNode(subject->ptr,entry->ln,value,AL_START_TAIL);
        } else {
            listInsertNode(subject->ptr,entry->ln,value,AL_START_HEAD);
        }
        incrRefCount(value);
    } else {
        redisPanic("Unknown list encoding");
    }
}

/* Compare the given object with the entry at the current position. */
int listTypeEqual(listTypeEntry *entry, robj *o) {
    listTypeIterator *li = entry->li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        redisAssert(o->encoding == REDIS_ENCODING_RAW);
        return ziplistCompare(entry->zi,o->ptr,sdslen(o->ptr));
    } else if (li->encoding == REDIS_ENCODING_LINKEDLIST) {
        return equalStringObjects(o,listNodeValue(entry->ln));
    } else {
        redisPanic("Unknown list encoding");
    }
}

/* Delete the element pointed to. */
void listTypeDelete(listTypeEntry *entry) {
    listTypeIterator *li = entry->li;
    if (li->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p = entry->zi;
        li->subject->ptr = ziplistDelete(li->subject->ptr,&p);

        /* Update position of the iterator depending on the direction */
        if (li->direction == REDIS_TAIL)
            li->zi = p;
        else
            li->zi = ziplistPrev(li->subject->ptr,p);
    } else if (entry->li->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *next;
        if (li->direction == REDIS_TAIL)
            next = entry->ln->next;
        else
            next = entry->ln->prev;
        listDelNode(li->subject->ptr,entry->ln);
        li->ln = next;
    } else {
        redisPanic("Unknown list encoding");
    }
}

void listTypeConvert(robj *subject, int enc) {
    listTypeIterator *li;
    listTypeEntry entry;
    redisAssert(subject->type == REDIS_LIST);

    if (enc == REDIS_ENCODING_LINKEDLIST) {
        list *l = listCreate();
        listSetFreeMethod(l,decrRefCount);

        /* listTypeGet returns a robj with incremented refcount */
        li = listTypeInitIterator(subject,0,REDIS_TAIL);
        while (listTypeNext(li,&entry)) listAddNodeTail(l,listTypeGet(&entry));
        listTypeReleaseIterator(li);

        subject->encoding = REDIS_ENCODING_LINKEDLIST;
        zfree(subject->ptr);
        subject->ptr = l;
    } else {
        redisPanic("Unsupported list conversion");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

void pushnGenericCommand(redisClient *c, int where) {
    /* return_value must be null,otherwise it have memory leak */
    c->returncode = REDIS_ERR;

    robj *lobj = lookupKeyWriteWithVersion(c->db, c->argv[1], &c->version);

    robj* key = c->argv[1];
    if(lobj != NULL) {
        if(checkType(c, lobj, REDIS_LIST)) {
            c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
            return;
        }
        uint16_t version = sdsversion(key->ptr);
        if(c->version_care && version != 0 && version != c->version) {
            c->returncode = REDIS_ERR_VERSION_ERROR;
            return;
        } else {
            sdsversion_change(key->ptr, c->version);
        }

    } else {
        sdsversion_change(key->ptr, 0);
    }

    if(c->version_care) {
        sdsversion_add(key->ptr, 1);
    }

    c->return_value = (void*)zmalloc(sizeof(push_return_value));
    if(c->return_value == NULL) {
        c->returncode = REDIS_ERR_MEMORY_ALLOCATE_ERROR;
        return;
    }

    int i = 2;
    for(; i < c->argc; i++) {
        if(lobj == NULL) {
            c->argv[i] = tryObjectEncoding(c->argv[i]);
            lobj = createZiplistObject();
            dbAdd(c->db,c->argv[1],lobj);
        }

        unsigned long list_len = listTypeLength(lobj);
        if (list_len >= (unsigned long)(c->server->list_max_size)) {
            break;
        }

        listTypePush(c,lobj,c->argv[i],where);
        c->server->dirty++;
    }

    if (i != 2) {
        dbUpdateKey(c->db, key);
        EXPIRE_OR_NOT
    }
    push_return_value* prv = (push_return_value*)(c->return_value);
    prv->pushed_num = i-2;
    prv->list_len = listTypeLength(lobj);

    if (i < c->argc) {
        c->returncode = REDIS_ERR_DATA_LEN_LIMITED;
    } else {
        c->returncode = REDIS_OK;
    }
}

void lpushCommand(redisClient *c) {
    pushnGenericCommand(c,REDIS_HEAD);
}

void rpushCommand(redisClient *c) {
    pushnGenericCommand(c,REDIS_TAIL);
}

void pushxnGenericCommand(redisClient *c, robj *refval, robj *val, int where) {
    c->returncode = REDIS_ERR;

    robj *subject;
    listTypeIterator *iter;
    listTypeEntry entry;
    int inserted = 0;
    if ((subject = lookupKeyReadWithVersion(c->db,c->argv[1],&c->version)) == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,subject,REDIS_LIST)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    robj* key = c->argv[1];
    if(subject != NULL) {
        uint16_t version = sdsversion(key->ptr);
        if(c->version_care && version != 0 && version != c->version) {
            c->returncode = REDIS_ERR_VERSION_ERROR;
            return;
        } else {
            sdsversion_change(key->ptr, c->version);
        }
    } else {
        sdsversion_change(key->ptr, 0);
    }

    if(c->version_care) {
        sdsversion_add(key->ptr, 1);
    }

    if(refval != NULL) {
        /* Note: we expect refval to be string-encoded because it is *not* the
         * last argument of the multi-bulk LINSERT. */
        redisAssert(refval->encoding == REDIS_ENCODING_RAW);

        /* We're not sure if this value can be inserted yet, but we cannot
         * convert the list inside the iterator. We don't want to loop over
         * the list twice (once to see if the value can be inserted and once
         * to do the actual insert), so we assume this value can be inserted
         * and convert the ziplist to a regular list if necessary. */
        listTypeTryConversion(c, subject,val);

        /* Seek refval from head to tail */
        iter = listTypeInitIterator(subject,0,REDIS_TAIL);
        while (listTypeNext(iter,&entry)) {
            if (listTypeEqual(&entry,refval)) {
                listTypeInsert(&entry,val,where);
                inserted = 1;
                break;
            }
        }
        listTypeReleaseIterator(iter);

        if (inserted) {
            /* Check if the length exceeds the ziplist length threshold. */
            if (subject->encoding == REDIS_ENCODING_ZIPLIST &&
                ziplistLen(subject->ptr) > c->server->list_max_ziplist_entries)
                    listTypeConvert(subject,REDIS_ENCODING_LINKEDLIST);
            c->server->dirty++;
        } else {
            /* Notify client of a failed insert */
            c->returncode = REDIS_ERR_CNEGO_ERROR;
            return;
        }

        c->return_value = (void*)zmalloc(sizeof(push_return_value));
        if(c->return_value == NULL) {
            c->returncode = REDIS_ERR;
            return;
        }
        push_return_value* prv = (push_return_value*)(c->return_value);
        prv->pushed_num = 1;
        prv->list_len = listTypeLength(subject);
    } else {
        int i = 2;
        for(; i < c->argc; i++) {
            unsigned long list_len = listTypeLength(subject);
            if (list_len >= (unsigned long)(c->server->list_max_size)) {
                break;
            }
            listTypePush(c, subject,val,where);
            c->server->dirty++;
        }

        c->return_value = (void*)zmalloc(sizeof(push_return_value));
        if(c->return_value == NULL) {
            c->returncode = REDIS_ERR;
            return;
        }
        push_return_value* prv = (push_return_value*)(c->return_value);
        prv->pushed_num = i-2;
        prv->list_len = listTypeLength(subject);

        if (i < c->argc) {
            dbUpdateKey(c->db, key);
            EXPIRE_OR_NOT
            c->returncode = REDIS_ERR_DATA_LEN_LIMITED;
            return;
        }
    }

    dbUpdateKey(c->db, key);
    EXPIRE_OR_NOT
    c->returncode = REDIS_OK;
}


void lpushxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxnGenericCommand(c,NULL,c->argv[2],REDIS_HEAD);
}

void rpushxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    pushxnGenericCommand(c,NULL,c->argv[2],REDIS_TAIL);
}

void linsertCommand(redisClient *c) {
    c->argv[4] = tryObjectEncoding(c->argv[4]);
    if (strcasecmp(c->argv[2]->ptr,"after") == 0) {
        pushxnGenericCommand(c,c->argv[3],c->argv[4],REDIS_TAIL);
    } else if (strcasecmp(c->argv[2]->ptr,"before") == 0) {
        pushxnGenericCommand(c,c->argv[3],c->argv[4],REDIS_HEAD);
    } else {
        c->returncode = REDIS_ERR_SYNTAX_ERROR;
    }
}

void llenCommand(redisClient *c) {
    robj *o = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version));
    if (o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,o,REDIS_LIST)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }
    c->retvalue.llnum = listTypeLength(o);
    c->returncode = REDIS_OK;
}

void lindexCommand(redisClient *c) {
    assert(c->return_value == NULL);
    c->returncode = REDIS_ERR;
    robj *o = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version));
    if(o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,o,REDIS_LIST)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }
    int index = atoi(c->argv[2]->ptr);
    robj *value = NULL;

    value_item_list* vlist = createValueItemList();
    if(vlist == NULL) {
        c->returncode = REDIS_ERR_MEMORY_ALLOCATE_ERROR;
        return;
    }

    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;
        p = ziplistIndex(o->ptr,index);
        if (ziplistGet(p,&vstr,&vlen,&vlong)) {
            if (vstr) {
                value = createStringObject((char*)vstr,vlen,0,0);
            } else {
                value = createStringObjectFromLongLong(vlong);
            }
            rpushValueItemNode(vlist,value);
            c->return_value = (void*)vlist;
            c->returncode = REDIS_OK;
        } else {
            freeValueItemList(vlist);
            vlist = NULL;
            c->returncode = REDIS_ERR_OUT_OF_RANGE;
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *ln = listIndex(o->ptr,index);
        if (ln != NULL) {
            value = listNodeValue(ln);
            incrRefCount(value);
            rpushValueItemNode(vlist,value);
            c->return_value = (void*)vlist;
            c->returncode = REDIS_OK;
        } else {
            freeValueItemList(vlist);
            vlist = NULL;
            c->returncode = REDIS_ERR_OUT_OF_RANGE;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
}

void lsetCommand(redisClient *c) {
    robj *o = lookupKeyWriteWithVersion(c->db,c->argv[1],&(c->version));
    if (o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if(checkType(c,o,REDIS_LIST)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    robj* key = c->argv[1];
    if(key != NULL) {
        uint16_t version = sdsversion(key->ptr);
        if(c->version_care && version != 0 && version != c->version) {
            c->returncode = REDIS_ERR_VERSION_ERROR;
            return;
        } else {
            sdsversion_change(key->ptr, c->version);
        }
    } else {
        sdsversion_change(key->ptr, 0);
    }

    if(c->version_care) {
        sdsversion_add(key->ptr, 1);
    }

    int index = atoi(c->argv[2]->ptr);
    robj *value = (c->argv[3] = tryObjectEncoding(c->argv[3]));

    listTypeTryConversion(c, o,value);
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p, *zl = o->ptr;
        p = ziplistIndex(zl,index);
        if (p == NULL) {
            c->returncode = REDIS_ERR_OUT_OF_RANGE;
        } else {
            o->ptr = ziplistDelete(o->ptr,&p);
            value = getDecodedObject(value);
            o->ptr = ziplistInsert(o->ptr,p,value->ptr,sdslen(value->ptr));
            decrRefCount(value);
            c->returncode = REDIS_OK;
            dbUpdateKey(c->db, key);
            c->server->dirty++;
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *ln = listIndex(o->ptr,index);
        if (ln == NULL) {
            c->returncode = REDIS_ERR_OUT_OF_RANGE;
        } else {
            decrRefCount((robj*)listNodeValue(ln));
            listNodeValue(ln) = value;
            incrRefCount(value);
            c->returncode = REDIS_OK;
            dbUpdateKey(c->db, key);
            c->server->dirty++;
        }
    } else {
        redisPanic("Unknown list encoding");
    }
}

//tair's new pop, support mutli values
void popnGenericCommand(redisClient *c, int where) {
    c->returncode = REDIS_ERR;
    robj *o = lookupKeyWriteWithVersion(c->db,c->argv[1],&(c->version));
    if(o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if(checkType(c,o,REDIS_LIST)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    int count = atoi(c->argv[2]->ptr);
    if (count <= 0) {
        //if count <= 0, then client will got success,
        //but data size == 0
        c->returncode = REDIS_OK_BUT_CZERO;
        return;
    }

    robj* key = c->argv[1];
    if(o != NULL) {
        uint16_t version = sdsversion(key->ptr);
        if(c->version_care && version != 0 && version != c->version) {
            c->returncode = REDIS_ERR_VERSION_ERROR;
            return;
        } else {
            sdsversion_change(key->ptr, c->version);
        }
    } else {
        sdsversion_change(key->ptr, 0);
    }

    if(c->version_care) {
        sdsversion_add(key->ptr, 1);
    }

    value_item_list* vlist = createValueItemList();
    if(vlist == NULL) {
        c->returncode = REDIS_ERR_MEMORY_ALLOCATE_ERROR;
        return;
    }

    int now = 0;
    for(; now < count; now++) {
        robj *value = listTypePop(o, where);
        if(value == NULL) {
            break;
        } else {
           rpushValueItemNode(vlist, value);
           if(listTypeLength(o) == 0) {
               dbDelete(c->db,c->argv[1]);
               c->server->dirty++;
               break;
           }
           c->server->dirty++;
        }
    }

    dbUpdateKey(c->db,key);
    c->version++;

    EXPIRE_OR_NOT

    c->return_value = (void*)vlist;
    c->returncode = REDIS_OK;
}

void lpopCommand(redisClient *c) {
    //popGenericCommand(c,REDIS_HEAD);
    popnGenericCommand(c,REDIS_HEAD);
}

void rpopCommand(redisClient *c) {
    //popGenericCommand(c,REDIS_TAIL);
    popnGenericCommand(c,REDIS_TAIL);
}

void lrangeCommand(redisClient *c) {
    c->returncode = REDIS_ERR;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);
    int llen;
    int rangelen;

    robj *o = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version));
    if (o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,o,REDIS_LIST)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }
    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        c->returncode = REDIS_ERR_OUT_OF_RANGE;
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    /* Return the result in form of a multi-bulk reply */
    value_item_list* vlist = createValueItemList();
    if(vlist == NULL) {
        c->returncode = REDIS_ERR_MEMORY_ALLOCATE_ERROR;
        return;
    }
    //addReplyMultiBulkLen(c,rangelen);
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        unsigned char *p = ziplistIndex(o->ptr,start);
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;//long long at 64-bit as void*

        while(rangelen--) {
            ziplistGet(p,&vstr,&vlen,&vlong);
            if (vstr) {
                rpushGenericValueItemNode(vlist,(void*)vstr,vlen,NODE_TYPE_BUFFER);
            } else {
                rpushGenericValueItemNode(vlist,(void*)vlong,0,NODE_TYPE_LONGLONG);
            }
            p = ziplistNext(o->ptr,p);
        }
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        listNode *ln = listIndex(o->ptr,start);

        while(rangelen--) {
            incrRefCount(ln->value);
			rpushGenericValueItemNode(vlist,ln->value,0,NODE_TYPE_ROBJ);
            ln = ln->next;
        }
    } else {
        redisPanic("List encoding is not LINKEDLIST nor ZIPLIST!");
    }
    c->return_value = (void*)vlist;
    c->returncode = REDIS_OK;
}

void ltrimCommand(redisClient *c) {
    c->returncode = REDIS_ERR;
    int start = atoi(c->argv[2]->ptr);
    int end = atoi(c->argv[3]->ptr);
    int llen;
    int j, ltrim, rtrim;
    list *list;
    listNode *ln;

    robj *o = lookupKeyWriteWithVersion(c->db,c->argv[1],
            &(c->version));
    if (o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,o,REDIS_LIST)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    robj* key = c->argv[1];
    if(o != NULL) {
        uint16_t version = sdsversion(key->ptr);
        if(c->version_care && version != 0 && version != c->version) {
            c->returncode = REDIS_ERR_VERSION_ERROR;
            return;
        } else {
            sdsversion_change(key->ptr, c->version);
        }
    } else {
        sdsversion_change(key->ptr, 0);
    }

    if(c->version_care) {
        sdsversion_add(key->ptr, 1);
    }

    llen = listTypeLength(o);

    /* convert negative indexes */
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        /* Out of range start or start > end result in empty list */
        ltrim = llen;
        rtrim = 0;
    } else {
        if (end >= llen) end = llen-1;
        ltrim = start;
        rtrim = llen-end-1;
    }

    /* Remove list elements to perform the trim */
    if (o->encoding == REDIS_ENCODING_ZIPLIST) {
        o->ptr = ziplistDeleteRange(o->ptr,0,ltrim);
        o->ptr = ziplistDeleteRange(o->ptr,-rtrim,rtrim);
    } else if (o->encoding == REDIS_ENCODING_LINKEDLIST) {
        list = o->ptr;
        for (j = 0; j < ltrim; j++) {
            ln = listFirst(list);
            listDelNode(list,ln);
        }
        for (j = 0; j < rtrim; j++) {
            ln = listLast(list);
            listDelNode(list,ln);
        }
    } else {
        redisPanic("Unknown list encoding");
    }

    dbUpdateKey(c->db, key);

    if (listTypeLength(o) == 0) dbDelete(c->db,c->argv[1]);
    c->server->dirty++;

    EXPIRE_OR_NOT

    c->returncode = REDIS_OK;
}

void lremCommand(redisClient *c) {
    robj *subject, *obj;
    obj = c->argv[3] = tryObjectEncoding(c->argv[3]);
    long toremove = atoi(c->argv[2]->ptr);
    if (toremove == 0) {
        c->returncode = REDIS_OK_BUT_CZERO;
        c->retvalue.llnum = 0;
    }

    long removed = 0;
    listTypeEntry entry;

    subject = lookupKeyWriteWithVersion(c->db,c->argv[1],&(c->version));
    if (subject == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,subject,REDIS_LIST)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    robj* key = c->argv[1];
    uint16_t version = sdsversion(key->ptr);
    if(c->version_care && version != 0 && version != c->version) {
        c->returncode = REDIS_ERR_VERSION_ERROR;
        return;
    } else {
        sdsversion_change(key->ptr, c->version);
    }
    if(c->version_care) {
        sdsversion_add(key->ptr, 1);
    }

    /* Make sure obj is raw when we're dealing with a ziplist */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        obj = getDecodedObject(obj);

    listTypeIterator *li;
    if (toremove < 0) {
        toremove = -toremove;
        li = listTypeInitIterator(subject,-1,REDIS_HEAD);
    } else {
        li = listTypeInitIterator(subject,0,REDIS_TAIL);
    }

    while (listTypeNext(li,&entry)) {
        if (listTypeEqual(&entry,obj)) {
            listTypeDelete(&entry);
            c->server->dirty++;
            removed++;
            if (toremove && removed == toremove) break;
        }
    }
    listTypeReleaseIterator(li);

    /* Clean up raw encoded object */
    if (subject->encoding == REDIS_ENCODING_ZIPLIST)
        decrRefCount(obj);

    if (listTypeLength(subject) == 0) dbDelete(c->db,c->argv[1]);
    c->retvalue.llnum = removed;
    c->returncode = REDIS_OK;

    dbUpdateKey(c->db,key);
    EXPIRE_OR_NOT
}

int getTimeoutFromObject(robj *object, time_t *timeout) {
    long tval;

    if (getLongFromObject(object,&tval) != REDIS_OK)
        return REDIS_ERR;

    if (tval < 0) {
        return REDIS_ERR;
    }

    if (tval > 0) tval += time(NULL);
    *timeout = tval;

    return REDIS_OK;
}
