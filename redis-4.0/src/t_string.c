/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
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

#include "server.h"
#include <math.h> /* isnan(), isinf() */

/*-----------------------------------------------------------------------------
 * String Commands
 * 字符串相关命令
 *----------------------------------------------------------------------------*/

/*
 * 检查字符串大小，不能大于512MB
 */
static int checkStringLength(client *c, long long size) {
    if (size > 512*1024*1024) {
        addReplyError(c,"string exceeds maximum allowed size (512MB)");
        return C_ERR;
    }
    return C_OK;
}

/* The setGenericCommand() function implements the SET operation with different
 * options and variants. This function is called in order to implement the
 * following commands: SET, SETEX, PSETEX, SETNX.
 * setGenericCommand函数根据不同参数和条件实现不同的SET操作：SET、SETEX、PSETEX、SETNX
 *
 * 'flags' changes the behavior of the command (NX or XX, see belove).
 * 'flag' 参数改变命令的行为，比如是不存在还是存在才需要SET
 *
 * 'expire' represents an expire to set in form of a Redis object as passed
 * by the user. It is interpreted according to the specified 'unit'.
 * 'expire' 参数表示需要设置的过期时间，根据'unit'(单位)解析出真正的时间值
 *
 * 'ok_reply' and 'abort_reply' is what the function will reply to the client
 * if the operation is performed, or when it is not because of NX or
 * XX flags.
 * 'ok_reply' 和 'abort_reply' 参数表示函数成功执行后会返回给客户端的信息，如果NX或者XX标志设置了，就不返回
 *
 * If ok_reply is NULL "+OK" is used.
 * If abort_reply is NULL, "$-1" is used.
 * 如果ok_reply为NULL，则使用的是OK
 * 如果abort_reply为NULL，则使用-1
 */

#define OBJ_SET_NO_FLAGS 0
#define OBJ_SET_NX (1<<0)     /* 如果key不存在，设置值 */
#define OBJ_SET_XX (1<<1)     /* 如果key存在，设置它的值 */
#define OBJ_SET_EX (1<<2)     /* 如果给定的时间单位是秒，设置key并设置过期时间 */
#define OBJ_SET_PX (1<<3)     /* 给定的时间单位是毫秒，设置key并设置过期时间 */

void setGenericCommand(client *c, int flags, robj *key, robj *val, robj *expire, int unit, robj *ok_reply, robj *abort_reply) {
    long long milliseconds = 0; /* 初始化，避免报错 */

    // 如果需要设置超时时间，根据unit单位参数设置超时时间
    if (expire) {
	// 获取时间值
        if (getLongLongFromObjectOrReply(c, expire, &milliseconds, NULL) != C_OK)
            return;
	// 处理非法的时间值
        if (milliseconds <= 0) {
            addReplyErrorFormat(c,"invalid expire time in %s",c->cmd->name);
            return;
        }
        if (unit == UNIT_SECONDS) milliseconds *= 1000; // 统一用转换成毫秒
    }

    /* 
     * 处理非法情况
     * 如果flags为OBJ_SET_NX 且 key存在或者flags为OBJ_SET_XX且key不存在，函数终止并返回abort_reply的值
     */
    if ((flags & OBJ_SET_NX && lookupKeyWrite(c->db,key) != NULL) ||
        (flags & OBJ_SET_XX && lookupKeyWrite(c->db,key) == NULL))
    {
        addReply(c, abort_reply ? abort_reply : shared.nullbulk);
        return;
    }
    // 设置val到key中
    setKey(c->db,key,val);
    // 增加服务器的dirty值 TODO：含义，原因？
    server.dirty++;
    // 设置过期时间
    if (expire) setExpire(c,c->db,key,mstime()+milliseconds);
    // 通知监听了key的数据库，key被操作了set、expire命令
    notifyKeyspaceEvent(NOTIFY_STRING,"set",key,c->db->id);
    if (expire) notifyKeyspaceEvent(NOTIFY_GENERIC,
        "expire",key,c->db->id);
    // 返回成功的信息
    addReply(c, ok_reply ? ok_reply : shared.ok);
}

/* 
 * SET命令的实现
 * 命令可以接受参数：key、value、不存在/存在选项、过期时间（可选 ）
 * SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>]
 */
void setCommand(client *c) {
    int j;
    robj *expire = NULL;
    int unit = UNIT_SECONDS;
    int flags = OBJ_SET_NO_FLAGS;

    // 从第三个参数(value之后的参数)开始
    for (j = 3; j < c->argc; j++) {
        char *a = c->argv[j]->ptr;
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1];

        if ((a[0] == 'n' || a[0] == 'N') &&
            (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
            !(flags & OBJ_SET_XX))
        {
	    // NX标志
            flags |= OBJ_SET_NX;
        } else if ((a[0] == 'x' || a[0] == 'X') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_NX))
        {
	    // XX标志
            flags |= OBJ_SET_XX;
        } else if ((a[0] == 'e' || a[0] == 'E') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_PX) && next)
        {
	    // EX处理
            flags |= OBJ_SET_EX;
            unit = UNIT_SECONDS;
            expire = next;
            j++;
        } else if ((a[0] == 'p' || a[0] == 'P') &&
                   (a[1] == 'x' || a[1] == 'X') && a[2] == '\0' &&
                   !(flags & OBJ_SET_EX) && next)
        {
	    // PX处理
            flags |= OBJ_SET_PX;
            unit = UNIT_MILLISECONDS;
            expire = next;
            j++;
        } else {
	    // 错误参数
            addReply(c,shared.syntaxerr);
            return;
        }
    }

    // 对value进行编码
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    // 调用setGenericCommand执行真正的操作
    setGenericCommand(c,flags,c->argv[1],c->argv[2],expire,unit,NULL,NULL);
}

/*
 * setnx命令实现
 */
void setnxCommand(client *c) {
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    setGenericCommand(c,OBJ_SET_NX,c->argv[1],c->argv[2],NULL,0,shared.cone,shared.czero);
}

/*
 * setex命令实现
 */
void setexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_SECONDS,NULL,NULL);
}

/*
 * psetex命令实现
 */
void psetexCommand(client *c) {
    c->argv[3] = tryObjectEncoding(c->argv[3]);
    setGenericCommand(c,OBJ_SET_NO_FLAGS,c->argv[1],c->argv[3],c->argv[2],UNIT_MILLISECONDS,NULL,NULL);
}

/*
 * get命令的"通用"实现
 */
int getGenericCommand(client *c) {
    robj *o;

    // 调用lookupKeyReadOrReply函数查找指定key，找不到，返回
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.nullbulk)) == NULL)
        return C_OK;

    // 如果找到的对象类型不是string返回类型错误
    if (o->type != OBJ_STRING) {
        addReply(c,shared.wrongtypeerr);
        return C_ERR;
    } else {
        addReplyBulk(c,o);
        return C_OK;
    }
}

/*
 * get命令
 * 调用getGenericCommand函数实现具体操作
 */
void getCommand(client *c) {
    getGenericCommand(c);
}

/*
 * getset命令实现
 */
void getsetCommand(client *c) {
    // 先get key
    if (getGenericCommand(c) == C_ERR) return;
    // 对新值进行编码
    c->argv[2] = tryObjectEncoding(c->argv[2]);
    // 设置新值到key中
    setKey(c->db,c->argv[1],c->argv[2]);
    // 通知监听了key的数据库，key执行了set命令
    notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[1],c->db->id);
    server.dirty++;
}

/*
 * setrange命令实现
 */
void setrangeCommand(client *c) {
    robj *o;
    long offset;
    sds value = c->argv[3]->ptr;

    /* 读取offset值到offset中 */
    if (getLongFromObjectOrReply(c,c->argv[2],&offset,NULL) != C_OK)
        return;

    /* 非法offset */
    if (offset < 0) {
        addReplyError(c,"offset is out of range");
        return;
    }

    /* 查找key是否存在，并可被写 */
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* 如果key对象不存在，且需要设置的value为空，返回0 */
        if (sdslen(value) == 0) {
            addReply(c,shared.czero);
            return;
        }

        /* 检查value大小是否满足 */
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

	// 创建一个新的字符串对象
        o = createObject(OBJ_STRING,sdsnewlen(NULL, offset+sdslen(value)));
	// 添加键值对到数据库中
        dbAdd(c->db,c->argv[1],o);
    } else {
        size_t olen;

        /* key存在，检查值类型是否为string，如果不是，返回 */
        if (checkType(c,o,OBJ_STRING))
            return;

        /* 如果设置的value为空，返回当前已存在的字符串的长度 */
        olen = stringObjectLen(o);
        if (sdslen(value) == 0) {
            addReplyLongLong(c,olen);
            return;
        }

        /* 检查value大小是否满足 */
        if (checkStringLength(c,offset+sdslen(value)) != C_OK)
            return;

        /* 如果对象是可共享或者可编码为字符串的，创建一份拷贝 */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
    }

    // 如果需要设置的内容是有效的,执行操作
    if (sdslen(value) > 0) {
	// sdsgrowzero申请更多的空间
        o->ptr = sdsgrowzero(o->ptr,offset+sdslen(value));
	// 拷贝值
        memcpy((char*)o->ptr+offset,value,sdslen(value));
	// 通知数据库执行了setrange操作
        signalModifiedKey(c->db,c->argv[1]);
        notifyKeyspaceEvent(NOTIFY_STRING,
            "setrange",c->argv[1],c->db->id);
        server.dirty++;
    }
    addReplyLongLong(c,sdslen(o->ptr));
}

/*
 * getrange命令实现
*/
void getrangeCommand(client *c) {
    robj *o;
    long long start, end;
    char *str, llbuf[32];
    size_t strlen;

    /* 检查第二个和第三个参数必须为整数 */
    if (getLongLongFromObjectOrReply(c,c->argv[2],&start,NULL) != C_OK)
        return;
    if (getLongLongFromObjectOrReply(c,c->argv[3],&end,NULL) != C_OK)
        return;
    /* 查找key是否存在，且value类型必须为string */
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.emptybulk)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;

    /* 如果对象编码为INT，转为STRING处理 */
    if (o->encoding == OBJ_ENCODING_INT) {
        str = llbuf;
        strlen = ll2string(llbuf,sizeof(llbuf),(long)o->ptr);
    } else {
        str = o->ptr;
        strlen = sdslen(str);
    }

    /* 处理负数的范围值 */
    if (start < 0 && end < 0 && start > end) {
        addReply(c,shared.emptybulk);
        return;
    }
    if (start < 0) start = strlen+start;
    if (end < 0) end = strlen+end;
    if (start < 0) start = 0;
    if (end < 0) end = 0;
    if ((unsigned long long)end >= strlen) end = strlen-1;

    /* Precondition: end >= 0 && end < strlen, so the only condition where
     * nothing can be returned is: start > end. */
    /*
     * 命令生效的前提是end >= 0 且 end < strlen
     * 如果start > end 或者 strlen == 0 返回空
     */
    if (start > end || strlen == 0) {
        addReply(c,shared.emptybulk);
    } else {
        addReplyBulkCBuffer(c,(char*)str+start,end-start+1);
    }
}

/*
 * mget 命令实现
 */
void mgetCommand(client *c) {
    int j;

    addReplyMultiBulkLen(c,c->argc-1);
    // for 循环，逐个获取
    for (j = 1; j < c->argc; j++) {
        robj *o = lookupKeyRead(c->db,c->argv[j]);
        if (o == NULL) {
            addReply(c,shared.nullbulk);
        } else {
            if (o->type != OBJ_STRING) {
                addReply(c,shared.nullbulk);
            } else {
                addReplyBulk(c,o);
            }
        }
    }
}

/*
 * mset通用实现
 */
void msetGenericCommand(client *c, int nx) {
    int j, busykeys = 0;

    // 参数必须是成对出现的 key1 value1 key2 value2...
    if ((c->argc % 2) == 0) {
        addReplyError(c,"wrong number of arguments for MSET");
        return;
    }
    /* Handle the NX flag. The MSETNX semantic is to return zero and don't
     * set nothing at all if at least one already key exists. */
    /*
     * 处理nx标志
     * msetnx的实现，如果有一个key已经存在，就会直接返回
     */
    if (nx) {
	// 检查是否有key已存在
        for (j = 1; j < c->argc; j += 2) {
            if (lookupKeyWrite(c->db,c->argv[j]) != NULL) {
                busykeys++;
            }
        }
        if (busykeys) {
            addReply(c, shared.czero);
            return;
        }
    }

    // 逐个设置键值
    for (j = 1; j < c->argc; j += 2) {
        c->argv[j+1] = tryObjectEncoding(c->argv[j+1]);
        setKey(c->db,c->argv[j],c->argv[j+1]);
        notifyKeyspaceEvent(NOTIFY_STRING,"set",c->argv[j],c->db->id);
    }
    server.dirty += (c->argc-1)/2;
    addReply(c, nx ? shared.cone : shared.ok);
}

/*
 * mset命令实现
 */
void msetCommand(client *c) {
    msetGenericCommand(c,0);
}

/*
 * msetnx命令实现
 */
void msetnxCommand(client *c) {
    msetGenericCommand(c,1);
}

/*
 * incr、decr具体的实现
 */
void incrDecrCommand(client *c, long long incr) {
    long long value, oldvalue;
    robj *o, *new;

    // 检查key和value的类型
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;
    if (getLongLongFromObjectOrReply(c,o,&value,NULL) != C_OK) return;

    // 处理执行后溢出的情况
    oldvalue = value;
    if ((incr < 0 && oldvalue < 0 && incr < (LLONG_MIN-oldvalue)) ||
        (incr > 0 && oldvalue > 0 && incr > (LLONG_MAX-oldvalue))) {
        addReplyError(c,"increment or decrement would overflow");
        return;
    }
    value += incr;

    /* 
     * 在long范围内，直接赋值，否则使用longlong创建字符串后再赋值
     */
    if (o && o->refcount == 1 && o->encoding == OBJ_ENCODING_INT &&
        (value < 0 || value >= OBJ_SHARED_INTEGERS) &&
        value >= LONG_MIN && value <= LONG_MAX)
    {
        new = o;
        o->ptr = (void*)((long)value);
    } else {
        new = createStringObjectFromLongLong(value);
        if (o) {
            dbOverwrite(c->db,c->argv[1],new);
        } else {
            dbAdd(c->db,c->argv[1],new);
        }
    }
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrby",c->argv[1],c->db->id);
    server.dirty++;
    addReply(c,shared.colon);
    addReply(c,new);
    addReply(c,shared.crlf);
}

/*
 * incr命令实现
 */
void incrCommand(client *c) {
    incrDecrCommand(c,1);
}

/*
 * decr命令实现
 */
void decrCommand(client *c) {
    incrDecrCommand(c,-1);
}

/*
 * incrby命令实现
 */
void incrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,incr);
}

/*
 * decrby命令实现
 */
void decrbyCommand(client *c) {
    long long incr;

    if (getLongLongFromObjectOrReply(c, c->argv[2], &incr, NULL) != C_OK) return;
    incrDecrCommand(c,-incr);
}

/*
 * incrbyfloat命令实现
 */
void incrbyfloatCommand(client *c) {
    long double incr, value;
    robj *o, *new, *aux;

    // 查找key、获取值
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o != NULL && checkType(c,o,OBJ_STRING)) return;
    if (getLongDoubleFromObjectOrReply(c,o,&value,NULL) != C_OK ||
        getLongDoubleFromObjectOrReply(c,c->argv[2],&incr,NULL) != C_OK)
        return;

    // 非法value处理
    value += incr;
    if (isnan(value) || isinf(value)) {
        addReplyError(c,"increment would produce NaN or Infinity");
        return;
    }
    // 创建字符串对象，写进数据库
    new = createStringObjectFromLongDouble(value,1);
    if (o)
        dbOverwrite(c->db,c->argv[1],new);
    else
        dbAdd(c->db,c->argv[1],new);
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"incrbyfloat",c->argv[1],c->db->id);
    server.dirty++;
    addReplyBulk(c,new);

    /* Always replicate INCRBYFLOAT as a SET command with the final value
     * in order to make sure that differences in float precision or formatting
     * will not create differences in replicas or after an AOF restart. */
    /*
     * 使用最后的值重新执行一次set命令，保证不会出现不同精度的值
     */
    aux = createStringObject("SET",3);
    rewriteClientCommandArgument(c,0,aux);
    decrRefCount(aux);
    rewriteClientCommandArgument(c,2,new);
}

/*
 * append命令实现
 */
void appendCommand(client *c) {
    size_t totlen;
    robj *o, *append;

    // 查找key
    o = lookupKeyWrite(c->db,c->argv[1]);
    if (o == NULL) {
        /* 创建key value，写进数据库 */
        c->argv[2] = tryObjectEncoding(c->argv[2]);
        dbAdd(c->db,c->argv[1],c->argv[2]);
        incrRefCount(c->argv[2]);
        totlen = stringObjectLen(c->argv[2]);
    } else {
        /* 类型必须为string */
        if (checkType(c,o,OBJ_STRING))
            return;

        /* append 就是要追加的字符串 */
        append = c->argv[2];
        totlen = stringObjectLen(o)+sdslen(append->ptr);
        if (checkStringLength(c,totlen) != C_OK)
            return;

        /* 执行追加操作 */
        o = dbUnshareStringValue(c->db,c->argv[1],o);
        o->ptr = sdscatlen(o->ptr,append->ptr,sdslen(append->ptr));
        totlen = sdslen(o->ptr);
    }
    signalModifiedKey(c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_STRING,"append",c->argv[1],c->db->id);
    server.dirty++;
    // 执行成功返回append后的长度
    addReplyLongLong(c,totlen);
}

/*
 * strlen命令实现
 */
void strlenCommand(client *c) {
    robj *o;
    // 查找key并判断类型是否合法，接着调用stringObjectLen直接读取对象的len属性
    if ((o = lookupKeyReadOrReply(c,c->argv[1],shared.czero)) == NULL ||
        checkType(c,o,OBJ_STRING)) return;
    addReplyLongLong(c,stringObjectLen(o));
}
