/*
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#endif /* HAVE_BACKTRACE */

#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <pthread.h>
#include <sys/resource.h>

/* Our shared "common" objects */

struct sharedObjectsStruct shared;

/* Global vars that are actally used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

/* Global vars */
struct redisLogConfig logConfig;

/*============================ Utility functions ============================ */

void redisLog(int level, const char *fmt, ...) {
    const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };
    const char *c = ".-*#";
    time_t now = time(NULL);
    va_list ap;
    FILE *fp;
    char buf[64];
    char msg[REDIS_MAX_LOGMSG_LEN];

    if (level < logConfig.verbosity) return;

    fp = (logConfig.logfile == NULL) ? stdout : fopen(logConfig.logfile,"a");
    if (!fp) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    strftime(buf,sizeof(buf),"%d %b %H:%M:%S",localtime(&now));
    fprintf(fp,"[%d] %s %c %s\n",(int)getpid(),buf,c[level],msg);
    fflush(fp);

    if (logConfig.logfile) fclose(fp);

    if (logConfig.syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
}

/* Redis generally does not try to recover from out of memory conditions
 * when allocating objects or strings, it is not clear if it will be possible
 * to report this condition to the client since the networking layer itself
 * is based on heap allocation for send buffers, so we simply abort.
 * At least the code will be simpler to read... */
void oom(const char *msg) {
    redisLog(REDIS_WARNING, "%s: Out of memory\n",msg);
    sleep(1);
    abort();
}


void _redisAssert(char *estr, char *file, int line) {
    redisLog(REDIS_WARNING,"=== ASSERTION FAILED ===");
    redisLog(REDIS_WARNING,"==> %s:%d '%s' is not true",file,line,estr);
#ifdef HAVE_BACKTRACE
    redisLog(REDIS_WARNING,"(forcing SIGSEGV in order to print the stack trace)");
    *((char*)-1) = 'x';
#endif
}

void _redisPanic(char *msg, char *file, int line) {
    redisLog(REDIS_WARNING,"!!! Software Failure. Press left mouse button to continue");
    redisLog(REDIS_WARNING,"Guru Meditation: %s #%s:%d",msg,file,line);
#ifdef HAVE_BACKTRACE
    redisLog(REDIS_WARNING,"(forcing SIGSEGV in order to print the stack trace)");
    *((char*)-1) = 'x';
#endif
}

/*====================== Hash table type implementation  ==================== */

/* This is an hash table type that uses the SDS dynamic strings libary as
 * keys and radis objects as values (objects can hold SDS strings,
 * lists, sets). */

void dictVanillaFree(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    zfree(val);
}

void dictListDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    listRelease((list*)val);
}

int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* A case insensitive version used for the command lookup table. */
int dictSdsKeyCaseCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcasecmp(key1, key2) == 0;
}

void dictRedisObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    if (val == NULL) return; /* Values of swapped out keys as set to NULL */
    decrRefCount(val);
}

void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

int dictObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
}

unsigned int dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

unsigned int dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

unsigned int dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

int dictEncObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;

    if (o1->encoding == REDIS_ENCODING_INT &&
        o2->encoding == REDIS_ENCODING_INT)
            return o1->ptr == o2->ptr;

    o1 = getDecodedObject(o1);
    o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
    decrRefCount(o1);
    decrRefCount(o2);
    return cmp;
}

unsigned int dictEncObjHash(const void *key) {
    robj *o = (robj*) key;

    if (o->encoding == REDIS_ENCODING_RAW) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else {
        if (o->encoding == REDIS_ENCODING_INT) {
            char buf[32];
            int len;

            len = ll2string(buf,32,(long)o->ptr);
            return dictGenHashFunction((unsigned char*)buf, len);
        } else {
            unsigned int hash;

            o = getDecodedObject(o);
            hash = dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
            decrRefCount(o);
            return hash;
        }
    }
}

/* Sets type */
dictType setDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictRedisObjectDestructor, /* key destructor */
    NULL                       /* val destructor */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
dictType zsetDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictRedisObjectDestructor, /* key destructor */
    NULL                       /* val destructor */
};

/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};

/* Db->expires */
dictType keyptrDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    NULL                       /* val destructor */
};

/* Command table. sds string -> command struct pointer. */
dictType commandTableDictType = {
    dictSdsCaseHash,           /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCaseCompare,     /* key compare */
    dictSdsDestructor,         /* key destructor */
    NULL                       /* val destructor */
};

/* Hash type hash table (note that small hashes are represented with zimpaps) */
dictType hashDictType = {
    dictEncObjHash,             /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictEncObjKeyCompare,       /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};

/* Keylist hash table type has unencoded redis objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) and to
 * map swapped keys to a list of clients waiting for this keys to be loaded. */
dictType keylistDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    dictListDestructor          /* val destructor */
};

int htNeedsResize(dict *dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size && used && size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < REDIS_HT_MINFILL));
}

/* If the percentage of used slots in the HT reaches REDIS_HT_MINFILL
 * we resize the hash table to save memory */
void tryResizeHashTables(redisServer *server) {
    int j;

    int dbnum = get_malloc_dbnum();
    for (j = 0; j < server->dbnum; j++) {
        set_malloc_dbnum(j);
        redisDb *db = server->db + j;
        if (htNeedsResize(db->dict))
            dictResize(db->dict);
        if (htNeedsResize(db->expires))
            dictResize(db->expires);
    }
    set_malloc_dbnum(dbnum);
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every serverCron() loop in order to rehash some key. */
void incrementallyRehash(redisServer *server) {
    int j;

    int dbnum = get_malloc_dbnum();
    for (j = 0; j < server->dbnum; j++) {
        set_malloc_dbnum(j);
        if (dictIsRehashing(server->db[j].dict)) {
            dictRehashMilliseconds(server->db[j].dict,1);
            break; /* already used our millisecond for this loop... */
        }
    }
    set_malloc_dbnum(dbnum);
}

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize the hash tables accordingly to the fact we have o not
 * running childs. */
void updateDictResizePolicy(redisServer *server) {
    REDIS_NOTUSED(server);
    dictEnableResize();
}


void updateLRUClock() {
    shared.lruclock = (time(NULL)/REDIS_LRU_CLOCK_RESOLUTION) &
                                                REDIS_LRU_CLOCK_MAX;
}

void activeExpireCycle(struct redisServer *server) {
    int j;
    uint16_t logiclock;
    long num;
    dictEntry *de;

    int dbnum = get_malloc_dbnum();
    for (j = 0; j < server->dbnum; j++) {
        set_malloc_dbnum(j);
        int expired;
        redisDb *db = server->db+j;

        do {
            num = db->need_remove_key;

            expired = 0;
            if (num > REDIS_EXPIRELOOKUPS_PER_CRON)
                num = REDIS_EXPIRELOOKUPS_PER_CRON;
            while (num--) {
                if ((de = dictGetRandomKey(db->dict)) == NULL) break;
                sds key = dictGetEntryKey(de);
                logiclock = sdslogiclock(key);
                if (db->logiclock > logiclock) {
                    robj *keyobj = createStringObject(key,sdslen(key),sdslogiclock(key),sdsversion(key));
                    dbDelete(db,keyobj);
                    decrRefCount(keyobj);
                    expired++;
                    db->need_remove_key--;
                    db->stat_expiredkeys++;
                }
            }
        } while (expired > REDIS_EXPIRELOOKUPS_PER_CRON/4);

        /* Continue to expire if at the end of the cycle more than 25%
         * of the keys were expired. */
        do {
            num = dictSize(db->expires);
            time_t now = time(NULL);

            expired = 0;
            if (num > REDIS_EXPIRELOOKUPS_PER_CRON)
                num = REDIS_EXPIRELOOKUPS_PER_CRON;
            while (num--) {
                time_t t;

                if ((de = dictGetRandomKey(db->expires)) == NULL) break;
                t = (time_t) dictGetEntryVal(de);
                if (now > t) {
                    sds key = dictGetEntryKey(de);
                    logiclock = sdslogiclock(key);
                    if (db->logiclock > logiclock) {
                        db->need_remove_key--;
                    }
                    robj *keyobj = createStringObject(key,sdslen(key),sdslogiclock(key),sdsversion(key));
                    dbDelete(db,keyobj);
                    decrRefCount(keyobj);
                    expired++;
                    db->stat_expiredkeys++;
                }
            }
        } while (expired > REDIS_EXPIRELOOKUPS_PER_CRON/4);
    }
    set_malloc_dbnum(dbnum);
}

/* =========================== Server initialization ======================== */

int serverCron(struct redisServer *server) {
    assert(server != NULL);
    int j = 0, loops = server->cronloops;

    /* We have just 24 bits per object for LRU information.
     * So we use an (eventually wrapping) LRU clock with 10 seconds resolution.
     * 2^22 bits with 10 seconds resoluton is more or less 1.5 years.
     *
     * Note that even if this will wrap after 1.5 years it's not a problem,
     * everything will still work but just some object will appear younger
     * to Redis. But for this to happen a given object should never be touched
     * for 1.5 years.
     *
     * Note that you can change the resolution altering the
     * REDIS_LRU_CLOCK_RESOLUTION define.
     */
    updateLRUClock();

    /* Show some info about non-empty databases */
    for (j = 0; j < server->dbnum; j++) {
        long long size = dictSlots(server->db[j].dict);
        long long used = dictSize(server->db[j].dict);
        long long vkeys = dictSize(server->db[j].expires);
        if (!(loops % 50) && (used || vkeys)) {
            redisLog(REDIS_VERBOSE,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
        }
    }

    if (!(loops % 10)) tryResizeHashTables(server);
    if (server->activerehashing) incrementallyRehash(server);

    /* Show information about connected clients */
    if (!(loops % 50)) {
        redisLog(REDIS_VERBOSE,"%zu bytes in use", zmalloc_used_memory());
    }

    /* Expire a few keys per cycle, only if this is a master.
     * On slaves we wait for DEL operations synthesized by the master
     * in order to guarantee a strict consistency. */
    activeExpireCycle(server);

    server->cronloops++;
    return 100;
}


/* =========================== Server initialization ======================== */

void createSharedObjects() {
    updateLRUClock();
    int j;
    for (j = 0; j < REDIS_SHARED_INTEGERS; j++) {
        shared.integers[j] = createObject(REDIS_STRING,(void*)(long)j);
        shared.integers[j]->encoding = REDIS_ENCODING_INT;
    }
}

void freeSharedObjects() {
	int j;
	//TODO free sharedObject

	for(j = 0; j < REDIS_SHARED_INTEGERS; j++) {
		forceFreeObject(shared.integers[j]);
		shared.integers[j] = NULL;
	}
}

void initServer(redisServer *server) {
    server->hash_max_zipmap_entries = REDIS_HASH_MAX_ZIPMAP_ENTRIES;
    server->hash_max_zipmap_value = REDIS_HASH_MAX_ZIPMAP_VALUE;
    server->list_max_ziplist_entries = REDIS_LIST_MAX_ZIPLIST_ENTRIES;
    server->list_max_ziplist_value = REDIS_LIST_MAX_ZIPLIST_VALUE;
    server->set_max_intset_entries = REDIS_SET_MAX_INTSET_ENTRIES;

    server->dbnum = MAX_DBNUM;
    server->maxmemory = memtoll("10gb",NULL);
    server->maxmemory_policy = REDIS_MAXMEMORY_ALLKEYS_LRU;
    server->maxmemory_samples = 3;
    server->activerehashing = 1;

    /* Double constants initialization */
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;

    int j = 0;
    server->clients = listCreate();

    server->db = zmalloc(sizeof(redisDb)*server->dbnum);
    for (j = 0; j < server->dbnum; j++) {
        memset(&(server->db[j]), 0, sizeof(redisDb));
        server->db[j].dict = dictCreate(&dbDictType,NULL);
        server->db[j].expires = dictCreate(&keyptrDictType,NULL);
        server->db[j].id = j;
        server->db[j].maxmemory = REDIS_DEFAULT_DB_MAX_MEMOERY;
        server->db[j].maxmemory_samples = server->maxmemory_samples;

        server->db[j].stat_evictedkeys = 0;
        server->db[j].stat_expiredkeys = 0;
        server->db[j].stat_keyspace_hits = 0;
        server->db[j].stat_keyspace_misses = 0;
        server->db[j].write_count = 0;
        server->db[j].read_count = 0;
        server->db[j].hit_count = 0;
        server->db[j].remove_count = 0;

        server->db[j].logiclock = 1;
        server->db[j].need_remove_key = 0;
    }
    server->dirty = 0;
    server->stat_numcommands = 0;
    server->stat_numconnections = 0;
    server->stat_starttime = time(NULL);
    srand(time(NULL)^getpid());
}

void unInitServer(redisServer* server) {
	int j = 0;

	for(j = 0; j < server->dbnum; j++) {
		dictRelease(server->db[j].dict);
		dictRelease(server->db[j].expires);
	}

	zfree(server->db);

	listRelease(server->clients);
}

/* Call() is the core of Redis execution of a command */
void call(redisClient *c) {
    long long dirty, start = ustime(), duration;

    dirty = c->server->dirty;
    c->cmd->proc(c);
    dirty = c->server->dirty-dirty;
    duration = ustime()-start;

    c->server->stat_numcommands++;
}

/* If this function gets called we already read a whole
 * command, argments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If 1 is returned the client is still alive and valid and
 * and other operations can be performed by the caller. Otherwise
 * if 0 is returned the client was destroied (i.e. after QUIT). */
int processCommand(redisClient *c) {
    /* The QUIT command is handled separately. Normal command procs will
     * go through checking for replication and QUIT will cause trouble
     * when FORCE_REPLICATION is enabled and would be implemented in
     * a regular command proc. */
    struct redisServer *server = c->server;

    /* Handle the maxmemory directive.
     *
     * First we try to free some memory if possible (if there are volatile
     * keys in the dataset). If there are not the only thing we can do
     * is returning an error. */
    if (c->db->maxmemory) freeDBMemoryIfNeeded(c->db);
    if (c->db->maxmemory && zmalloc_db_used_memory(c->db->id) > c->db->maxmemory) {
        return REDIS_ERR_REACH_MAXMEMORY;
    }
    if (server->maxmemory) freeMemoryIfNeeded(server);
    if (server->maxmemory && (c->cmd->flags & REDIS_CMD_DENYOOM) &&
        zmalloc_used_memory() > server->maxmemory)
    {
        return REDIS_ERR_REACH_MAXMEMORY;
    }

    call(c);

    return REDIS_OK;
}

/*================================== Commands =============================== */

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes */
        sprintf(s,"%lluB",n);
        return;
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        sprintf(s,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        sprintf(s,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        sprintf(s,"%.2fG",d);
    }
}

/* ============================ Maxmemory directive  ======================== */

/* This function gets called when 'maxmemory' is set on the config file to limit
 * the max memory used by the server, and we are out of memory.
 * This function will try to, in order:
 *
 * - Free objects from the free list
 * - Try to remove keys with an EXPIRE set
 *
 * It is not possible to free enough memory to reach used-memory < maxmemory
 * the server will start refusing commands that will enlarge even more the
 * memory usage.
 */
void freeMemoryIfNeeded(struct redisServer *server) {
    int i;
    uint16_t logiclock;
    sds thiskey;
    for (i = 0; i < server->dbnum; i++) {
        redisDb *db = server->db+i;
        if (db->need_remove_key > 0) {
            struct dictEntry *de;
            de = dictGetRandomKey(db->dict);
            if (de == NULL) {
                continue;
            }
            thiskey = dictGetEntryKey(de);

            logiclock = sdslogiclock(thiskey);
            if (db->logiclock > logiclock) {
                robj *keyobj = createStringObject(thiskey,sdslen(thiskey),
                        sdslogiclock(thiskey),sdsversion(thiskey));
                dbDelete(db,keyobj);
                db->stat_evictedkeys++;
                db->need_remove_key--;
                decrRefCount(keyobj);
            }
        }
    }

    /* Remove keys accordingly to the active policy as long as we are
     * over the memory limit. */
    if (server->maxmemory_policy == REDIS_MAXMEMORY_NO_EVICTION) return;

    while (server->maxmemory && zmalloc_used_memory() > server->maxmemory) {
        int j, k, freed = 0;

        for (j = 0; j < server->dbnum; j++) {
            long bestval = 0; /* just to prevent warning */
            sds bestkey = NULL;
            struct dictEntry *de;
            redisDb *db = server->db+j;
            dict *dict;

            if (server->maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_LRU ||
                server->maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_RANDOM)
            {
                dict = server->db[j].dict;
            } else {
                dict = server->db[j].expires;
            }
            if (dictSize(dict) == 0) continue;

            /* volatile-random and allkeys-random policy */
            if (server->maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_RANDOM ||
                server->maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_RANDOM)
            {
                de = dictGetRandomKey(dict);
                bestkey = dictGetEntryKey(de);
            }

            /* volatile-lru and allkeys-lru policy */
            else if (server->maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_LRU ||
                server->maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_LRU)
            {
                for (k = 0; k < server->maxmemory_samples; k++) {
                    sds thiskey;
                    long thisval;
                    robj *o;

                    de = dictGetRandomKey(dict);
                    thiskey = dictGetEntryKey(de);
                    /* When policy is volatile-lru we need an additonal lookup
                     * to locate the real key, as dict is set to db->expires. */
                    if (server->maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_LRU)
                        de = dictFind(db->dict, thiskey);
                    o = dictGetEntryVal(de);
                    thisval = estimateObjectIdleTime(o);

                    /* Higher idle time is better candidate for deletion */
                    if (bestkey == NULL || thisval > bestval) {
                        bestkey = thiskey;
                        bestval = thisval;
                    }
                }
            }

            /* volatile-ttl */
            else if (server->maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_TTL) {
                for (k = 0; k < server->maxmemory_samples; k++) {
                    sds thiskey;
                    long thisval;

                    de = dictGetRandomKey(dict);
                    thiskey = dictGetEntryKey(de);
                    thisval = (long) dictGetEntryVal(de);

                    /* Expire sooner (minor expire unix timestamp) is better
                     * candidate for deletion */
                    if (bestkey == NULL || thisval < bestval) {
                        bestkey = thiskey;
                        bestval = thisval;
                    }
                }
            }

            /* Finally remove the selected key. */
            if (bestkey) {
                logiclock = sdslogiclock(bestkey);
                if (db->logiclock > logiclock) {
                    db->need_remove_key--;
                }
                robj *keyobj = createStringObject(bestkey,sdslen(bestkey),
                        sdslogiclock(bestkey),sdsversion(bestkey));
                dbDelete(db,keyobj);
                db->stat_evictedkeys++;
                decrRefCount(keyobj);
                freed++;
            }
        }
        if (!freed) return; /* nothing to free... */
    }
}

int setDBMaxmemory(redisServer *server, int id, uint64_t maxmem) {
    if (id < 0 || id >= server->dbnum)
        return REDIS_ERR;
    server->db[id].maxmemory = maxmem;
    return REDIS_OK;
}


int freeDBMemory(redisDb *db, int expires_db) {
    uint16_t logiclock;
    dict *dict = NULL;
    if (expires_db)
        dict = db->expires;
    else
        dict = db->dict;

    if (dictSize(dict) == 0)
        return 0;

    sds bestkey = NULL;
    struct dictEntry *de = NULL;
    long bestval = 0;
    for (int k = 0; k < db->maxmemory_samples; k++) {
        sds thiskey;
        long thisval;
        robj *o;

        de = dictGetRandomKey(dict);
        thiskey = dictGetEntryKey(de);

        logiclock = sdslogiclock(thiskey);
        if (db->logiclock > logiclock) {
            bestkey = thiskey;
            break;
        }

        /* When policy is volatile-lru we need an additonal lookup
         * to locate the real key, as dict is set to db->expires. */
        if (expires_db)
            de = dictFind(db->dict, thiskey);
        o = dictGetEntryVal(de);
        thisval = estimateObjectIdleTime(o);

        /* Higher idle time is better candidate for deletion */
        if (bestkey == NULL || thisval > bestval) {
            bestkey = thiskey;
            bestval = thisval;
        }
    }
    /* Finally remove the selected key. */
    if (bestkey) {
        logiclock = sdslogiclock(bestkey);
        if (db->logiclock > logiclock) {
            db->need_remove_key--;
        }
        robj *keyobj = createStringObject(bestkey,sdslen(bestkey),sdslogiclock(bestkey),sdsversion(bestkey));
        dbDelete(db,keyobj);
        db->stat_evictedkeys++;
        decrRefCount(keyobj);
        return 1;
    }
    return 0;
}

void freeDBMemoryIfNeeded(struct redisDb *db) {
    /* now just support lru, first volatile-lru, second allkeys-lru*/
    while (db->maxmemory && zmalloc_db_used_memory(db->id) > db->maxmemory) {
        int freed = 0;
        if (freeDBMemory(db, 1))
            freed++;
        else if (freeDBMemory(db, 0))
            freed++;

        if (!freed) return; /* nothing to free... */
    }
}


/* The End */
