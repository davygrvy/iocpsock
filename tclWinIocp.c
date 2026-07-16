/* ----------------------------------------------------------------------
 *
 * tclWinIocp.c --
 *
 *	Shared routines for managing and running a completion port for
 *	overlapped I/O on Windows common to the native channel drivers
 *	both included in the core and available to extensions.
 *
 *	This file includes the one single global thread to service the
 *	completeion port for the internal (and external) native channel
 *	drivers, the memory allocator for the special non-paged pool,
 *	and the optimized lock-free single-producer single-consumer
 *	ring buffer FIFO queue routines.
 *
 * ----------------------------------------------------------------------
 * RCS: @(#) $Id: $
 * ----------------------------------------------------------------------
 */

#include "iocpsockInt.h"

#include <stdbool.h>
#include <stdatomic.h>


struct iocpheader {

};

/* to be shared public protos */
extern DWORD		Tcl_WinIocpAssocHandle(HANDLE hndl, iocpheader* ocp);
extern DWORD		Tcl_WinIocpPostToCP(iocpheader* ocp);

extern __inline LPVOID	Tcl_WinIocpNPPAlloc(SIZE_T size);
extern __inline LPVOID	Tcl_WinIocpNPPReAlloc(LPVOID block, SIZE_T size);
extern __inline BOOL	Tcl_WinIocpNPPFree(LPVOID block);

/* forward declare */
typedef SPSCQueue;

extern void		Tcl_WinIocpQCreate(SPSCQueue* q, SIZE_T capacity);
extern void		Tcl_WinIocpQDestroy(SPSCQueue* q);
extern bool		Tcl_WinIocpQPoPFront(SPSCQueue* q, void** out_value);
extern bool		Tcl_WinIocpQPushBack(SPSCQueue* q, void* item);
//extern void		Tcl_WinIocpQPopAllCompare(PSLIST_HEADER pListHead, LPVOID pItem);

/* shared private protos */
extern DWORD InitializeIocpSubSystem();

/* file-scope protos */
static DWORD WINAPI CompletionThreadProc(LPVOID lpParam);
Tcl_ExitProc IocpExitHandler;

/* file-scope globals */
static LONG initialized = 0;
static HANDLE cport;
static HANDLE cpthread;
static HANDLE NPPheap;
#if _DEBUG
static LONG StatSpecialBytesInUse = 0;
#endif

DWORD
InitializeIocpSubSystem()
{
    DWORD error = NO_ERROR;
    SYSTEM_INFO si;
    SIZE_T HeapInitialBytes, HeapLimitBytes;
    ULONGLONG TotalMemoryInKb, TotalMemoryBytes;

    /* global/once init */
    if (InterlockedExchange(&initialized, 1) == 0) {

	/* Create the completion port. */
	cport = CreateIoCompletionPort(
		INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)NULL, 0);
	if (cport == NULL) {
	    error = GetLastError();
	    goto error;
	}

	/*
	 * NPPheap initialization for IOCP.
	 *
	 * Why CreateHeap is used:
	 * We use a dedicated private heap for overlapped buffers to
	 * isolate them from the default process heap. This significantly
	 * reduces heap fragmentation in high-throughput environments and
	 * allows for safe, concurrent HeapAlloc calls across multiple
	 * I/O worker threads.
	 * 
	 * Why this heap is special:
	 * It manages the memory blocks that will be repeatedly passed to
	 * WSARecv/WSASend.  While standard user-mode memory is pageable,
	 * the kernel will temporarily lock these specific memory pages
	 * into physical RAM during I/O operations. Keeping the heap
	 * clean and allocated sequentially helps prevent I/O page limit
	 * exhaustion errors.
	 * 
	 * We only allow the heap to grow to a limit of 1/4 the physical
	 * RAM.  I read once this is the practical limit of the
	 * non-paged pool.  Yes, we absolutely expect someone will push
	 * Tcl as a server to 25,000+ open sockets.
	 */

	GetNativeSystemInfo(&si);

	/* about 256k on x86, could be larger on ARM */
#if defined(_M_ARM) || defined(_M_ARM64) || defined(__arm__) || defined(__aarch64__)
	HeapInitialBytes = si.dwPageSize * 64;	    /* TODO */
#else
	HeapInitialBytes = si.dwPageSize * 64;
#endif

	GetPhysicallyInstalledSystemMemory(&TotalMemoryInKb);

	/* Total physical RAM in bytes */
	TotalMemoryBytes = TotalMemoryInKb * 1024;

	/*  1/4 of total physical RAM */
	HeapLimitBytes = (SIZE_T)(TotalMemoryBytes / 4);

	NPPheap = HeapCreate(0, HeapInitialBytes, HeapLimitBytes);
	if (NPPheap == NULL) {
	    error = GetLastError();
	    CloseHandle(cport);
	    goto error;
	}

	cpthread = CreateThread(NULL, 0, CompletionThreadProc, NULL,
		CREATE_SUSPENDED, NULL);
	if (cpthread == NULL) {
	    error = GetLastError();
	    HeapDestroy(NPPheap);
	    CloseHandle(cport);
	    goto error;
	}

	/*
	 * Do not elevate priority.
	 *
	 * TODO: We could try THREAD_PRIORITY_BELOW_NORMAL for a more
	 * responsive Tk UI when under heavy I/O load.
	 */
	SetThreadPriority(cpthread, THREAD_PRIORITY_NORMAL);
	ResumeThread(cpthread);

	Tcl_CreateExitHandler(IocpExitHandler, NULL);
    }
	
    return NO_ERROR;

error:
    InterlockedExchange(&initialized, 0);
    return error;
}

void
IocpExitHandler(ClientData clientData)
{
    DWORD wait;

    if (InterlockedExchange(&initialized, 0) == 1) {
	/* Cause the completion thread to exit. */
	PostQueuedCompletionStatus(cport, 0, 0, 0);

	/* Wait for our completion thread to exit. */
	wait = WaitForSingleObject(cpthread, 400);
	if (wait == WAIT_TIMEOUT) {
	    TerminateThread(cpthread, 0x666);
	}
	CloseHandle(cpthread);

	/* Close the completion port object. */
	CloseHandle(cport);

	/* Tear down the private memory heap. */
	HeapDestroy(NPPheap);
    }
}

/*
 * ----------------------------------------------------------------------
 * CompletionThreadProc --
 *
 *	The "main" of the I/O handling thread.  Only one thread is used
 *	in the Tcl process.  If more threads are used, we run the risk
 *	of creating out-of-order data in the streams.  It becomes a
 *	one-to-many-to-one issue.
 *
 * Results:
 * 
 *	None.  Returns when the completion port is sent a completion
 *	notification with a NULL key by the exit handler.
 * 
 * Side effects:
 * 
 *	Without direct interaction from Tcl, incoming I/O will be moved
 *	to their FIFO queues and replaced for the next operation.  Tcl will
 *	service them when the event loop is ready to.
 * 
 * ----------------------------------------------------------------------
 */

DWORD WINAPI
CompletionThreadProc(LPVOID lpParam)
{
    TclWinIocpInfo* infoPtr;
    TclWinIocpBufferInfo* buffPtr;
    OVERLAPPED* ol;
    DWORD bytes, opErr;
    BOOL ok;

    again:
    opErr = NO_ERROR;

    ok = GetQueuedCompletionStatus(cport, &bytes,
	    (PULONG_PTR)&infoPtr, &ol, INFINITE);

    if (ok && !infoPtr) {
	/* A NULL key indicates closure time for this thread. */
	return NO_ERROR;
    }

    if (!ok) {
	opErr = GetLastError();
    }

    /*
     * Use the pointer to the overlapped structure and derive from it
     * the top of the parent BufferInfo structure it sits in.
     */

    buffPtr = CONTAINING_RECORD(ol, BufferInfo, ol);

    /* Go handle the IO operation. */
    infoPtr->serviceProc(infoPtr, buffPtr, cport, bytes, opErr);
    goto again;
}

DWORD
Tcl_WinIocpAssocHandle(HANDLE hndl, iocpheader *ocp)
{
    CreateIoCompletionPort(hndl, cport, (ULONG_PTR)ocp, 0);
    return NO_ERROR;
}

DWORD
Tcl_WinIocpPostToCP(iocpheader* ocp)
{
    return PostQueuedCompletionStatus(cport, 0, (ULONG_PTR)ocp, );
}



/* special pool */

__inline LPVOID
Tcl_WinIocpNPPAlloc(SIZE_T size)
{
    LPVOID p;
    p = HeapAlloc(NPPheap, HEAP_ZERO_MEMORY, size);
#if _DEBUG
    if (p) InterlockedExchangeAdd(&StatSpecialBytesInUse, size);
#endif
    return p;
}

__inline LPVOID
Tcl_WinIocpNPPReAlloc(LPVOID block, SIZE_T size)
{
    LPVOID p;
#if _DEBUG
    SIZE_T oldSize;
    oldSize = HeapSize(NPPheap, 0, block);
#endif
    p = HeapReAlloc(NPPheap, HEAP_ZERO_MEMORY, block, size);
#if _DEBUG
    if (p) InterlockedExchangeAdd(&StatSpecialBytesInUse, ((LONG)size - oldSize));
#endif
    return p;
}

__inline BOOL
Tcl_WinIocpNPPFree(LPVOID block)
{
    BOOL code;
#if _DEBUG
    SIZE_T oldSize;
    oldSize = HeapSize(NPPheap, 0, block);
#endif
    code = HeapFree(NPPheap, 0, block);
#if _DEBUG
    if (code) InterlockedExchangeAdd(&StatSpecialBytesInUse, -((LONG)oldSize));
#endif
    return code;
}

/* lock-free queue */

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
    /* Align variables to 64 bytes to eliminate cross-thread false sharing */
    ALIGN_CACHELINE volatile ULONG_PTR head;
    ALIGN_CACHELINE volatile ULONG_PTR tail;
    SIZE_T mask;
    void** buffer;
} SPSCQueue;


void
Tcl_WinIocpQCreate(SPSCQueue* q, SIZE_T capacity)
{
    /* Power of 2 only */
    if ((capacity & (capacity - 1)) == 0) {
	Tcl_Panic("Power of 2 only");
    }
    q->head = 0;
    q->tail = 0;
    q->mask = capacity - 1;
    q->buffer = (void**)malloc(capacity * sizeof(void*));
}

void
Tcl_WinIocpQDestroy(SPSCQueue* q)
{
    free(q->buffer);
}


bool
Tcl_WinIocpQPoPFront(SPSCQueue* q, void** out_value) {
    ULONG_PTR current_head = q->head;

#if defined(_M_ARM) || defined(_M_ARM64) || defined(__arm__) || defined(__aarch64__)
    MemoryBarrier();
#endif
    ULONG_PTR current_tail = q->tail;
    ATOMIC_FENCE_ACQUIRE(); /* Ensure tail visibility is pulled before matching */

    if (current_head == current_tail) {
	return false; /* Queue is empty */
    }

    *out_value = q->buffer[current_head & q->mask];

    /* Ensure copy-out finishes before releasing slot back to producer */
    ATOMIC_FENCE_RELEASE();
    q->head = current_head + 1;
    return true;
}

bool
Tcl_WinIocpQPushBack(SPSCQueue* q, void* item) {
    ULONG_PTR current_head = q->head;
    ATOMIC_FENCE_ACQUIRE(); /* Ensure head read finishes before tail evaluation */

    ULONG_PTR current_tail = q->tail;

    if ((current_tail - current_head) == (q->mask + 1)) {
	return false; /* Queue is full */
    }

    q->buffer[current_tail & q->mask] = item;

    /* Ensure data write completes before releasing index to consumer */
    ATOMIC_FENCE_RELEASE();
#if defined(_M_ARM) || defined(_M_ARM64) || defined(__arm__) || defined(__aarch64__)
    MemoryBarrier(); /* Hardware barrier fallback for weak-ordered ARM targets */
#endif

    q->tail = current_tail + 1;
    return true;
}


void
Tcl_WinIocpQPopAllCompare(PSLIST_HEADER pListHead, LPVOID pItem)
{
    // TODO: unlink the first entry, walk the list pulling out all matches for item, splice fixed list at head
    _InterlockedCompareExchange128(pListHead->Depth);
}

void
Tcl_WinIocpQPop(PSLIST_HEADER pListHead, PSLIST_ENTRY pListNode)
{
    // TODO: unsplice list, traverse it until node is found unlink it, replice list
}