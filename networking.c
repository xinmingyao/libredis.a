#include "redis.h"
#include <sys/uio.h>

void *dupClientReplyValue(void *o) {
    incrRefCount((robj*)o);
    return o;
}

int listMatchObjects(void *a, void *b) {
    return equalStringObjects(a,b);
}

redisClient *createClient(struct redisServer *server) {
    redisClient *c = zmalloc(sizeof(redisClient));
    if (!c) return NULL;
    c->server = server;
    selectDb(c,0);
    c->old_dbnum = 0;
    c->oldargc = 0;
    c->argc = 0;
    c->argv = NULL;
    c->cmd = NULL;
    listAddNodeTail(server->clients,c);
    return c;
}

/* Create a duplicate of the last object in the reply list when
 * it is not exclusively owned by the reply list. */
robj *dupLastObjectIfNeeded(list *reply) {
    robj *new, *cur;
    listNode *ln;
    redisAssert(listLength(reply) > 0);
    ln = listLast(reply);
    cur = listNodeValue(ln);
    if (cur->refcount > 1) {
        new = dupStringObject(cur);
        decrRefCount(cur);
        listNodeValue(ln) = new;
    }
    return listNodeValue(ln);
}

static void freeClientArgv(redisClient *c) {
    int j;
    for (j = 0; j < c->argc; j++) {
        if(c->argv[j] != NULL) {
            decrRefCount(c->argv[j]);
            c->argv[j] = NULL;
        }
    }
    c->argc = 0;
    c->cmd = NULL;
}

void freeClient(struct redisServer *server, redisClient *c) {
    listNode *ln;

    freeClientArgv(c);
    /* Remove from the list of clients */
    ln = listSearchKey(server->clients,c);
    redisAssert(ln != NULL);
    listDelNode(server->clients,ln);
    
    /* Release memory */
    zfree(c->argv);
    zfree(c);
}

/* resetClient prepare the client to process the next command */
void resetClient(redisClient *c) {
    freeClientArgv(c);
}

//void rewriteClientCommandVector(redisClient *c, int argc, ...) {
//    va_list ap;
//    int j;
//    robj **argv; /* The new argument vector */
//
//    argv = zmalloc(sizeof(robj*)*argc);
//    va_start(ap,argc);
//    for (j = 0; j < argc; j++) {
//        robj *a;
//        
//        a = va_arg(ap, robj*);
//        argv[j] = a;
//        incrRefCount(a);
//    }
//    /* We free the objects in the original vector at the end, so we are
//     * sure that if the same objects are reused in the new vector the
//     * refcount gets incremented before it gets decremented. */
//    for (j = 0; j < c->argc; j++) {
//        if(c->argv[j] != NULL) {
//            decrRefCount(c->argv[j]);
//        }
//    }
//    zfree(c->argv);
//    /* Replace argv and argc with our new versions. */
//    c->argv = argv;
//    c->oldargc = argc;
//    c->argc = argc;
//    va_end(ap);
//}
