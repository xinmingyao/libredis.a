#ifndef __REDIS_GLOBAL_COMMAND__
#define __REDIS_GLOBAL_COMMAND__

#include "redis.h"

//struct redisCommand *commandTable;
struct redisCommand readonlyCommandTable[] = {
#define GET_COMMAND 0
    {"get",getCommand,2,0},
#define SET_COMMAND 1
    {"set",setCommand,3,REDIS_CMD_DENYOOM},
#define SETNX_COMMAND 2
    {"setnx",setnxCommand,3,REDIS_CMD_DENYOOM},
#define SETEX_COMMAND 3
    {"setex",setexCommand,4,REDIS_CMD_DENYOOM},
#define DEL_COMMAND 4
    {"del",delCommand,2,0},
#define EXISTS_COMMAND 5
    {"exists",existsCommand,2,0},
#define INCR_COMMAND 6
    {"incr",incrCommand,2,REDIS_CMD_DENYOOM},
#define DECR_COMMAND 7
    {"decr",decrCommand,2,REDIS_CMD_DENYOOM},
#define RPUSH_COMMAND 8
    {"rpush",rpushCommand,3,REDIS_CMD_DENYOOM},
#define LPUSH_COMMAND 9
    {"lpush",lpushCommand,3,REDIS_CMD_DENYOOM},
#define RPUSHX_COMMAND 10
    {"rpushx",rpushxCommand,3,REDIS_CMD_DENYOOM},
#define LPUSHX_COMMAND 11
    {"lpushx",lpushxCommand,3,REDIS_CMD_DENYOOM},
#define LINSERT_COMMAND 12
    {"linsert",linsertCommand,5,REDIS_CMD_DENYOOM},
#define RPOP_COMMAND 13
    {"rpop",rpopCommand,3,0},
#define LPOP_COMMAND 14
    {"lpop",lpopCommand,3,0},
#define LLEN_COMMAND 15
    {"llen",llenCommand,2,0},
#define LINDEX_COMMAND 16
    {"lindex",lindexCommand,3,0},
#define LSET_COMMAND 17
    {"lset",lsetCommand,4,REDIS_CMD_DENYOOM},
#define LRANGE_COMMAND 18
    {"lrange",lrangeCommand,4,0},
#define LTRIM_COMMAND 19
    {"ltrim",ltrimCommand,4,0},
#define LREM_COMMAND 20
    {"lrem",lremCommand,4,0},
#define SADD_COMMAND 21
    {"sadd",saddCommand,3,REDIS_CMD_DENYOOM},
#define SREM_COMMAND 22
    {"srem",sremCommand,3,0},
#define SMOVE_COMMAND 23
    {"smove",smoveCommand,4,0},
#define SISMEMBER_COMMAND 24
    {"sismember",sismemberCommand,3,0},
#define SCARD_COMMAND 25
    {"scard",scardCommand,2,0},
#define SPOP_COMMAND 26
    {"spop",spopCommand,2,0},
#define SINTER_COMMAND 27
    {"sinter",sinterCommand,2,REDIS_CMD_DENYOOM},
#define SINTERSTORE_COMMAND 28
    {"sinterstore",sinterstoreCommand,3,REDIS_CMD_DENYOOM},
#define SMEMBERS_COMMAND 29
    {"smembers",sinterCommand,2,0},
#define ZADD_COMMAND 30
    {"zadd",zaddCommand,4,REDIS_CMD_DENYOOM},
#define ZINCRBY_COMMAND 31
    {"zincrby",zincrbyCommand,4,REDIS_CMD_DENYOOM},
#define ZREM_COMMAND 32
    {"zrem",zremCommand,3,0},
#define ZREMRANGEBYSCORE_COMMAND 33
    {"zremrangebyscore",zremrangebyscoreCommand,4,0},
#define ZREMRANGEBYRANK_COMMAND 34
    {"zremrangebyrank",zremrangebyrankCommand,4,0},
#define ZRANGE_COMMAND 35
    {"zrange",zrangeCommand,4,0},
#define ZRANGEBYSCORE_COMMAND 36
    {"zrangebyscore",zrangebyscoreCommand,6,0},
#define ZREVRANGEBYSCORE_COMMAND 37
    {"zrevrangebyscore",zrevrangebyscoreCommand,6,0},
#define ZCOUNT_COMMAND 38
    {"zcount",zcountCommand,4,0},
#define ZREVRANGE_COMMAND 39
    {"zrevrange",zrevrangeCommand,4,0},
#define ZCARD_COMMAND 40
    {"zcard",zcardCommand,2,0},
#define ZSCORE_COMMAND 41
    {"zscore",zscoreCommand,3,0},
#define ZRANK_COMMAND 42
    {"zrank",zrankCommand,3,0},
#define ZREVRANK_COMMAND 43
    {"zrevrank",zrevrankCommand,3,0},
#define HSET_COMMAND 44
    {"hset",hsetCommand,4,REDIS_CMD_DENYOOM},
#define HSETNX_COMMAND 45
    {"hsetnx",hsetnxCommand,4,REDIS_CMD_DENYOOM},
#define HGET_COMMAND 46
    {"hget",hgetCommand,3,0},
#define HMSET_COMMAND 47
    {"hmset",hmsetCommand,4,REDIS_CMD_DENYOOM},
#define HMGET_COMMAND 48
    {"hmget",hmgetCommand,3,0},
#define HINCRBY_COMMAND 49
    {"hincrby",hincrbyCommand,4,REDIS_CMD_DENYOOM},
#define HDEL_COMMAND 50
    {"hdel",hdelCommand,3,0},
#define HLEN_COMMAND 51
    {"hlen",hlenCommand,2,0},
#define HKEYS_COMMAND 52
    {"hkeys",hkeysCommand,2,0},
#define HVALS_COMMAND 53
    {"hvals",hvalsCommand,2,0},
#define HGETALL_COMMAND 54
    {"hgetall",hgetallCommand,2,0},
#define HEXISTS_COMMAND 55
    {"hexists",hexistsCommand,3,0},
#define INCRBY_COMMAND 56
    {"incrby",incrbyCommand,4,REDIS_CMD_DENYOOM},
#define DECRBY_COMMAND 57
    {"decrby",decrbyCommand,3,REDIS_CMD_DENYOOM},
#define GETSET_COMMAND 58
    {"getset",getsetCommand,3,REDIS_CMD_DENYOOM},
#define EXPIRE_COMMAND 59
    {"expire",expireCommand,3,0},
#define TYPE_COMMAND 60
    {"type",typeCommand,2,0},
#define TTL_COMMAND 61
    {"ttl",ttlCommand,2,0},
#define PERSIST_COMMAND 62
    {"persist",persistCommand,2,0},
#define ZRANGEWITHSCORE_COMMAND 63
    {"zrangewithscore",zrangewithscoreCommand,4,0},
#define ZREVRANGEWITHSCORE_COMMAND 64
    {"zrevrangewithscore",zrevrangewithscoreCommand,4,0},
#define SETNXEX_COMMAND 65
    {"setnxex",setnxexCommand,4,REDIS_CMD_DENYOOM}
};

#define getCommand(cmd) (&readonlyCommandTable[cmd])
#define getCommandArgc(cmd) (readonlyCommandTable[cmd].argc)

char* STRING_BEFORE = "BEFORE";
char* STRING_AFTER = "AFTER";
#define STRING_BEFORE_LEN 6
#define STRING_AFTER_LEN  5

#endif
