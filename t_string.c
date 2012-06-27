#include "redis.h"

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/

void setGenericCommand(redisClient *c, int nx, robj *key, robj *val, robj *expire) {
    int retval;
    long seconds = 0; /* initialized to avoid an harmness warning */
    robj *oldval;
    c->returncode = REDIS_ERR;

    if (expire) {
        if (getLongFromObject(expire, &seconds) != REDIS_OK) {
            c->returncode = REDIS_ERR_IS_NOT_INTEGER;
            return;
        }
    }

    oldval = lookupKeyWriteWithVersion(c->db,key,&(c->version)); /* Force expire of old key if needed */
    if(oldval != NULL) {
        if(checkType(c, oldval, REDIS_STRING)) {
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

    retval = dbAdd(c->db,key,val);
    if (retval == REDIS_ERR) {
        if (!nx) {
            dbSuperReplace(c->db,key,val);
            incrRefCount(val);
        } else {
            c->returncode = REDIS_OK_BUT_ALREADY_EXIST;
            return;
        }
    } else {
        incrRefCount(val);
    }
    c->server->dirty++;
    if (expire) {
        setExpire(c->db,key,seconds);
    } else if(c->expiretime == 0) {
        removeXExpire(c->db, key);
    }
    c->returncode = REDIS_OK;
}

void setCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,0,c->argv[1],c->argv[2],NULL);
}

void setnxCommand(redisClient *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,1,c->argv[1],c->argv[2],NULL);
}

void setnxexCommand(redisClient *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,1,c->argv[1],c->argv[3],c->argv[2]);
}

void setexCommand(redisClient *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,0,c->argv[1],c->argv[3],c->argv[2]);
}

int getGenericCommand(redisClient *c) {
    c->returncode = REDIS_ERR;
    robj *o;

    if ((o = lookupKeyReadWithVersion(c->db,c->argv[1],&(c->version))) == NULL) {
        c->returncode = REDIS_OK_NOT_EXIST;
        return REDIS_OK;
    }

    if (o->type != REDIS_STRING) {
        c->returncode = REDIS_ERR_WRONG_TYPE_ERROR;
        return REDIS_ERR;
    } else {
        value_item_list* vlist = createValueItemList();
        if(vlist == NULL) {
            c->returncode = REDIS_ERR_MEMORY_ALLOCATE_ERROR;
            return REDIS_ERR;
        }
        if (o->encoding == REDIS_ENCODING_INT) {
            rpushLongLongValueItemNode(vlist, (long)o->ptr);
        } else {
            rpushValueItemNode(vlist, o);
            incrRefCount(o);
        }
        c->return_value = (void*)vlist;
        c->returncode = REDIS_OK;
        return REDIS_OK;
    }
    //never step here
    return REDIS_OK;
}

void getCommand(redisClient *c) {
    getGenericCommand(c);
}

void getsetCommand(redisClient *c) {
    if (getGenericCommand(c) == REDIS_ERR) return;
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    dbReplace(c->db,c->argv[1],c->argv[2]);
    incrRefCount(c->argv[2]);
    c->server->dirty++;
    removeExpire(c->db,c->argv[1]);
}

void incrDecrCommand(redisClient *c, long long init_value, long long incr) {
    c->returncode = REDIS_ERR;
    long long value, oldvalue;
    robj *o;

    o = lookupKeyWriteWithVersion(c->db,c->argv[1],&(c->version));
    if (o != NULL && checkType(c,o,REDIS_STRING)) {
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

    if (o == NULL) {
       value = init_value;
    } else if (getLongLongFromObject(o,&value) != REDIS_OK) {
        c->returncode = REDIS_ERR_IS_NOT_INTEGER;
        return;
    }

    oldvalue = value;
    value += incr;

    value = (int32_t)value;

    o = createStringObjectFromLongLong(value);
    dbSuperReplace(c->db,c->argv[1],o);
    c->server->dirty++;

    EXPIRE_OR_NOT

    c->retvalue.llnum = value;
    c->returncode = REDIS_OK;
}

void incrCommand(redisClient *c) {
    incrDecrCommand(c,0,1);
}

void decrCommand(redisClient *c) {
    incrDecrCommand(c,0,-1);
}

void incrbyCommand(redisClient *c) {
    long long incr;
    long long init_value;
    if (getLongLongFromObject(c->argv[2], &init_value) != REDIS_OK) {
        c->returncode = REDIS_ERR_IS_NOT_INTEGER;
        c->retvalue.llnum = 0;
        return;
    }
    if (getLongLongFromObject(c->argv[3], &incr) != REDIS_OK) {
        c->returncode = REDIS_ERR_IS_NOT_INTEGER;
        c->retvalue.llnum = 0;
        return;
    }
    incrDecrCommand(c,init_value,incr);
}

void decrbyCommand(redisClient *c) {
    long long incr;

    if (getLongLongFromObject(c->argv[2], &incr) != REDIS_OK) {
        c->returncode = REDIS_ERR_IS_NOT_INTEGER;
        return;
    }
    incrDecrCommand(c,0,-incr);
}
