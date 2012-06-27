#ifndef __REDIS_H
#define __REDIS_H

#include "fmacros.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <syslog.h>
#include <assert.h>

#include "sds.h"    /* Dynamic safe strings */
#include "dict.h"   /* Hash tables */
#include "adlist.h" /* Linked lists */
#include "zmalloc.h" /* total memory usage aware version of malloc/free */
#include "zipmap.h" /* Compact string -> string data structure */
#include "ziplist.h" /* Compact list data structure */
#include "intset.h" /* Compact integer set structure */

#define REDIS_OK_BUT_ALREADY_EXIST			5
#define REDIS_ERR_EXPIRE_TIME_OUT           4
#define REDIS_OK_NOT_EXIST                  3
#define REDIS_OK_BUT_CONE                   2
#define REDIS_OK_BUT_CZERO                  1
#define REDIS_OK                            0
#define REDIS_ERR                           -1
#define REDIS_ERR_LENGTHZERO                -2
#define REDIS_ERR_REACH_MAXMEMORY           -3
#define REDIS_ERR_UNKNOWN_COMMAND           -4
#define REDIS_ERR_WRONG_NUMBER_ARGUMENTS    -5
#define REDIS_ERR_OPERATION_NOT_PERMITTED   -6
#define REDIS_ERR_QUEUED                    -7
#define REDIS_ERR_LOADINGERR                -8
#define REDIS_ERR_FORBIDDEN_ABOUT_PUBSUB    -9
#define REDIS_ERR_FORBIDDEN_INFO_SLAVEOF    -10
#define REDIS_ERR_VERSION_ERROR             -11
#define REDIS_OK_RANGE_HAVE_NONE			-12
#define REDIS_ERR_WRONG_TYPE_ERROR          -13
#define REDIS_ERR_CNEGO_ERROR               -14
#define REDIS_ERR_IS_NOT_NUMBER             -15
#define REDIS_ERR_INCDECR_OVERFLOW          -16
#define REDIS_ERR_IS_NOT_INTEGER            -17
#define REDIS_ERR_MEMORY_ALLOCATE_ERROR     -18
#define REDIS_ERR_OUT_OF_RANGE              -19
#define REDIS_ERR_IS_NOT_DOUBLE             -20
#define REDIS_ERR_SYNTAX_ERROR              -21
#define REDIS_ERR_NAMESPACE_ERROR           -22
#define REDIS_ERR_DATA_LEN_LIMITED          -23

/* Static server configuration */
#define REDIS_SERVERPORT        6379    /* TCP port */
#define REDIS_MAXIDLETIME       (60*5)  /* default client timeout */
#define REDIS_IOBUF_LEN         1024
#define REDIS_LOADBUF_LEN       1024
#define REDIS_STATIC_ARGS       8
#define REDIS_DEFAULT_DBNUM     16
#define REDIS_CONFIGLINE_MAX    1024
#define REDIS_MAX_SYNC_TIME     60      /* Slave can't take more to sync */
#define REDIS_EXPIRELOOKUPS_PER_CRON    10 /* lookup 10 expires per loop */
#define REDIS_MAX_WRITE_PER_EVENT (1024*64)
#define REDIS_REQUEST_MAX_SIZE (1024*1024*256) /* max bytes in inline command */
#define REDIS_SHARED_INTEGERS 10000
#define REDIS_REPLY_CHUNK_BYTES (5*1500) /* 5 TCP packets with default MTU */
#define REDIS_MAX_LOGMSG_LEN    1024 /* Default maximum length of syslog messages */
#define REDIS_DEFAULT_DB_MAX_MEMOERY 1024*1024*10 /* 10MB */

/* Hash table parameters */
#define REDIS_HT_MINFILL        10      /* Minimal hash table fill 10% */

/* Command flags:
 *   REDIS_CMD_DENYOOM:
 *     Commands marked with this flag will return an error when 'maxmemory' is
 *     set and the server is using more than 'maxmemory' bytes of memory.
 *     In short: commands with this flag are denied on low memory conditions.
 *   REDIS_CMD_FORCE_REPLICATION:
 *     Force replication even if dirty is 0. */
#define REDIS_CMD_DENYOOM 4
#define REDIS_CMD_FORCE_REPLICATION 8

/* Object types */
#define REDIS_STRING 0
#define REDIS_LIST 1
#define REDIS_SET 2
#define REDIS_ZSET 3
#define REDIS_HASH 4
#define REDIS_VMPOINTER 8
#define REDIS_NONE  16
#define REDIS_UNKNOWN  32

/* Objects encoding. Some kind of objects like Strings and Hashes can be
 * internally represented in multiple ways. The 'encoding' field of the object
 * is set to one of this fields for this object. */
#define REDIS_ENCODING_RAW 0     /* Raw representation */
#define REDIS_ENCODING_INT 1     /* Encoded as integer */
#define REDIS_ENCODING_HT 2      /* Encoded as hash table */
#define REDIS_ENCODING_ZIPMAP 3  /* Encoded as zipmap */
#define REDIS_ENCODING_LINKEDLIST 4 /* Encoded as regular linked list */
#define REDIS_ENCODING_ZIPLIST 5 /* Encoded as ziplist */
#define REDIS_ENCODING_INTSET 6  /* Encoded as intset */
#define REDIS_ENCODING_SKIPLIST 7  /* Encoded as skiplist */

/* Object types only used for dumping to disk */
#define REDIS_EXPIRETIME 253
#define REDIS_SELECTDB 254
#define REDIS_EOF 255

/* Defines related to the dump file format. To store 32 bits lengths for short
 * keys requires a lot of space, so we check the most significant 2 bits of
 * the first byte to interpreter the length:
 *
 * 00|000000 => if the two MSB are 00 the len is the 6 bits of this byte
 * 01|000000 00000000 =>  01, the len is 14 byes, 6 bits + 8 bits of next byte
 * 10|000000 [32 bit integer] => if it's 01, a full 32 bit len will follow
 * 11|000000 this means: specially encoded object will follow. The six bits
 *           number specify the kind of object that follows.
 *           See the REDIS_RDB_ENC_* defines.
 *
 * Lenghts up to 63 are stored using a single byte, most DB keys, and may
 * values, will fit inside. */
#define REDIS_RDB_6BITLEN 0
#define REDIS_RDB_14BITLEN 1
#define REDIS_RDB_32BITLEN 2
#define REDIS_RDB_ENCVAL 3
#define REDIS_RDB_LENERR UINT_MAX

/* When a length of a string object stored on disk has the first two bits
 * set, the remaining two bits specify a special encoding for the object
 * accordingly to the following defines: */
#define REDIS_RDB_ENC_INT8 0        /* 8 bit signed integer */
#define REDIS_RDB_ENC_INT16 1       /* 16 bit signed integer */
#define REDIS_RDB_ENC_INT32 2       /* 32 bit signed integer */
#define REDIS_RDB_ENC_LZF 3         /* string compressed with FASTLZ */

/* The following is the *percentage* of completed I/O jobs to process when the
 * handelr is called. While Virtual Memory I/O operations are performed by
 * threads, this operations must be processed by the main thread when completed
 * in order to take effect. */
#define REDIS_MAX_COMPLETED_JOBS_PROCESSED 1

/* Client flags */
#define REDIS_SLAVE 1       /* This client is a slave server */
#define REDIS_MASTER 2      /* This client is a master server */
#define REDIS_MONITOR 4     /* This client is a slave monitor, see MONITOR */
#define REDIS_MULTI 8       /* This client is in a MULTI context */
#define REDIS_BLOCKED 16    /* The client is waiting in a blocking operation */
#define REDIS_IO_WAIT 32    /* The client is waiting for Virtual Memory I/O */
#define REDIS_DIRTY_CAS 64  /* Watched keys modified. EXEC will fail. */
#define REDIS_CLOSE_AFTER_REPLY 128 /* Close after writing entire reply. */
#define REDIS_UNBLOCKED 256 /* This client was unblocked and is stored in
                               server.unblocked_clients */

/* List related stuff */
#define REDIS_HEAD 0
#define REDIS_TAIL 1

/* Sort operations */
#define REDIS_SORT_GET 0
#define REDIS_SORT_ASC 1
#define REDIS_SORT_DESC 2
#define REDIS_SORTKEY_MAX 1024

/* Log levels */
#define REDIS_DEBUG 0
#define REDIS_VERBOSE 1
#define REDIS_NOTICE 2
#define REDIS_WARNING 3

/* Anti-warning macro... */
#define REDIS_NOTUSED(V) ((void) V)

#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^32 elements */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 */

/* Append only defines */
#define APPENDFSYNC_NO 0
#define APPENDFSYNC_ALWAYS 1
#define APPENDFSYNC_EVERYSEC 2

/* Zip structure related defaults */
#define REDIS_HASH_MAX_ZIPMAP_ENTRIES 512
#define REDIS_HASH_MAX_ZIPMAP_VALUE 64
#define REDIS_LIST_MAX_ZIPLIST_ENTRIES 512
#define REDIS_LIST_MAX_ZIPLIST_VALUE 64
#define REDIS_SET_MAX_INTSET_ENTRIES 512

/* Sets operations codes */
#define REDIS_OP_UNION 0
#define REDIS_OP_DIFF 1
#define REDIS_OP_INTER 2

/* Redis maxmemory strategies */
#define REDIS_MAXMEMORY_VOLATILE_LRU 0
#define REDIS_MAXMEMORY_VOLATILE_TTL 1
#define REDIS_MAXMEMORY_VOLATILE_RANDOM 2
#define REDIS_MAXMEMORY_ALLKEYS_LRU 3
#define REDIS_MAXMEMORY_ALLKEYS_RANDOM 4
#define REDIS_MAXMEMORY_NO_EVICTION 5

/* We can print the stacktrace, so our assert is defined this way: */
#define redisAssert(_e) ((_e)?(void)0 : (_redisAssert(#_e,__FILE__,__LINE__),_exit(1)))
#define redisPanic(_e) _redisPanic(#_e,__FILE__,__LINE__),_exit(1)
void _redisAssert(char *estr, char *file, int line);
void _redisPanic(char *msg, char *file, int line);

/*-----------------------------------------------------------------------------
 * Data types
 *----------------------------------------------------------------------------*/
#define NODE_TYPE_NULL      -1
#define NODE_TYPE_ROBJ      0
#define NODE_TYPE_BUFFER    1
#define NODE_TYPE_LONGLONG  2
#define NODE_TYPE_DOUBLE    3

typedef union ret_val {
    long long llnum;
    double dnum;
} ret_val;

typedef struct value_item_node {
    struct value_item_node* pre;
    struct value_item_node* next;
    int8_t type;/* NODE_TYPE_ROBJ,NODE_TYPE_BUFFER,NODE_TYPE_LONGLONG */
    uint32_t size;
    union _obj {
        void* obj;
        double dnum;
        long long llnum;
    } obj;
} value_item_node;


typedef struct value_item_list {
    struct value_item_node* head;
    struct value_item_node* tail;
    int len;
} value_item_list;

typedef struct value_item_iterator {
    struct value_item_node* next;
    int now;
} value_item_iterator;

/* return to push command*/
typedef struct push_return_value {
    uint32_t pushed_num;   /* amount of pushed values successfull*/
    uint32_t list_len;     /* after insert, length of list */
} push_return_value;

/* A redis object, that is a type able to hold a string / list / set */

/* The actual Redis Object */
#define REDIS_LRU_CLOCK_MAX ((1<<21)-1) /* Max value of obj->lru */
#define REDIS_LRU_CLOCK_RESOLUTION 10 /* LRU clock resolution in seconds */
typedef struct redisObject {
    unsigned type:4;
    unsigned encoding:4;
    unsigned lru:24;        /* lru time (relative to server.lruclock) */
    int refcount;
    void *ptr;
} robj;

/* Macro used to initalize a Redis object allocated on the stack.
 * Note that this macro is taken near the structure definition to make sure
 * we'll update it when the structure is changed, to avoid bugs like
 * bug #85 introduced exactly in this way. */
#define initStaticStringObject(_var,_ptr) do { \
    _var.refcount = 1; \
    _var.type = REDIS_STRING; \
    _var.encoding = REDIS_ENCODING_RAW; \
    _var.ptr = _ptr; \
} while(0);

#define EXPIRE_OR_NOT if(c->expiretime > 0) {   \
    setXExpire(c->db,c->argv[1],c->expiretime); \
} else if (c->expiretime == 0){     \
    removeXExpire(c->db,c->argv[1]); \
}

typedef struct redisDb {
#ifdef __cplusplus
    struct dict *dict;
    struct dict *expires;              /* Timeout of keys with a timeout set */
#else
    dict *dict;                 /* The keyspace for this DB */
    dict *expires;              /* Timeout of keys with a timeout set */
#endif
    int id;

    long long stat_evictedkeys;     /* number of evicted keys (maxmemory) */
    long long stat_expiredkeys;     /* number of expired keys */
    long long stat_keyspace_hits;
    long long stat_keyspace_misses;
    unsigned long long maxmemory;
    int write_count;
    int read_count;
    int hit_count;
    int remove_count;
    int maxmemory_samples;
    uint16_t logiclock;
    size_t need_remove_key;
} redisDb;

/* With multiplexing we need to take per-clinet state.
 * Clients are taken in a liked list. */
typedef struct redisClient {
    redisDb *db;
    int dictid;

    int old_dbnum;          /* last db num */
    int oldargc;            /* last max argc */
    int argc;
    robj **argv;
    struct redisCommand *cmd;

    /* Request version care*/
    char version_care;

    /* Response buffer */
    uint16_t version;
    long expiretime;
    int returncode; //return code for example REDIS_OK;
    void* return_value; //return value by list
    ret_val retvalue;  //integer or double value

    struct redisServer *server;
} redisClient;

struct saveparam {
    time_t seconds;
    int changes;
};

struct sharedObjectsStruct {
    robj *integers[REDIS_SHARED_INTEGERS];
    unsigned lruclock:22;        /* clock incrementing every minute, for LRU */
};

struct redisLogConfig {
    int verbosity;
    char *logfile;
    int syslog_enabled;
    char *syslog_ident;
    int syslog_facility;
} ;


struct redisServer {
    pthread_t mainthread;
    redisDb *db;
    long long dirty;            /* changes to DB from the last save */
    list *clients;
    int cronloops;              /* number of times the cron function run */
    /* Fields used only for stats */
    time_t stat_starttime;          /* server start time */
    long long stat_numcommands;     /* number of processed commands */
    long long stat_numconnections;  /* number of connections received */
    /* Configuration */
    int maxidletime;
    int dbnum;
    int activerehashing;
    /* Limits */
    unsigned long long maxmemory;
    int maxmemory_policy;
    int maxmemory_samples;
    /* Zip structure config */
    size_t hash_max_zipmap_entries;
    size_t hash_max_zipmap_value;
    size_t list_max_ziplist_entries;
    size_t list_max_ziplist_value;
    size_t set_max_intset_entries;

    int list_max_size;
    int hash_max_size;
    int set_max_size;
    int zset_max_size;
    /* Misc */
    unsigned lruclock_padding:10;
};

typedef void redisCommandProc(redisClient *c);
struct redisCommand {
    char *name;
    redisCommandProc *proc;
    int argc;
    int flags;
};

struct redisFunctionSym {
    char *name;
    unsigned long pointer;
};

typedef struct _redisSortObject {
    robj *obj;
    union {
        double score;
        robj *cmpobj;
    } u;
} redisSortObject;

typedef struct _redisSortOperation {
    int type;
    robj *pattern;
} redisSortOperation;

/* ZSETs use a specialized version of Skiplists */
typedef struct zskiplistNode {
    robj *obj;
    double score;
    struct zskiplistNode *backward;
    struct zskiplistLevel {
        struct zskiplistNode *forward;
        unsigned int span;
    } level[];
} zskiplistNode;

typedef struct zskiplist {
    struct zskiplistNode *header, *tail;
    unsigned long length;
    int level;
} zskiplist;

typedef struct zset {
#ifdef __cplusplus
    struct dict *dict;
#else
    dict *dict;
#endif
    zskiplist *zsl;
} zset;


/* Structure to hold list iteration abstraction. */
typedef struct {
    robj *subject;
    unsigned char encoding;
    unsigned char direction; /* Iteration direction */
    unsigned char *zi;
    listNode *ln;
} listTypeIterator;

/* Structure for an entry while iterating over a list. */
typedef struct {
    listTypeIterator *li;
    unsigned char *zi;  /* Entry in ziplist */
    listNode *ln;       /* Entry in linked list */
} listTypeEntry;

/* Structure to hold set iteration abstraction. */
typedef struct {
    robj *subject;
    int encoding;
    int ii; /* intset iterator */
    dictIterator *di;
} setTypeIterator;

/* Structure to hold hash iteration abstration. Note that iteration over
 * hashes involves both fields and values. Because it is possible that
 * not both are required, store pointers in the iterator to avoid
 * unnecessary memory allocation for fields/values. */
typedef struct {
    int encoding;
    unsigned char *zi;
    unsigned char *zk, *zv;
    unsigned int zklen, zvlen;

    dictIterator *di;
    dictEntry *de;
} hashTypeIterator;

#define REDIS_HASH_KEY 1
#define REDIS_HASH_VALUE 2

/*-----------------------------------------------------------------------------
 * Extern declarations
 *----------------------------------------------------------------------------*/

#ifdef TAIR_STORAGE
extern struct redisShared serverShared;
extern struct redisLogConfig logConfig;
#else
extern struct redisServer server;
#endif
extern struct sharedObjectsStruct shared;
extern dictType setDictType;
extern dictType zsetDictType;
extern double R_Zero, R_PosInf, R_NegInf, R_Nan;
// yexiang: redis bug ?
extern dictType hashDictType;

/*-----------------------------------------------------------------------------
 * Functions prototypes
 *----------------------------------------------------------------------------*/
/*return data list  */
value_item_iterator* createValueItemIterator(value_item_list* list);
value_item_node* nextValueItemNode(value_item_iterator** it);
void freeValueItemIterator(value_item_iterator** it);
value_item_list* createValueItemList();
void freeValueItemList(value_item_list* list);
value_item_node* createValueItemNode(robj* obj);
value_item_node* createGenericValueItemNode(void* obj,uint32_t size,int type);
value_item_node* createDoubleValueItemNode(double score);
value_item_node* createLongLongValueItemNode(long long llnum);
void freeValueItemNode(value_item_node* node);
int rpushValueItemNode(value_item_list* list, robj* obj);
int rpushGenericValueItemNode(value_item_list* list,void* obj,uint32_t size,int type);
int rpushDoubleValueItemNode(value_item_list* list, double score);
int rpushLongLongValueItemNode(value_item_list* list, long long llnum);
int lpushValueItemNode(value_item_list* list, robj* obj);
int lpushGenericValueItemNode(value_item_list* list,void* obj,uint32_t size,int type);
int lpushDoubleValueItemNode(value_item_list* list, double score);
void removeValueItemNode(value_item_node* node);
value_item_node* lpopValueItemNode(value_item_list* list);
value_item_node* rpopValueItemNode(value_item_list* list);
int getValueItemNodeType(value_item_node* node);
int getValueItemNodeSize(value_item_node* node);

/* networking.c -- Networking and Client related operations */
void closeTimedoutClients(void);
void resetClient(redisClient *c);

redisClient *createClient(struct redisServer *server);
void freeClient(struct redisServer *server, redisClient *c);
void clearClientQueryBuf(redisClient *client);
void readQueryFromClient(redisClient *client, char *buf, int len);

void *dupClientReplyValue(void *o);
void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer);
//void rewriteClientCommandVector(redisClient *c, int argc, ...);

/* List data type */
void listTypeTryConversion(redisClient *c, robj *subject, robj *value);
void listTypePush(redisClient *c, robj *subject, robj *value, int where);
robj *listTypePop(robj *subject, int where);
unsigned long listTypeLength(robj *subject);
listTypeIterator *listTypeInitIterator(robj *subject, int index, unsigned char direction);
void listTypeReleaseIterator(listTypeIterator *li);
int listTypeNext(listTypeIterator *li, listTypeEntry *entry);
robj *listTypeGet(listTypeEntry *entry);
void listTypeInsert(listTypeEntry *entry, robj *value, int where);
int listTypeEqual(listTypeEntry *entry, robj *o);
void listTypeDelete(listTypeEntry *entry);
void listTypeConvert(robj *subject, int enc);
void popGenericCommand(redisClient *c, int where);

/* Redis object implementation */
void decrRefCount(void *o);
void incrRefCount(robj *o);
void forceFreeObject(void *o);
void freeStringObject(robj *o);
void freeKeyStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
robj *createObject(int type, void *ptr);
robj *createStringObject(char *ptr, size_t len, uint16_t logiclock, uint16_t version);
robj *dupStringObject(robj *o);
robj *tryObjectEncoding(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createListObject();
robj *createZiplistObject();
robj *createSetObject();
robj *createIntsetObject();
robj *createHashObject();
robj *createZsetObject();
int getLongFromObject(robj *o, long *target);
int checkType(redisClient *c, robj *o, int type);
int getDoubleFromObject(robj *o, double *target);
int getLongLongFromObject(robj *o, long long *target);
char *strEncoding(int encoding);
int compareStringObjects(robj *a, robj *b);
int equalStringObjects(robj *a, robj *b);
unsigned long estimateObjectIdleTime(robj *o);

/* Synchronous I/O with timeout */
int syncWrite(int fd, char *ptr, ssize_t size, int timeout);
int syncRead(int fd, char *ptr, ssize_t size, int timeout);
int syncReadLine(int fd, char *ptr, ssize_t size, int timeout);
int fwriteBulkString(FILE *fp, char *s, unsigned long len);
int fwriteBulkDouble(FILE *fp, double d);
int fwriteBulkLongLong(FILE *fp, long long l);
int fwriteBulkObject(FILE *fp, robj *obj);

/* Sorted sets data type */
zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
zskiplistNode *zslInsert(zskiplist *zsl, double score, robj *obj);

/* Core functions */
void freeMemoryIfNeeded(struct redisServer *server);
void freeDBMemoryIfNeeded(struct redisDb *db);
int processCommand(redisClient *c);
void setupSignalHandlers(void);
struct redisCommand *lookupCommand(redisServer *server, sds name);
struct redisCommand *lookupCommandByCString(redisServer *server, char *s);
void call(redisClient *c);
int prepareForShutdown();
void redisLog(int level, const char *fmt, ...);
void usage();
void updateDictResizePolicy(redisServer *server);
int htNeedsResize(dict *dict);
void oom(const char *msg);
#ifdef TAIR_STORAGE
void populateCommandTable(struct redisServer *server);
#else
void populateCommandTable(void);
#endif

/* Set data type */
robj *setTypeCreate(robj *value);
int setTypeAdd(struct redisClient *c, robj *subject, robj *value);
int setTypeRemove(robj *subject, robj *value);
int setTypeIsMember(robj *subject, robj *value);
setTypeIterator *setTypeInitIterator(robj *subject);
void setTypeReleaseIterator(setTypeIterator *si);
int setTypeNext(setTypeIterator *si, robj **objele, int64_t *llele);
robj *setTypeNextObject(setTypeIterator *si);
int setTypeRandomElement(robj *setobj, robj **objele, int64_t *llele);
unsigned long setTypeSize(robj *subject);
void setTypeConvert(robj *subject, int enc);

/* Hash data type */
void convertToRealHash(robj *o);
void hashTypeTryConversion(redisClient *c, robj *subject, robj **argv, int start, int end);
void hashTypeTryObjectEncoding(robj *subject, robj **o1, robj **o2);
int hashTypeGet(robj *o, robj *key, robj **objval, unsigned char **v, unsigned int *vlen);
robj *hashTypeGetObject(robj *o, robj *key);
int hashTypeExists(robj *o, robj *key);
int hashTypeSet(redisClient *c, robj *o, robj *key, robj *value);
int hashTypeDelete(robj *o, robj *key);
unsigned long hashTypeLength(robj *o);
hashTypeIterator *hashTypeInitIterator(robj *subject);
void hashTypeReleaseIterator(hashTypeIterator *hi);
int hashTypeNext(hashTypeIterator *hi);
int hashTypeCurrent(hashTypeIterator *hi, int what, robj **objval, unsigned char **v, unsigned int *vlen);
robj *hashTypeCurrentObject(hashTypeIterator *hi, int what);
robj *hashTypeLookupWriteOrCreate(redisClient *c, robj *key);

/* Utility functions */
int stringmatchlen(const char *pattern, int patternLen,
        const char *string, int stringLen, int nocase);
int stringmatch(const char *pattern, const char *string, int nocase);
long long memtoll(const char *p, int *err);
int ll2string(char *s, size_t len, long long value);
int isStringRepresentableAsLong(sds s, long *longval);
int isStringRepresentableAsLongLong(sds s, long long *longval);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
long long ustime(void);

/* Configuration */
void appendServerSaveParams(time_t seconds, int changes);
#ifdef TAIR_STORAGE
void loadServerConfig(struct redisServer *);
void unLoadServerConfig(struct redisServer *);
void resetServerSaveParams(struct redisServer *);
#else
void loadServerConfig(char *filename);
void resetServerSaveParams();
#endif
/* db.c -- Keyspace access API */
int removeExpire(redisDb *db, robj *key);
int removeXExpire(redisDb *db, robj *key);
void propagateExpire(redisDb *db, robj *key);
int expireIfNeeded(redisDb *db, robj *key);
time_t getExpire(redisDb *db, robj *key);
void setExpire(redisDb *db, robj *key, time_t when);
void setXExpire(redisDb *db, robj *key, time_t when);
robj *lookupKeyWithVersion(redisDb *db, robj *key, uint16_t *version);
robj *lookupKeyReadWithVersion(redisDb *db, robj *key, uint16_t *version);
robj *lookupKeyWriteWithVersion(redisDb *db, robj *key, uint16_t *version);
robj *lookupKeyReadOrReplyWithVersion(redisClient *c, robj *key, robj *reply, uint16_t *version);
robj *lookupKeyReadOrStatusReplyWithVersion(redisClient *c, robj *key, robj *reply, uint16_t *version);
robj *lookupKeyWriteOrReplyWithVersion(redisClient *c, robj *key, robj *reply, uint16_t *version);
int dbAdd(redisDb *db, robj *key, robj *val);
int dbReplace(redisDb *db, robj *key, robj *val);
int dbUpdateKey(redisDb *db, robj* key);
int dbSuperReplace(redisDb *db, robj *key, robj *val);
int dbExists(redisDb *db, robj *key);
robj *dbRandomKey(redisDb *db);
int dbDelete(redisDb *db, robj *key);
long long emptyDb();
int selectDb(redisClient *c, int id);

/* Git SHA1 */
char *redisGitSHA1(void);
char *redisGitDirty(void);

int setDBMaxmemory(redisServer *server, int db, uint64_t maxmem);

/* Commands prototypes */
void setCommand(redisClient *c);
void setnxCommand(redisClient *c);
void setnxexCommand(redisClient *c);
void setexCommand(redisClient *c);
void getCommand(redisClient *c);
void delCommand(redisClient *c);
void existsCommand(redisClient *c);
void incrCommand(redisClient *c);
void decrCommand(redisClient *c);
void incrbyCommand(redisClient *c);
void decrbyCommand(redisClient *c);
void lpushCommand(redisClient *c);
void rpushCommand(redisClient *c);
void lpushxCommand(redisClient *c);
void rpushxCommand(redisClient *c);
void linsertCommand(redisClient *c);
void lpopCommand(redisClient *c);
void rpopCommand(redisClient *c);
void llenCommand(redisClient *c);
void lindexCommand(redisClient *c);
void lrangeCommand(redisClient *c);
void ltrimCommand(redisClient *c);
void typeCommand(redisClient *c);
void lsetCommand(redisClient *c);
void saddCommand(redisClient *c);
void sremCommand(redisClient *c);
void smoveCommand(redisClient *c);
void sismemberCommand(redisClient *c);
void scardCommand(redisClient *c);
void spopCommand(redisClient *c);
void sinterCommand(redisClient *c);
void sinterstoreCommand(redisClient *c);
void syncCommand(redisClient *c);
void flushdbCommand(redisClient *c);
void flushallCommand(redisClient *c);
void lremCommand(redisClient *c);
void infoCommand(redisClient *c);
void monitorCommand(redisClient *c);
void expireCommand(redisClient *c);
void getsetCommand(redisClient *c);
void ttlCommand(redisClient *c);
void persistCommand(redisClient *c);
void slaveofCommand(redisClient *c);
void zaddCommand(redisClient *c);
void zincrbyCommand(redisClient *c);
void zrangeCommand(redisClient *c);
void zrangewithscoreCommand(redisClient *c);
void zrangebyscoreCommand(redisClient *c);
void zrevrangebyscoreCommand(redisClient *c);
void zcountCommand(redisClient *c);
void zrevrangeCommand(redisClient *c);
void zrevrangewithscoreCommand(redisClient *c);
void zcardCommand(redisClient *c);
void zremCommand(redisClient *c);
void zscoreCommand(redisClient *c);
void zremrangebyscoreCommand(redisClient *c);
void discardCommand(redisClient *c);
void blpopCommand(redisClient *c);
void brpopCommand(redisClient *c);
void brpoplpushCommand(redisClient *c);
void zrankCommand(redisClient *c);
void zrevrankCommand(redisClient *c);
void hsetCommand(redisClient *c);
void hsetnxCommand(redisClient *c);
void hgetCommand(redisClient *c);
void hmsetCommand(redisClient *c);
void hmgetCommand(redisClient *c);
void hdelCommand(redisClient *c);
void hlenCommand(redisClient *c);
void zremrangebyrankCommand(redisClient *c);
void zunionstoreCommand(redisClient *c);
void zinterstoreCommand(redisClient *c);
void hkeysCommand(redisClient *c);
void hvalsCommand(redisClient *c);
void hgetallCommand(redisClient *c);
void hexistsCommand(redisClient *c);
void hincrbyCommand(redisClient *c);


void initServer(redisServer *server);
void unInitServer(redisServer *server);
void initServerConfig(struct redisServer *server);
void unInitServerConfig(struct redisServer *server);
void createSharedObjects();
void freeSharedObjects();
int serverCron(struct redisServer *server);
size_t dbSize(redisDb *db);

#if defined(__GNUC__)
void *calloc(size_t count, size_t size) __attribute__ ((deprecated));
void free(void *ptr) __attribute__ ((deprecated));
void *malloc(size_t size) __attribute__ ((deprecated));
void *realloc(void *ptr, size_t size) __attribute__ ((deprecated));
#endif

#endif
