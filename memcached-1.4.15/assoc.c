/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Hash table
 *
 * The hash function used here is by Bob Jenkins, 1996:
 *    <http://burtleburtle.net/bob/hash/doobs.html>
 *       "By Bob Jenkins, 1996.  bob_jenkins@burtleburtle.net.
 *       You may use this code any way you wish, private, educational,
 *       or commercial.  It's free."
 *
 * The rest of the file is licensed under the BSD license.  See LICENSE.
 */

#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

static pthread_cond_t maintenance_cond = PTHREAD_COND_INITIALIZER;


typedef  unsigned long  int  ub4;   /* unsigned 4-byte quantities */
typedef  unsigned       char ub1;   /* unsigned 1-byte quantities */

/* how many powers of 2's worth of buckets we use */
unsigned int hashpower = HASHPOWER_DEFAULT;
//hashsize(n)为2的幂
#define hashsize(n) ((ub4)1<<(n))
//哈希掩码,hashmask的值的二进制形式就是后面全为1的数
#define hashmask(n) (hashsize(n)-1)

/* Main hash table. This is where we look except during expansion. */
//哈希表指针
static item** primary_hashtable = 0;

/*
 * Previous hash table. During expansion, we look here for keys that haven't
 * been moved over to the primary yet.
 */
static item** old_hashtable = 0;

/* Number of items in the hash table. */
static unsigned int hash_items = 0;

/* Flag: Are we in the middle of expanding now? */
static bool expanding = false;
static bool started_expanding = false;

/*
 * During expansion we migrate values with bucket granularity; this is how
 * far we've gotten so far. Ranges from 0 .. hashsize(hashpower - 1) - 1.
 */
static unsigned int expand_bucket = 0;
//本函数由main函数调用
void assoc_init(const int hashtable_init) {
    if (hashtable_init) {
        hashpower = hashtable_init;
    }
	//分配空间,hashsize就是哈希表长度
    primary_hashtable = calloc(hashsize(hashpower), sizeof(void *));
    if (! primary_hashtable) {
        fprintf(stderr, "Failed to init hashtable.\n");
        exit(EXIT_FAILURE);
    }
    STATS_LOCK();
    stats.hash_power_level = hashpower;
    stats.hash_bytes = hashsize(hashpower) * sizeof(void *);
    STATS_UNLOCK();
}
//由于哈希值只能确定是在哈希表中的哪个桶(bucket)，但一个桶里面是有一条冲突链的  
//此时需要用到具体的键值遍历并一一比较冲突链上的所有节点。虽然key是以'\0'结尾 
//的字符串，但调用strlen还是有点耗时(需要遍历键值字符串)。所以需要另外一个参数
//nkey指明这个key的长度
//reference:http://blog.csdn.net/luotuo44/article/details/42773231
item *assoc_find(const char *key, const size_t nkey, const uint32_t hv) {
    item *it;
    unsigned int oldbucket;

    // 得到相应的桶, bucket
    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        it = old_hashtable[oldbucket];
    } else {
		//由哈希值判断这个key是属于那个桶(bucket)的
        it = primary_hashtable[hv & hashmask(hashpower)];
    }

    // 在桶里遍历搜索目标
    item *ret = NULL;
    int depth = 0;
    while (it) {
		//调用memcmp来进行比较
        if ((nkey == it->nkey) && (memcmp(key, ITEM_key(it), nkey) == 0)) {
            ret = it;
            break;
        }
        it = it->h_next;
        ++depth;
    }
    MEMCACHED_ASSOC_FIND(key, nkey, depth);
    return ret;
}

/* returns the address of the item pointer before the key.  if *item == 0,
   the item wasn't found */
//查找item,返回前驱节点的h_next成员地址
static item** _hashitem_before (const char *key, const size_t nkey, const uint32_t hv) {
    item **pos;
    unsigned int oldbucket;

    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
        pos = &old_hashtable[oldbucket];
    } else {
        pos = &primary_hashtable[hv & hashmask(hashpower)];
    }
	//遍历桶的冲突链查找item 
    while (*pos && ((nkey != (*pos)->nkey) || memcmp(key, ITEM_key(*pos), nkey))) {
        pos = &(*pos)->h_next;
    }
    return pos;
}

/* grows the hashtable to the next power of 2. */
static void assoc_expand(void) {
    old_hashtable = primary_hashtable;
	//申请一个新的hash表，此时primary_hashtable是新表，old_hashtable是旧表
    primary_hashtable = calloc(hashsize(hashpower + 1), sizeof(void *));
    if (primary_hashtable) {
        if (settings.verbose > 1)
            fprintf(stderr, "Hash table expansion starting\n");
        hashpower++;
        expanding = true;
        expand_bucket = 0;
        STATS_LOCK();
        stats.hash_power_level = hashpower;
        stats.hash_bytes += hashsize(hashpower) * sizeof(void *);
        stats.hash_is_expanding = 1;
        STATS_UNLOCK();
    } else {
        primary_hashtable = old_hashtable;
        /* Bad news, but we can keep running. */
    }
}
//assoc_insert函数会调用本函数，当item数量到了哈希表长度的1.5倍时，在该函数中会唤醒扩容线程
static void assoc_start_expand(void) {
    if (started_expanding)
        return;
    started_expanding = true;
	//phtread_cond_signal来唤醒等待该maintanance_cond条件变量的线程，即扩容线程
    pthread_cond_signal(&maintenance_cond);
}

/* Note: this isn't an assoc_update.  The key must not already exist to call this */
//hash,插入元素,hv是这个item键值的哈希值
int assoc_insert(item *it, const uint32_t hv) {
    unsigned int oldbucket;

//    assert(assoc_find(ITEM_key(it), it->nkey) == 0);  /* shouldn't have duplicately named things defined */

    // 头插法
    if (expanding &&
        (oldbucket = (hv & hashmask(hashpower - 1))) >= expand_bucket)
    {
		//目前处于扩容状态，但这个桶的数据还没有迁移，因此插入到旧表old_hashtable
        it->h_next = old_hashtable[oldbucket];
        old_hashtable[oldbucket] = it;
    } else {
		//插入到新表，primary_hashtable
        it->h_next = primary_hashtable[hv & hashmask(hashpower)];
        primary_hashtable[hv & hashmask(hashpower)] = it;
    }
	//元素数目+1
    hash_items++;

    // 适时扩张，当元素个数超过hash容量的1.5倍时
    if (! expanding && hash_items > (hashsize(hashpower) * 3) / 2) {
        //调用assoc_start_expand函数来唤醒扩容线程
		assoc_start_expand();
    }

    MEMCACHED_ASSOC_INSERT(ITEM_key(it), it->nkey, hash_items);
    return 1;
}
//从链表中删除一个节点的常规做法是：先找到这个节点的前驱节点，然后使用前驱节点的next指针进行删除和拼接操作。
void assoc_delete(const char *key, const size_t nkey, const uint32_t hv) {
    // 寻找到需要删除节点的前一个节点, 这是链表删除的经典操作
    item **before = _hashitem_before(key, nkey, hv);
	//查找成功
    if (*before) {
        item *nxt;
        hash_items--;
        /* The DTrace probe cannot be triggered as the last instruction
         * due to possible tail-optimization by the compiler
         */
        MEMCACHED_ASSOC_DELETE(key, nkey, hash_items);
        nxt = (*before)->h_next;
        (*before)->h_next = 0;   /* probably pointless, but whatever. */
        *before = nxt;
        return;
    }
    /* Note:  we never actually get here.  the callers don't delete things
       they can't find. */
    assert(*before != 0);
}

//全局flag,用来判断是否终止扩容线程
static volatile int do_run_maintenance_thread = 1;

//该变量用来定义部分数据迁移时的桶大小，以避免数据迁移时过久持有锁，在assoc_maintenance_thread中被使用
//具体用法查看：http://blog.csdn.net/luotuo44/article/details/42773231
#define DEFAULT_HASH_BULK_MOVE 1
int hash_bulk_move = DEFAULT_HASH_BULK_MOVE;


//启动扩容线程，扩容线程在main函数中会启动，启动运行一遍之后会阻塞在条件变量maintenance_cond上面,
//当插入元素超过规定，唤醒条件变量，再次运行
static void *assoc_maintenance_thread(void *arg) {
	//do_run_maintenance_thread是全局变量，初始值为1
	//在stop_assoc_maintenance_thread函数中会被赋值0，用来终止迁移线程 
    while (do_run_maintenance_thread) {
        int ii = 0;

        /* Lock the cache, and bulk move multiple buckets to the new
         * hash table. */
        item_lock_global();
        mutex_lock(&cache_lock);
		
		//hash_bulk_move用来控制每次迁移，移动多少个桶的item。默认是一个.  
        //如果expanding为true才会进入循环体，所以迁移线程刚创建的时候，并不会进入循环体
        for (ii = 0; ii < hash_bulk_move && expanding; ++ii) {
            item *it, *next;
            int bucket;
			//遍历旧哈希表中由expand_bucket指明的桶,将该桶的所有item迁移到新扩容的哈希表中
            for (it = old_hashtable[expand_bucket]; NULL != it; it = next) {
                next = it->h_next;

                bucket = hash(ITEM_key(it), it->nkey, 0) & hashmask(hashpower);
                it->h_next = primary_hashtable[bucket];
                primary_hashtable[bucket] = it;
            }

            old_hashtable[expand_bucket] = NULL;
			//迁移完一个桶，接着把expand_bucket指向下一个待迁移的桶
            expand_bucket++;
			//数据迁移完毕
            if (expand_bucket == hashsize(hashpower - 1)) {
                expanding = false;
                free(old_hashtable);
                STATS_LOCK();
                stats.hash_bytes -= hashsize(hashpower - 1) * sizeof(void *);
                stats.hash_is_expanding = 0;
                STATS_UNLOCK();
                if (settings.verbose > 1)
                    fprintf(stderr, "Hash table expansion done\n");
            }
        }
		//释放锁
        mutex_unlock(&cache_lock);
        item_unlock_global();
		//不再迁移数据
        if (!expanding) {
            /* finished expanding. tell all threads to use fine-grained locks */
            switch_item_lock_type(ITEM_LOCK_GRANULAR);
            slabs_rebalancer_resume();
            /* We are done expanding.. just wait for next invocation */
            mutex_lock(&cache_lock);
            started_expanding = false;
			//挂起迁移线程，直到worker线程插入数据后发现item数量已经到了1.5倍哈希表大小，  
            //此时调用worker线程调用assoc_start_expand函数，该函数会调用pthread_cond_signal  
            //唤醒迁移线程
            pthread_cond_wait(&maintenance_cond, &cache_lock);
            /* Before doing anything, tell threads to use a global lock */
            mutex_unlock(&cache_lock);
            slabs_rebalancer_pause();
            switch_item_lock_type(ITEM_LOCK_GLOBAL);
            mutex_lock(&cache_lock);
            assoc_expand();//???
            mutex_unlock(&cache_lock);
        }
    }
    return NULL;
}

static pthread_t maintenance_tid;

//main函数会调用本函数，启动数据迁移线程
int start_assoc_maintenance_thread() {
    int ret;
    char *env = getenv("MEMCACHED_HASH_BULK_MOVE");
    if (env != NULL) {
        hash_bulk_move = atoi(env);
        if (hash_bulk_move == 0) {
            hash_bulk_move = DEFAULT_HASH_BULK_MOVE;
        }
    }
    if ((ret = pthread_create(&maintenance_tid, NULL,
                              assoc_maintenance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

void stop_assoc_maintenance_thread() {
    mutex_lock(&cache_lock);
    do_run_maintenance_thread = 0;
    pthread_cond_signal(&maintenance_cond);
    mutex_unlock(&cache_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(maintenance_tid, NULL);
}


