#include "redis.h"

#include <math.h>

/*-----------------------------------------------------------------------------
 * Hash type API
 *----------------------------------------------------------------------------*/

#define CHECK_HASH_LENGTH(lobj) do {                            \
    unsigned long hash_len = hashTypeLength(lobj);              \
    if(hash_len + ((c->argc - 2) >> 1) > (unsigned long)(c->server->hash_max_size)) {       \
        c->retvalue.llnum = 0;                                  \
        c->returncode = REDIS_ERR_DATA_LEN_LIMITED;             \
        return;                                                 \
    }                                                           \
} while(0)

/* Check the length of a number of objects to see if we need to convert a
 * zipmap to a real hash. Note that we only check string encoded objects
 * as their string length can be queried in constant time. */
void hashTypeTryConversion(struct redisClient* c, robj *subject, robj **argv, int start, int end) {
    int i;
    if (subject->encoding != REDIS_ENCODING_ZIPMAP) return;

    for (i = start; i <= end; i++) {
        if (argv[i]->encoding == REDIS_ENCODING_RAW &&
            sdslen(argv[i]->ptr) > c->server->hash_max_zipmap_value)
        {
            convertToRealHash(subject);
            return;
        }
    }
}

/* Encode given objects in-place when the hash uses a dict. */
void hashTypeTryObjectEncoding(robj *subject, robj **o1, robj **o2) {
    if (subject->encoding == REDIS_ENCODING_HT) {
        if (o1) *o1 = tryObjectEncoding(*o1);
        if (o2) *o2 = tryObjectEncoding(*o2);
    }
}

/* Get the value from a hash identified by key.
 *
 * If the string is found either REDIS_ENCODING_HT or REDIS_ENCODING_ZIPMAP
 * is returned, and either **objval or **v and *vlen are set accordingly,
 * so that objects in hash tables are returend as objects and pointers
 * inside a zipmap are returned as such.
 *
 * If the object was not found -1 is returned.
 *
 * This function is copy on write friendly as there is no incr/decr
 * of refcount needed if objects are accessed just for reading operations. */
int hashTypeGet(robj *o, robj *key, robj **objval, unsigned char **v,
                unsigned int *vlen)
{
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        int found;

        key = getDecodedObject(key);
        found = zipmapGet(o->ptr,key->ptr,sdslen(key->ptr),v,vlen);
        decrRefCount(key);
        if (!found) return -1;
    } else {
        dictEntry *de = dictFind(o->ptr,key);
        if (de == NULL) return -1;
        *objval = dictGetEntryVal(de);
    }
    return o->encoding;
}

/* Higher level function of hashTypeGet() that always returns a Redis
 * object (either new or with refcount incremented), so that the caller
 * can retain a reference or call decrRefCount after the usage.
 *
 * The lower level function can prevent copy on write so it is
 * the preferred way of doing read operations. */
robj *hashTypeGetObject(robj *o, robj *key) {
    robj *objval;
    unsigned char *v;
    unsigned int vlen;

    int encoding = hashTypeGet(o,key,&objval,&v,&vlen);
    switch(encoding) {
        case REDIS_ENCODING_HT:
            incrRefCount(objval);
            return objval;
        case REDIS_ENCODING_ZIPMAP:
            objval = createStringObject((char*)v,vlen,0,0);
            return objval;
        default: return NULL;
    }
}

/* Test if the key exists in the given hash. Returns 1 if the key
 * exists and 0 when it doesn't. */
int hashTypeExists(robj *o, robj *key) {
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        if (zipmapExists(o->ptr,key->ptr,sdslen(key->ptr))) {
            decrRefCount(key);
            return 1;
        }
        decrRefCount(key);
    } else {
        if (dictFind(o->ptr,key) != NULL) {
            return 1;
        }
    }
    return 0;
}

/* Add an element, discard the old if the key already exists.
 * Return 0 on insert and 1 on update. */
int hashTypeSet(redisClient *c, robj *o, robj *key, robj *value) {
    int update = 0;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        value = getDecodedObject(value);
        o->ptr = zipmapSet(o->ptr,
            key->ptr,sdslen(key->ptr),
            value->ptr,sdslen(value->ptr), &update);
        decrRefCount(key);
        decrRefCount(value);

        /* Check if the zipmap needs to be upgraded to a real hash table */
        if (zipmapLen(o->ptr) > c->server->hash_max_zipmap_entries)
            convertToRealHash(o);
    } else {
        if (dictReplace(o->ptr,key,value)) {
            /* Insert */
            incrRefCount(key);
        } else {
            /* Update */
            update = 1;
        }
        incrRefCount(value);
    }
    return update;
}

/* Delete an element from a hash.
 * Return 1 on deleted and 0 on not found. */
int hashTypeDelete(robj *o, robj *key) {
    int deleted = 0;
    if (o->encoding == REDIS_ENCODING_ZIPMAP) {
        key = getDecodedObject(key);
        o->ptr = zipmapDel(o->ptr,key->ptr,sdslen(key->ptr), &deleted);
        decrRefCount(key);
    } else {
        deleted = dictDelete((dict*)o->ptr,key) == DICT_OK;
        /* Always check if the dictionary needs a resize after a delete. */
        if (deleted && htNeedsResize(o->ptr)) dictResize(o->ptr);
    }
    return deleted;
}

/* Return the number of elements in a hash. */
unsigned long hashTypeLength(robj *o) {
    return (o->encoding == REDIS_ENCODING_ZIPMAP) ?
        zipmapLen((unsigned char*)o->ptr) : dictSize((dict*)o->ptr);
}

hashTypeIterator *hashTypeInitIterator(robj *subject) {
    hashTypeIterator *hi = zmalloc(sizeof(hashTypeIterator));
    hi->encoding = subject->encoding;
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        hi->zi = zipmapRewind(subject->ptr);
    } else if (hi->encoding == REDIS_ENCODING_HT) {
        hi->di = dictGetIterator(subject->ptr);
    } else {
        redisAssert(NULL);
    }
    return hi;
}

void hashTypeReleaseIterator(hashTypeIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_HT) {
        dictReleaseIterator(hi->di);
    }
    zfree(hi);
}

/* Move to the next entry in the hash. Return REDIS_OK when the next entry
 * could be found and REDIS_ERR when the iterator reaches the end. */
int hashTypeNext(hashTypeIterator *hi) {
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        if ((hi->zi = zipmapNext(hi->zi, &hi->zk, &hi->zklen,
            &hi->zv, &hi->zvlen)) == NULL) return REDIS_ERR;
    } else {
        if ((hi->de = dictNext(hi->di)) == NULL) return REDIS_ERR;
    }
    return REDIS_OK;
}

/* Get key or value object at current iteration position.
 * The returned item differs with the hash object encoding:
 * - When encoding is REDIS_ENCODING_HT, the objval pointer is populated
 *   with the original object.
 * - When encoding is REDIS_ENCODING_ZIPMAP, a pointer to the string and
 *   its length is retunred populating the v and vlen pointers.
 * This function is copy on write friendly as accessing objects in read only
 * does not require writing to any memory page.
 *
 * The function returns the encoding of the object, so that the caller
 * can underestand if the key or value was returned as object or C string. */
int hashTypeCurrent(hashTypeIterator *hi, int what, robj **objval, unsigned char **v, unsigned int *vlen) {
    if (hi->encoding == REDIS_ENCODING_ZIPMAP) {
        if (what & REDIS_HASH_KEY) {
            *v = hi->zk;
            *vlen = hi->zklen;
        } else {
            *v = hi->zv;
            *vlen = hi->zvlen;
        }
    } else {
        if (what & REDIS_HASH_KEY)
            *objval = dictGetEntryKey(hi->de);
        else
            *objval = dictGetEntryVal(hi->de);
    }
    return hi->encoding;
}

/* A non copy-on-write friendly but higher level version of hashTypeCurrent()
 * that always returns an object with refcount incremented by one (or a new
 * object), so it's up to the caller to decrRefCount() the object if no
 * reference is retained. */
robj *hashTypeCurrentObject(hashTypeIterator *hi, int what) {
    robj *obj;
    unsigned char *v = NULL;
    unsigned int vlen = 0;
    int encoding = hashTypeCurrent(hi,what,&obj,&v,&vlen);

    if (encoding == REDIS_ENCODING_HT) {
        incrRefCount(obj);
        return obj;
    } else {
        return createStringObject((char*)v,vlen,0,0);
    }
}

robj *hashTypeLookupWriteOrCreate(redisClient *c, robj *key) {
    robj *o = lookupKeyWriteWithVersion(c->db,key,&(c->version));
    if (o == NULL) {
        sdsversion_change(key->ptr, 0);
        if(c->version_care) {
            sdsversion_add(key->ptr,1);
        }
        o = createHashObject();
        dbAdd(c->db,key,o);
    } else {
        if (o->type != REDIS_HASH) {
            c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
            return NULL;
        }

        uint16_t version = sdsversion(key->ptr);
        if(c->version_care && version != 0 && version != c->version) {
            c->returncode = REDIS_ERR_VERSION_ERROR;
            return NULL;
        } else {
            sdsversion_change(key->ptr, c->version);
        }
        if(c->version_care) {
            sdsversion_add(key->ptr,1);
        }
    }
    return o;
}

void convertToRealHash(robj *o) {
    unsigned char *key, *val, *p, *zm = o->ptr;
    unsigned int klen, vlen;
    dict *dict = dictCreate(&hashDictType,NULL);

    redisAssert(o->type == REDIS_HASH && o->encoding != REDIS_ENCODING_HT);
    p = zipmapRewind(zm);
    while((p = zipmapNext(p,&key,&klen,&val,&vlen)) != NULL) {
        robj *keyobj, *valobj;

        keyobj = createStringObject((char*)key,klen,0,0);
        valobj = createStringObject((char*)val,vlen,0,0);
        keyobj = tryObjectEncoding(keyobj);
        valobj = tryObjectEncoding(valobj);
        dictAdd(dict,keyobj,valobj);
    }
    o->encoding = REDIS_ENCODING_HT;
    o->ptr = dict;
    zfree(zm);
}

/*-----------------------------------------------------------------------------
 * Hash type commands
 *----------------------------------------------------------------------------*/

void hsetCommand(redisClient *c) {
    int update;
    robj *o;

    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTypeTryConversion(c,o,c->argv,2,3);
    hashTypeTryObjectEncoding(o,&c->argv[2], &c->argv[3]);

    update = hashTypeSet(c, o,c->argv[2],c->argv[3]);
    if(update) {
        c->returncode = REDIS_OK_BUT_ALREADY_EXIST;
    } else {
        unsigned long hash_len = hashTypeLength(o);
        if (hash_len > (unsigned long)(c->server->hash_max_size)) {
            hashTypeDelete(o, c->argv[2]);
            c->retvalue.llnum = 0;
            c->returncode = REDIS_ERR_DATA_LEN_LIMITED;
            return;
        } else {
            c->returncode = REDIS_OK;
        }
    }

    dbUpdateKey(c->db, c->argv[1]);
    EXPIRE_OR_NOT

    c->server->dirty++;
}

void hsetnxCommand(redisClient *c) {
    robj *o;
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    hashTypeTryConversion(c,o,c->argv,2,3);

    if (hashTypeExists(o, c->argv[2])) {
        c->returncode = REDIS_OK_BUT_ALREADY_EXIST;
    } else {
        hashTypeTryObjectEncoding(o,&c->argv[2], &c->argv[3]);
        dbUpdateKey(c->db, c->argv[1]);
        hashTypeSet(c, o,c->argv[2],c->argv[3]);
        c->returncode = REDIS_OK;
        c->server->dirty++;

        EXPIRE_OR_NOT
    }
}

void hmsetCommand(redisClient *c) {
    int i;
    robj *o;

    if ((c->argc % 2) == 1) {
        c->returncode = REDIS_ERR_WRONG_NUMBER_ARGUMENTS;
        return;
    }

    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;

    hashTypeTryConversion(c,o,c->argv,2,c->argc-1);

    unsigned long hash_len = hashTypeLength(o);
    if (hash_len >= (unsigned long)(c->server->hash_max_size)) {
        for (i = 2; i < c->argc; i+= 2) {
            hashTypeTryObjectEncoding(o,&c->argv[i], &c->argv[i+1]);
            dictEntry *de = dictFind(o->ptr, c->argv[i]);
            if (de == NULL) {
                break;
            }
            hashTypeSet(c, o,c->argv[i],c->argv[i+1]);
        }
        if (i != 2) {
            c->server->dirty++;
            dbUpdateKey(c->db, c->argv[1]);
            EXPIRE_OR_NOT
            c->retvalue.llnum = (i - 2) >> 1;
        }
        if (i < c->argc) {
            c->returncode = REDIS_ERR_DATA_LEN_LIMITED;
        } else {
            c->returncode = REDIS_OK;
        }
    } else {
        for (i = 2; i < c->argc; i += 2) {
            hashTypeTryObjectEncoding(o,&c->argv[i], &c->argv[i+1]);
            hashTypeSet(c, o,c->argv[i],c->argv[i+1]);
        }
        c->server->dirty++;
        dbUpdateKey(c->db, c->argv[1]);

        EXPIRE_OR_NOT
        c->retvalue.llnum = (i - 2) >> 1;
        c->returncode = REDIS_OK;
    }
}

void hincrbyCommand(redisClient *c) {
    long long value, incr;
    robj *o, *current, *new;

    if (getLongLongFromObject(c->argv[3],&incr) != REDIS_OK) {
        c->returncode = REDIS_ERR_IS_NOT_INTEGER;
        return;
    }
    /* Notes it will change c->argv[1]'s version */
    if ((o = hashTypeLookupWriteOrCreate(c,c->argv[1])) == NULL) return;
    if ((current = hashTypeGetObject(o,c->argv[2])) != NULL) {
        if (getLongLongFromObject(current,&value) != REDIS_OK) {
            decrRefCount(current);
            c->returncode = REDIS_ERR_IS_NOT_INTEGER;
            return;
        }
        decrRefCount(current);
    } else {
        CHECK_HASH_LENGTH(o);
        value = 0;
    }

    value += incr;

    value = (int32_t)value;

    new = createStringObjectFromLongLong(value);
    hashTypeTryObjectEncoding(o,&c->argv[2],NULL);
    hashTypeSet(c,o,c->argv[2],new);
    /* Notes now it's update version*/
    dbUpdateKey(c->db,c->argv[1]);
    decrRefCount(new);
    c->retvalue.llnum = value;
    c->returncode = REDIS_OK;

    EXPIRE_OR_NOT

    c->server->dirty++;
}

void hgetCommand(redisClient *c) {
    robj *o, *value;
    unsigned char *v;
    unsigned int vlen;
    int encoding;

    o = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version));
    if (o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,o,REDIS_HASH)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    if ((encoding = hashTypeGet(o,c->argv[2],&value,&v,&vlen)) != -1) {
        value_item_list* vlist = createValueItemList();
        if(vlist == NULL) {
            c->returncode = REDIS_ERR_MEMORY_ALLOCATE_ERROR;
            return;
        }

        if (encoding == REDIS_ENCODING_HT) {
            if (value->encoding == REDIS_ENCODING_INT) {
                rpushLongLongValueItemNode(vlist, (long)value->ptr);
            } else {
                rpushValueItemNode(vlist,value);
                incrRefCount(value);
            }
        } else {
            rpushGenericValueItemNode(vlist,v,vlen,NODE_TYPE_BUFFER);
        }
        c->return_value = (void*)vlist;
        c->returncode = REDIS_OK;
    } else {
        c->returncode = REDIS_OK_NOT_EXIST;
    }
}

void hmgetCommand(redisClient *c) {
    int i, encoding;
    robj *o, *value;
    unsigned char *v;
    unsigned int vlen;

    o = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version));
    if (o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (o->type != REDIS_HASH) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    /* Note the check for o != NULL happens inside the loop. This is
     * done because objects that cannot be found are considered to be
     * an empty hash. The reply should then be a series of NULLs. */
    //addReplyMultiBulkLen(c,c->argc-2);
    value_item_list* vlist = createValueItemList();
    if(vlist == NULL) {
        c->returncode = REDIS_ERR_MEMORY_ALLOCATE_ERROR;
        return;
    }
    for (i = 2; i < c->argc; i++) {
        if (o != NULL &&
            (encoding = hashTypeGet(o,c->argv[i],&value,&v,&vlen)) != -1) {
            if (encoding == REDIS_ENCODING_HT) {
                if (value->encoding == REDIS_ENCODING_INT) {
                    rpushLongLongValueItemNode(vlist, (long)value->ptr);
                } else {
                    rpushValueItemNode(vlist,value);
                    incrRefCount(value);
                }
            } else {
                rpushGenericValueItemNode(vlist,v,vlen,NODE_TYPE_BUFFER);
            }
        } else {
            rpushGenericValueItemNode(vlist,NULL,0,NODE_TYPE_NULL);
        }
    }
    c->return_value = (void*)vlist;
    c->returncode = REDIS_OK;
}

void hdelCommand(redisClient *c) {
    robj *o = lookupKeyWriteWithVersion(c->db,c->argv[1],&(c->version));
    if (o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,o,REDIS_HASH)) {
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

    if (hashTypeDelete(o,c->argv[2])) {
        dbUpdateKey(c->db, key);
        if (hashTypeLength(o) == 0) {
            dbDelete(c->db,c->argv[1]);
        } else {
            EXPIRE_OR_NOT
        }
        c->returncode = REDIS_OK;
        c->server->dirty++;
    } else {
        c->returncode = REDIS_OK_NOT_EXIST;
    }
}

void hlenCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version))) == NULL) {
	   c->returncode = REDIS_OK_NOT_EXIST;
	   return;
	}

	if (checkType(c,o,REDIS_HASH)) {
		c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
		return;
	}

	c->retvalue.llnum = hashTypeLength(o);
	c->returncode = REDIS_OK;
}

void genericHgetallCommand(redisClient *c, int flags) {
    robj *o;
    unsigned long count = 0;
    hashTypeIterator *hi;

    o = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version));
    if(o == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }
    if (checkType(c,o,REDIS_HASH)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    value_item_list* vlist = createValueItemList();
    if(vlist == NULL) {
        c->returncode = REDIS_ERR_MEMORY_ALLOCATE_ERROR;
        return;
    }
    hi = hashTypeInitIterator(o);
    while (hashTypeNext(hi) != REDIS_ERR) {
        robj *obj;
        unsigned char *v = NULL;
        unsigned int vlen = 0;
        int encoding;

        if (flags & REDIS_HASH_KEY) {
            encoding = hashTypeCurrent(hi,REDIS_HASH_KEY,&obj,&v,&vlen);
            if (encoding == REDIS_ENCODING_HT) {
                if (obj->encoding == REDIS_ENCODING_INT) {
                    rpushLongLongValueItemNode(vlist, (long)obj->ptr);
                } else {
                    rpushValueItemNode(vlist,obj);
                    incrRefCount(obj);
                }
            } else {
                rpushGenericValueItemNode(vlist,v,vlen,NODE_TYPE_BUFFER);
            }
            count++;
        }
        if (flags & REDIS_HASH_VALUE) {
            encoding = hashTypeCurrent(hi,REDIS_HASH_VALUE,&obj,&v,&vlen);
            if (encoding == REDIS_ENCODING_HT) {
                if (obj->encoding == REDIS_ENCODING_INT) {
                    rpushLongLongValueItemNode(vlist, (long)obj->ptr);
                } else {
                    rpushValueItemNode(vlist,obj);
                    incrRefCount(obj);
                }
            } else {
                rpushGenericValueItemNode(vlist,v,vlen,NODE_TYPE_BUFFER);
            }
            count++;
        }
    }
    hashTypeReleaseIterator(hi);
    c->return_value = (void*)vlist;
    c->returncode = REDIS_OK;
}

void hkeysCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_KEY);
}

void hvalsCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_VALUE);
}

void hgetallCommand(redisClient *c) {
    genericHgetallCommand(c,REDIS_HASH_KEY|REDIS_HASH_VALUE);
}

void hexistsCommand(redisClient *c) {
    robj *o;
    if ((o = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version))) == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return;
    }

    if (checkType(c,o,REDIS_HASH)) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return;
    }

    if (hashTypeExists(o,c->argv[2])) {
        c->returncode = REDIS_OK;
        return;
    }
    c->returncode = REDIS_OK_NOT_EXIST;
}
