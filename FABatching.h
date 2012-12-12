#import <Foundation/Foundation.h>
#import <objc/runtime.h>
#import <objc/message.h>
#import <libkern/OSAtomic.h>

// the size needed for the batch, with proper FABatchAlignment for objects
#define FABatchAlignment    8
#define FAObjectsPerBatch   64
#define FABatchSize         ((sizeof(FABatch) + (FABatchAlignment - 1)) & ~(FABatchAlignment - 1))
#define FAPoolSize          128

typedef struct
{
    long    _instance_size;
    int32_t _freed;
    int32_t _allocated;
    int32_t _reserved;
} FABatch;

typedef struct
{
    NSUInteger poolSize;
    uintptr_t low, high;
    FABatch   *currentBatch;
    FABatch   **batches;
    OSSpinLock spinLock;
} FABatchPool;

static inline FABatch *FANewObjectBatch(FABatchPool *pool, long batchInstanceSize)
{
    NSUInteger     len;
    unsigned long  size;
    FABatch        *batch;

    // Empty/Full pool => allocate new batch
    if(pool->low == pool->high || ((pool->high + 1) % pool->poolSize) == pool->low) {
        batchInstanceSize = (batchInstanceSize + (FABatchAlignment - 1)) & ~(FABatchAlignment - 1);
        size = batchInstanceSize + sizeof(int);

        len = size * FAObjectsPerBatch + FABatchSize;
        if(!(batch = (FABatch *)calloc(1, len))){
            NSLog(@"Failed to allocate object. Out of memory?");
            return nil;
        }
        batch->_instance_size = batchInstanceSize;
    } else {
        // Otherwise we recycle an existing batch
        batch = pool->batches[pool->low];
        pool->low = (pool->low + 1) % pool->poolSize;
    }
    return batch;
}

static inline void FARecycleObjectBatch(FABatchPool *pool, FABatch *batch)
{
    unsigned long next = (pool->high + 1) % pool->poolSize;
    if(next == pool->low) // Full?
        free(batch);
    else {
        batch->_freed = 0;
        batch->_allocated = 0;
        pool->batches[pool->high] = batch;
        pool->high = next;
        __sync_val_compare_and_swap(&pool->currentBatch, batch, pool->batches[next]);
    }
}

static inline BOOL FASizeFitsObjectBatch(FABatch *p, long size)
{
    // We can't deal with subclasses larger than what we first allocated
    return p && size <= p->_instance_size;
}

static inline BOOL FABatchIsExhausted(FABatch *p)
{
    return p->_allocated == FAObjectsPerBatch;
}

#define FA_BATCH_IVARS                                                                         \
    /* The batch the object is in */                                                           \
    FABatch *_batch;                                                                           \
    /* It's minus one so we don't have to initialize it to 1 */                                \
    NSInteger _retainCountMinusOne;

#define FA_BATCH_IMPL(Klass)                                                                   \
static FABatchPool _BatchPool;                                                                 \
                                                                                               \
static inline Klass *FABatchAlloc##Klass(Class self)                                           \
{                                                                                              \
    size_t instanceSize = class_getInstanceSize(self);                                         \
    OSSpinLockLock(&_BatchPool.spinLock);                                                      \
    if(__builtin_expect(!_BatchPool.batches, 0)) {                                             \
        _BatchPool.poolSize = FAPoolSize;                                                      \
        _BatchPool.batches  = (FABatch **)malloc(sizeof(void*) * _BatchPool.poolSize);         \
        _BatchPool.currentBatch = FANewObjectBatch(&_BatchPool, instanceSize);                 \
    }                                                                                          \
                                                                                               \
    Klass *obj = nil;                                                                          \
    FABatch *batch = _BatchPool.currentBatch;                                                  \
    if(__builtin_expect(FASizeFitsObjectBatch(_BatchPool.currentBatch, instanceSize), 1))      \
    {                                                                                          \
        /* Grab an object from the current batch */                                            \
        /* and place isa pointer there */                                                      \
        NSUInteger offset;                                                                     \
        offset      = FABatchSize + batch->_instance_size * batch->_allocated;                 \
        obj         = (id)((char *)batch + offset);                                            \
        obj->_batch = batch;                                                                   \
        obj->_retainCountMinusOne = 0;                                                         \
                                                                                               \
        batch->_allocated++;                                                                   \
        *(Class *)obj = self;                                                                  \
    } else {                                                                                   \
        NSCAssert(NO, @"Unable to get %@ from batch", self);                                   \
    }                                                                                          \
                                                                                               \
    /* Batch full? => Make a new one for next time */                                          \
    if(FABatchIsExhausted(batch) && _BatchPool.currentBatch == batch)                          \
        _BatchPool.currentBatch = FANewObjectBatch(&_BatchPool, instanceSize);                 \
                                                                                               \
    OSSpinLockUnlock(&_BatchPool.spinLock);                                                    \
    return obj;                                                                                \
}                                                                                              \
                                                                                               \
+ (id)allocWithZone:(NSZone *)zone { return FABatchAlloc##Klass(self); }                       \
+ (id)alloc                        { return FABatchAlloc##Klass(self); }                       \
                                                                                               \
- (id)retain                                                                                   \
{                                                                                              \
    __sync_add_and_fetch(&_retainCountMinusOne, 1);                                            \
    return self;                                                                               \
}                                                                                              \
- (oneway void)release                                                                         \
{                                                                                              \
    if(__builtin_expect(__sync_sub_and_fetch(&_retainCountMinusOne, 1) < 0, 0))                \
        [self dealloc];                                                                        \
}

#define FA_BATCH_DEALLOC                                                                       \
    /* Recycle the entire batch if all the objects in it are unreferenced */                   \
    if(__sync_add_and_fetch(&_batch->_freed, 1) == FAObjectsPerBatch) {                        \
        OSSpinLockLock(&_BatchPool.spinLock);                                                  \
        FARecycleObjectBatch(&_BatchPool, _batch);                                             \
        OSSpinLockUnlock(&_BatchPool.spinLock);                                                \
    }                                                                                          \
    return;                                                                                    \
    __builtin_unreachable();                                                                   \
    [super dealloc]; /* Silence compiler warning about not calling super dealloc */
