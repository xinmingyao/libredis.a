#include "redis.h"
#include "pqsort.h" /* Partial qsort for SORT+LIMIT */

__thread
struct sortArgument {
    int sort_alpha;
    int sort_bypattern;
    int sort_desc;
} arg;

redisSortOperation *createSortOperation(int type, robj *pattern) {
    redisSortOperation *so = zmalloc(sizeof(*so));
    so->type = type;
    so->pattern = pattern;
    return so;
}

/* Return the value associated to the key with a name obtained
 * substituting the first occurence of '*' in 'pattern' with 'subst'.
 * The returned object will always have its refcount increased by 1
 * when it is non-NULL. */
robj *lookupKeyByPattern(redisDb *db, robj *pattern, robj *subst) {
    char *p, *f;
    sds spat, ssub;
    robj keyobj, fieldobj, *o;
    int prefixlen, sublen, postfixlen, fieldlen;
    /* Expoit the internal sds representation to create a sds string allocated on the stack in order to make this function faster */
    struct {
        int len;
        int free;
        char buf[REDIS_SORTKEY_MAX+1];
    } keyname, fieldname;

    /* If the pattern is "#" return the substitution object itself in order
     * to implement the "SORT ... GET #" feature. */
    spat = pattern->ptr;
    if (spat[0] == '#' && spat[1] == '\0') {
        incrRefCount(subst);
        return subst;
    }

    /* The substitution object may be specially encoded. If so we create
     * a decoded object on the fly. Otherwise getDecodedObject will just
     * increment the ref count, that we'll decrement later. */
    subst = getDecodedObject(subst);

    ssub = subst->ptr;
    if (sdslen(spat)+sdslen(ssub)-1 > REDIS_SORTKEY_MAX) return NULL;
    p = strchr(spat,'*');
    if (!p) {
        decrRefCount(subst);
        return NULL;
    }

    /* Find out if we're dealing with a hash dereference. */
    if ((f = strstr(p+1, "->")) != NULL) {
        fieldlen = sdslen(spat)-(f-spat);
        /* this also copies \0 character */
        memcpy(fieldname.buf,f+2,fieldlen-1);
        fieldname.len = fieldlen-2;
    } else {
        fieldlen = 0;
    }

    prefixlen = p-spat;
    sublen = sdslen(ssub);
    postfixlen = sdslen(spat)-(prefixlen+1)-fieldlen;
    memcpy(keyname.buf,spat,prefixlen);
    memcpy(keyname.buf+prefixlen,ssub,sublen);
    memcpy(keyname.buf+prefixlen+sublen,p+1,postfixlen);
    keyname.buf[prefixlen+sublen+postfixlen] = '\0';
    keyname.len = prefixlen+sublen+postfixlen;
    decrRefCount(subst);

    /* Lookup substituted key */
    uint16_t version = 0;
    initStaticStringObject(keyobj,((char*)&keyname)+(sizeof(struct sdshdr)));
    o = lookupKeyReadWithVersion(db,&keyobj,&version);
    if (o == NULL) return NULL;

    if (fieldlen > 0) {
        if (o->type != REDIS_HASH || fieldname.len < 1) return NULL;

        /* Retrieve value from hash by the field name. This operation
         * already increases the refcount of the returned object. */
        initStaticStringObject(fieldobj,((char*)&fieldname)+(sizeof(struct sdshdr)));
        o = hashTypeGetObject(o, &fieldobj);
    } else {
        if (o->type != REDIS_STRING) return NULL;

        /* Every object that this function returns needs to have its refcount
         * increased. sortCommand decreases it again. */
        incrRefCount(o);
    }

    return o;
}

/* sortCompare() is used by qsort in sortCommand(). Given that qsort_r with
 * the additional parameter is not standard but a BSD-specific we have to
 * pass sorting parameters via the global 'server' structure */
int sortCompare(const void *s1, const void *s2) {
    const redisSortObject *so1 = s1, *so2 = s2;
    int cmp;

    if (!arg.sort_alpha) {
        /* Numeric sorting. Here it's trivial as we precomputed scores */
        if (so1->u.score > so2->u.score) {
            cmp = 1;
        } else if (so1->u.score < so2->u.score) {
            cmp = -1;
        } else {
            cmp = 0;
        }
    } else {
        /* Alphanumeric sorting */
        if (arg.sort_bypattern) {
            if (!so1->u.cmpobj || !so2->u.cmpobj) {
                /* At least one compare object is NULL */
                if (so1->u.cmpobj == so2->u.cmpobj)
                    cmp = 0;
                else if (so1->u.cmpobj == NULL)
                    cmp = -1;
                else
                    cmp = 1;
            } else {
                /* We have both the objects, use strcoll */
                cmp = strcoll(so1->u.cmpobj->ptr,so2->u.cmpobj->ptr);
            }
        } else {
            /* Compare elements directly. */
            cmp = compareStringObjects(so1->obj,so2->obj);
        }
    }
    return arg.sort_desc ? -cmp : cmp;
}
