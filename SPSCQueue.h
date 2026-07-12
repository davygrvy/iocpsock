#ifndef SPSC_QUEUE_H
#define SPSC_QUEUE_H

#include <windows.h>
#include <stdbool.h>

/* Force a power-of-two capacity (e.g., 256, 1024, 4096) */
#define QUEUE_CAPACITY 1024
#define QUEUE_MASK     (QUEUE_CAPACITY - 1)

/* Define payload data type */
typedef void* QueueItem; 

/* 
 * Macro abstracts for Compiler Alignment & Fences 
 */
#if defined(__GNUC__) || defined(__clang__)
    #define ALIGN_CACHELINE __attribute__((aligned(64)))
    #define ATOMIC_FENCE_ACQUIRE() __atomic_thread_fence(__ATOMIC_ACQUIRE)
    #define ATOMIC_FENCE_RELEASE() __atomic_thread_fence(__ATOMIC_RELEASE)
#elif defined(_MSC_VER)
    #define ALIGN_CACHELINE __declspec(align(64))
    #include <intrin.h>
    #define ATOMIC_FENCE_ACQUIRE() _ReadBarrier()
    #define ATOMIC_FENCE_RELEASE() _WriteBarrier()
#else
    #error "Unsupported compiler. Please provide fence primitives."
#endif

typedef struct {
    QueueItem buffer[QUEUE_CAPACITY];

    /* Align variables to 64 bytes to eliminate cross-thread false sharing */
    ALIGN_CACHELINE volatile ULONG_PTR head;
    ALIGN_CACHELINE volatile ULONG_PTR tail;
} SPSCQueue;

/* Initialize the queue structure */
static inline void SPSCQueue_Init(SPSCQueue* q) {
    q->head = 0;
    q->tail = 0;
    SecureZeroMemory(q->buffer, sizeof(q->buffer));
}

/* 
 * Push an item into the queue (Producer thread only) 
 * Returns true on success, false if full.
 */
static inline bool SPSCQueue_Push(SPSCQueue* q, QueueItem item) {
    ULONG_PTR current_head = q->head; 
    ATOMIC_FENCE_ACQUIRE(); /* Ensure head read finishes before tail evaluation */

    ULONG_PTR current_tail = q->tail;

    if ((current_tail - current_head) == QUEUE_CAPACITY) {
        return false; /* Queue is full */
    }

    q->buffer[current_tail & QUEUE_MASK] = item;

    /* Ensure data write completes before releasing index to consumer */
    ATOMIC_FENCE_RELEASE();
#if defined(_M_ARM) || defined(_M_ARM64) || defined(__arm__) || defined(__aarch64__)
    MemoryBarrier(); /* Hardware barrier fallback for weak-ordered ARM targets */
#endif

    q->tail = current_tail + 1;
    return true;
}

/* 
 * Pop an item from the queue (Consumer thread only) 
 * Returns true on success, false if empty.
 */
static inline bool SPSCQueue_Pop(SPSCQueue* q, QueueItem* out_value) {
    ULONG_PTR current_head = q->head;
    
#if defined(_M_ARM) || defined(_M_ARM64) || defined(__arm__) || defined(__aarch64__)
    MemoryBarrier(); 
#endif
    ULONG_PTR current_tail = q->tail;
    ATOMIC_FENCE_ACQUIRE(); /* Ensure tail visibility is pulled before matching */

    if (current_head == current_tail) {
        return false; /* Queue is empty */
    }

    *out_value = q->buffer[current_head & QUEUE_MASK];

    /* Ensure copy-out finishes before releasing slot back to producer */
    ATOMIC_FENCE_RELEASE();
    q->head = current_head + 1;
    return true;
}

/* Returns true if the queue is empty */
static inline bool SPSCQueue_IsEmpty(const SPSCQueue* q) {
    return q->head == q->tail;
}

/* Returns current item count */
static inline ULONG_PTR SPSCQueue_Size(const SPSCQueue* q) {
    ULONG_PTR current_head = q->head;
    ULONG_PTR current_tail = q->tail;
    
    if (current_head >= current_tail) {
        return 0;
    }
    return current_tail - current_head;
}

#endif /* SPSC_QUEUE_H */
