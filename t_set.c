#include "redis.h"

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a regular
 * hash table. */
robj *setTypeCreate(robj *value) {
    if (isObjectRepresentableAsLongLong(value,NULL) == REDIS_OK)
        return createIntsetObject();
    return createSetObject();
}

int setTypeAdd(struct redisClient *c, robj *subject, robj *value) {
    long long llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (dictAdd(subject->ptr,value,NULL) == DICT_OK) {
            incrRefCount(value);
            return 1;
        }
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK) {
            uint8_t success = 0;
            subject->ptr = intsetAdd(subject->ptr,llval,&success);
            if (success) {
                /* Convert to regular set when the intset contains
                 * too many entries. */
                if (intsetLen(subject->ptr) > c->server->set_max_intset_entries)
                    setTypeConvert(subject,REDIS_ENCODING_HT);
                return 1;
            }
        } else {
            /* Failed to get integer from object, convert to regular set. */
            setTypeConvert(subject,REDIS_ENCODING_HT);

            /* The set *was* an intset and this value is not integer
             * encodable, so dictAdd should always work. */
            redisAssert(dictAdd(subject->ptr,value,NULL) == DICT_OK);
            incrRefCount(value);
            return 1;
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

int setTypeRemove(robj *setobj, robj *value) {
    long long llval;
    if (setobj->encoding == REDIS_ENCODING_HT) {
        if (dictDelete(setobj->ptr,value) == DICT_OK) {
            if (htNeedsResize(setobj->ptr)) dictResize(setobj->ptr);
            return 1;
        }
    } else if (setobj->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

int setTypeIsMember(robj *subject, robj *value) {
    long long llval;
    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictFind((dict*)subject->ptr,value) != NULL;
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        if (isObjectRepresentableAsLongLong(value,&llval) == REDIS_OK) {
            return intsetFind((intset*)subject->ptr,llval);
        }
    } else {
        redisPanic("Unknown set encoding");
    }
    return 0;
}

setTypeIterator *setTypeInitIterator(robj *subject) {
    setTypeIterator *si = zmalloc(sizeof(setTypeIterator));
    si->subject = subject;
    si->encoding = subject->encoding;
    if (si->encoding == REDIS_ENCODING_HT) {
        si->di = dictGetIterator(subject->ptr);
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        si->ii = 0;
    } else {
        redisPanic("Unknown set encoding");
    }
    return si;
}

void setTypeReleaseIterator(setTypeIterator *si) {
    if (si->encoding == REDIS_ENCODING_HT)
        dictReleaseIterator(si->di);
    zfree(si);
}

/* Move to the next entry in the set. Returns the object at the current
 * position.
 *
 * Since set elements can be internally be stored as redis objects or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointer
 * (eobj) or (llobj) accordingly.
 *
 * When there are no longer elements -1 is returned.
 * Returned objects ref count is not incremented, so this function is
 * copy on write friendly. */
int setTypeNext(setTypeIterator *si, robj **objele, int64_t *llele) {
    if (si->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictNext(si->di);
        if (de == NULL) return -1;
        *objele = dictGetEntryKey(de);
    } else if (si->encoding == REDIS_ENCODING_INTSET) {
        if (!intsetGet(si->subject->ptr,si->ii++,llele))
            return -1;
    }
    return si->encoding;
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new objects
 * or incrementing the ref count of returned objects. So if you don't
 * retain a pointer to this object you should call decrRefCount() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue as the result will be anyway of incrementing the ref count. */
robj *setTypeNextObject(setTypeIterator *si) {
    int64_t intele;
    robj *objele;
    int encoding;

    encoding = setTypeNext(si,&objele,&intele);
    switch(encoding) {
        case -1:    return NULL;
        case REDIS_ENCODING_INTSET:
            return createStringObjectFromLongLong(intele);
        case REDIS_ENCODING_HT:
            incrRefCount(objele);
            return objele;
        default:
            redisPanic("Unsupported encoding");
    }
    return NULL; /* just to suppress warnings */
}

/* Return random element from a non empty set.
 * The returned element can be a int64_t value if the set is encoded
 * as an "intset" blob of integers, or a redis object if the set
 * is a regular set.
 *
 * The caller provides both pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and is used by the caller to check if the
 * int64_t pointer or the redis object pointere was populated.
 *
 * When an object is returned (the set was a real set) the ref count
 * of the object is not incremented so this function can be considered
 * copy on write friendly. */
int setTypeRandomElement(robj *setobj, robj **objele, int64_t *llele) {
    if (setobj->encoding == REDIS_ENCODING_HT) {
        dictEntry *de = dictGetRandomKey(setobj->ptr);
        *objele = dictGetEntryKey(de);
    } else if (setobj->encoding == REDIS_ENCODING_INTSET) {
        *llele = intsetRandom(setobj->ptr);
    } else {
        redisPanic("Unknown set encoding");
    }
    return setobj->encoding;
}

unsigned long setTypeSize(robj *subject) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        return dictSize((dict*)subject->ptr);
    } else if (subject->encoding == REDIS_ENCODING_INTSET) {
        return intsetLen((intset*)subject->ptr);
    } else {
        redisPanic("Unknown set encoding");
    }
}

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hashtable) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *setobj, int enc) {
    setTypeIterator *si;
    redisAssert(setobj->type == REDIS_SET &&
                setobj->encoding == REDIS_ENCODING_INTSET);

    if (enc == REDIS_ENCODING_HT) {
        int64_t intele;
        dict *d = dictCreate(&setDictType,NULL);
        robj *element;

        /* Presize the dict to avoid rehashing */
        dictExpand(d,intsetLen(setobj->ptr));

        /* To add the elements we extract integers and create redis objects */
        si = setTypeInitIterator(setobj);
        while (setTypeNext(si,NULL,&intele) != -1) {
            element = createStringObjectFromLongLong(intele);
            redisAssert(dictAdd(d,element,NULL) == DICT_OK);
        }
        setTypeReleaseIterator(si);

        setobj->encoding = REDIS_ENCODING_HT;
        zfree(setobj->ptr);
        setobj->ptr = d;
    } else {
        redisPanic("Unsupported set conversion");
    }
}

void saddCommand(redisClient *c) {
    robj *set = lookupKeyWriteWithVersion(c->db,c->argv[1],&(c->version));
    
    robj* key = c->argv[1];
    if(set != NULL) {
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
    
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    if (set == NULL) {
        set = setTypeCreate(c->argv[2]);
        dbAdd(c->db,c->argv[1],set);
    } else {
        if (set->type != REDIS_SET) {
            c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
            return;
        }
    }
    if (setTypeAdd(c, set,c->argv[2])) {
        unsigned long size = setTypeSize(set);
        if (size > (unsigned long)(c->server->set_max_size)) {
            setTypeRemove(set, c->argv[2]);
            c->returncode = REDIS_ERR_DATA_LEN_LIMITED;
            return;
        }
        c->server->dirty++;
        c->returncode = REDIS_OK;
    } else {
        c->returncode = REDIS_OK_BUT_ALREADY_EXIST;
    }
    
    dbUpdateKey(c->db, key);
    EXPIRE_OR_NOT
}

void sremCommand(redisClient *c) {
    c->returncode = REDIS_ERR;
    robj *set = lookupKeyWriteWithVersion(c->db,c->argv[1],&(c->version));
    if(set == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if(checkType(c,set,REDIS_SET)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    robj* key = c->argv[1];
    if(set != NULL) {
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

    c->argv[2] = tryObjectEncoding(c->argv[2]);
    if (setTypeRemove(set,c->argv[2])) {
        dbUpdateKey(c->db, key);
        if (setTypeSize(set) == 0) dbDelete(c->db,c->argv[1]);
        c->server->dirty++;
        c->retvalue.llnum = 1;
        c->returncode = REDIS_OK;
    } else {
        c->retvalue.llnum = 0;
        c->returncode = REDIS_OK_NOT_EXIST;
    }
        
    EXPIRE_OR_NOT
}

void smoveCommand(redisClient *c) {
    robj *srcset, *dstset, *ele;
    uint16_t src_version = 0;
    uint16_t dst_version = 0;
    srcset = lookupKeyWriteWithVersion(c->db,c->argv[1],&src_version);
    dstset = lookupKeyWriteWithVersion(c->db,c->argv[2],&dst_version);
    ele = c->argv[3] = tryObjectEncoding(c->argv[3]);

    /* If the source key does not exist return 0 */
    if (srcset == NULL) {
		c->returncode = REDIS_OK_BUT_CZERO;
        return;
    }

    /* If the source key has the wrong type, or the destination key
     * is set and has the wrong type, return with an error. */
    if (checkType(c,srcset,REDIS_SET) ||
        (dstset && checkType(c,dstset,REDIS_SET))) {
		c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;	
		return;
	}

    /* If srcset and dstset are equal, SMOVE is a no-op */
    if (srcset == dstset) {
		c->returncode = REDIS_OK_BUT_CONE;
        return;
    }

    /* If the element cannot be removed from the src set, return 0. */
    if (!setTypeRemove(srcset,ele)) {
		c->returncode = REDIS_OK_BUT_CZERO;
        return;
    }

    /* Remove the src set from the database when empty */
    if (setTypeSize(srcset) == 0) dbDelete(c->db,c->argv[1]);
    c->server->dirty++;

    /* Create the destination set when it doesn't exist */
    if (!dstset) {
        dstset = setTypeCreate(ele);
        dbAdd(c->db,c->argv[2],dstset);
    }

    /* An extra key has changed when ele was successfully added to dstset */
    if (setTypeAdd(c, dstset,ele)) c->server->dirty++;
	c->returncode = REDIS_OK_BUT_CONE;
}

void sismemberCommand(redisClient *c) {
    robj *set = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version));
    if (set == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,set,REDIS_SET)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    c->argv[2] = tryObjectEncoding(c->argv[2]);
    if (setTypeIsMember(set,c->argv[2])) {
        c->returncode = REDIS_OK;
    } else {
        c->returncode = REDIS_OK_BUT_CZERO;
    }
}

void scardCommand(redisClient *c) {
    c->returncode = REDIS_ERR;
    robj *o = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version));
    if (o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,o,REDIS_SET)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    c->retvalue.llnum = setTypeSize(o);
    c->returncode =  REDIS_OK;
}

void spopCommand(redisClient *c) {
    robj *set, *ele;// *aux;
    int64_t llele;
    int encoding;

    set = lookupKeyWriteWithVersion(c->db,c->argv[1],&(c->version));
    
	if (set == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,set,REDIS_SET)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }
    
	robj* key = c->argv[1];
    if(set != NULL) {
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

    encoding = setTypeRandomElement(set,&ele,&llele);
    if (encoding == REDIS_ENCODING_INTSET) {
        ele = createStringObjectFromLongLong(llele);
        set->ptr = intsetRemove(set->ptr,llele,NULL);
    } else {
        incrRefCount(ele);
        setTypeRemove(set,ele);
    }

    /* Replicate/AOF this command as an SREM operation */
    //aux = createStringObject("SREM",4,0);
    //rewriteClientCommandVector(c,3,aux,c->argv[1],ele);
    //decrRefCount(ele);
    //decrRefCount(aux);

    value_item_list *vlist = createValueItemList();
    if(vlist == NULL) {
        c->returncode = REDIS_ERR_MEMORY_ALLOCATE_ERROR;
        return;
    }
    rpushValueItemNode(vlist, ele);
    //incrRefCount(ele);
    c->return_value = (void*)vlist;
    c->returncode = REDIS_OK;

    if (setTypeSize(set) == 0) dbDelete(c->db,c->argv[1]);
    c->server->dirty++;
    dbUpdateKey(c->db, key);
    c->version++;

    EXPIRE_OR_NOT
}

int qsortCompareSetsByCardinality(const void *s1, const void *s2) {
    return setTypeSize(*(robj**)s1)-setTypeSize(*(robj**)s2);
}

void sinterGenericCommand(redisClient *c, robj **setkeys, unsigned long setnum, robj *dstkey) {
    robj **sets = zmalloc(sizeof(robj*)*setnum);
    setTypeIterator *si;
    robj *eleobj, *dstset = NULL;
    int64_t intobj;
    value_item_list* vlist = NULL;
    unsigned long j, cardinality = 0;
    int encoding;

    for (j = 0; j < setnum; j++) {
        robj *setobj = dstkey ?
            lookupKeyWriteWithVersion(c->db,setkeys[j],&(c->version)) :
            lookupKeyReadWithVersion(c->db,setkeys[j],&(c->version));
        if (!setobj) {
            zfree(sets);
            if (dstkey) {
                if (dbDelete(c->db,dstkey)) {
                    c->server->dirty++;
                }
                c->returncode = REDIS_OK_BUT_CZERO;
            } else {
                c->returncode = REDIS_OK_NOT_EXIST;
            }
            return;
        }
        if (checkType(c,setobj,REDIS_SET)) {
            c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
            zfree(sets);
            return;
        }
        sets[j] = setobj;
    }
    /* Sort sets from the smallest to largest, this will improve our
     * algorithm's performace */
    qsort(sets,setnum,sizeof(robj*),qsortCompareSetsByCardinality);

    /* The first thing we should output is the total number of elements...
     * since this is a multi-bulk write, but at this stage we don't know
     * the intersection set size, so we use a trick, append an empty object
     * to the output list and save the pointer to later modify it with the
     * right length */
    if (!dstkey) {
        vlist = createValueItemList();
        if(vlist == NULL) {
            c->returncode = REDIS_ERR_MEMORY_ALLOCATE_ERROR;
            return;
        }
    } else {
        /* If we have a target key where to store the resulting set
         * create this key with an empty set inside */
        dstset = createIntsetObject();
    }

    /* Iterate all the elements of the first (smallest) set, and test
     * the element against all the other sets, if at least one set does
     * not include the element it is discarded */
    si = setTypeInitIterator(sets[0]);
    while((encoding = setTypeNext(si,&eleobj,&intobj)) != -1) {
        for (j = 1; j < setnum; j++) {
            if (sets[j] == sets[0]) continue;
            if (encoding == REDIS_ENCODING_INTSET) {
                /* intset with intset is simple... and fast */
                if (sets[j]->encoding == REDIS_ENCODING_INTSET &&
                    !intsetFind((intset*)sets[j]->ptr,intobj))
                {
                    break;
                /* in order to compare an integer with an object we
                 * have to use the generic function, creating an object
                 * for this */
                } else if (sets[j]->encoding == REDIS_ENCODING_HT) {
                    eleobj = createStringObjectFromLongLong(intobj);
                    if (!setTypeIsMember(sets[j],eleobj)) {
                        decrRefCount(eleobj);
                        break;
                    }
                    decrRefCount(eleobj);
                }
            } else if (encoding == REDIS_ENCODING_HT) {
                /* Optimization... if the source object is integer
                 * encoded AND the target set is an intset, we can get
                 * a much faster path. */
                if (eleobj->encoding == REDIS_ENCODING_INT &&
                    sets[j]->encoding == REDIS_ENCODING_INTSET &&
                    !intsetFind((intset*)sets[j]->ptr,(long)eleobj->ptr))
                {
                    break;
                /* else... object to object check is easy as we use the
                 * type agnostic API here. */
                } else if (!setTypeIsMember(sets[j],eleobj)) {
                    break;
                }
            }
        }

        /* Only take action when all sets contain the member */
        if (j == setnum) {
            if (!dstkey) {
                if (encoding == REDIS_ENCODING_HT) {
                    if (eleobj->encoding == REDIS_ENCODING_INT) {
                        rpushLongLongValueItemNode(vlist, (long)eleobj->ptr);
                    } else {
                        rpushValueItemNode(vlist,eleobj);
                        incrRefCount(eleobj);
                    }
                } else {
                    rpushLongLongValueItemNode(vlist,intobj);
                }
                cardinality++;
            } else {
                if (encoding == REDIS_ENCODING_INTSET) {
                    eleobj = createStringObjectFromLongLong(intobj);
                    setTypeAdd(c, dstset,eleobj);
                    decrRefCount(eleobj);
                } else {
                    setTypeAdd(c, dstset,eleobj);
                }
            }
        }
    }
    setTypeReleaseIterator(si);

    if (dstkey) {
        /* Store the resulting set into the target, if the intersection
         * is not an empty set. */
        dbDelete(c->db,dstkey);
        if (setTypeSize(dstset) > 0) {
            //Notes dstkey has version
            dbAdd(c->db,dstkey,dstset);
            c->retvalue.llnum = setTypeSize(dstset);
            c->returncode = REDIS_OK;
        } else {
            decrRefCount(dstset);
            c->returncode = REDIS_OK_BUT_CZERO;
        }
        c->server->dirty++;
    } else {
        c->return_value = (void*)vlist;
        c->returncode = REDIS_OK;
    }
    zfree(sets);
}

void sinterCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+1,c->argc-1,NULL);
}

void sinterstoreCommand(redisClient *c) {
    sinterGenericCommand(c,c->argv+2,c->argc-2,c->argv[1]);
}
