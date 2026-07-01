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
 *	atomic FIFO queue routines.
 *
 * ----------------------------------------------------------------------
 * RCS: @(#) $Id: $
 * ----------------------------------------------------------------------
 */

#include "iocpsockInt.h"

struct iocpheader {

};

/* to be shared public protos */
extern DWORD		Tcl_WinIocpAssocHandle(HANDLE hndl, iocpheader* ocp);
extern DWORD		Tcl_WinIocpPostToCP(HANDLE hndl, iocpheader* ocp);
extern __inline LPVOID	Tcl_WinIocpNPPAlloc(SIZE_T size);
extern __inline LPVOID	Tcl_WinIocpNPPReAlloc(LPVOID block, SIZE_T size);
extern __inline BOOL	Tcl_WinIocpNPPFree(LPVOID block);

/* https://learn.microsoft.com/en-us/windows/win32/Sync/interlocked-singly-linked-lists */
extern PSLIST_HEADER	Tcl_WinIocpQCreate();
extern void		Tcl_WinIocpQDestroy(PSLIST_HEADER pListHead);
extern PSLIST_ENTRY	Tcl_WinIocpQNodeCreate();
extern void		Tcl_WinIocpQNodeDestroy(PSLIST_ENTRY pListNode);
extern LPVOID		Tcl_WinIocpQPoPFront(PSLIST_HEADER pListHead);
extern void		Tcl_WinIocpQPushBack(PSLIST_HEADER pListHead, PSLIST_ENTRY pListNode);
extern void		Tcl_WinIocpQPushFront(PSLIST_HEADER pListHead, PSLIST_ENTRY pListNode);
extern void		Tcl_WinIocpQPopAllCompare(PSLIST_HEADER pListHead, LPVOID pItem);
extern void		Tcl_WinIocpQPop(PSLIST_HEADER pListHead, PSLIST_ENTRY pListNode);

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
//static LONG StatSpecialBytesInUse = 0;

DWORD
InitializeIocpSubSystem()
{
    DWORD error = NO_ERROR;
    SYSTEM_INFO si;

    /* global/once init */
    if (InterlockedExchange(&initialized, 1) == 0) {

	/* Create the completion port. */
	cport = CreateIoCompletionPort(
		INVALID_HANDLE_VALUE, NULL, (ULONG_PTR)NULL, 0);
	if (cport == NULL) {
	    error = GetLastError();
	    goto error;
	}

	GetNativeSystemInfo(&si);
#define IOCP_HEAP_START_SIZE	(si.dwPageSize*64)  /* about 256k */

	/* Create the special private memory heap. */
	NPPheap = HeapCreate(0, IOCP_HEAP_START_SIZE, 0);
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
	 * Do not elevate priority. You could try
	 * THREAD_PRIORITY_BELOW_NORMAL for a more responsive Tk UI
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
    TclWinIocpBufferInfo* bufPtr;
    OVERLAPPED* ol;
    DWORD bytes, opErr, error = NO_ERROR;
    BOOL ok;

    again:
    opErr = NO_ERROR;

    ok = GetQueuedCompletionStatus(cport, &bytes,
	    (PULONG_PTR)&infoPtr, &ol, INFINITE);

    if (ok && !infoPtr) {
	/* A NULL key indicates closure time for this thread. */
	return error;
    }

	/*
	 * Use the pointer to the overlapped structure and derive from it
	 * the top of the parent BufferInfo structure it sits in.  If the
	 * position of the overlapped structure moves around within the
	 * BufferInfo structure declaration, this logic does _not_ need
	 * to be modified.
	 */

	bufPtr = CONTAINING_RECORD(ol, BufferInfo, ol);

	if (!ok) {
	    /*
	     * If GetQueuedCompletionStatus() returned a failure on
	     * the operation, call GetOverlappedResult() to
	     * retreive the error code.
	     */

	    ok = GetOverlappedResult(infoPtr->handle, ol,
		    &bytes, FALSE);

	    if (!ok) {
		opErr = GetLastError();
	    }
	}

	/* Go handle the IO operation. */
	infoPtr->serviceProc(infoPtr, bufPtr, cport, bytes, opErr);
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
    PostQueuedCompletionStatus(cport, 0, (ULONG_PTR)ocp, );
}



/* special pool */

__inline LPVOID
Tcl_WinIocpNPPAlloc(SIZE_T size)
{
    LPVOID p;
    p = HeapAlloc(NPPheap, HEAP_ZERO_MEMORY, size);
    //if (p) InterlockedExchangeAdd(&StatSpecialBytesInUse, size);
    return p;
}

__inline LPVOID
Tcl_WinIocpNPPReAlloc(LPVOID block, SIZE_T size)
{
    LPVOID p;
    SIZE_T oldSize;
    //oldSize = HeapSize(NPPheap, 0, block);
    p = HeapReAlloc(NPPheap, HEAP_ZERO_MEMORY, block, size);
    //if (p) InterlockedExchangeAdd(&StatSpecialBytesInUse, ((LONG)size - oldSize));
    return p;
}

__inline BOOL
Tcl_WinIocpNPPFree(LPVOID block)
{
    BOOL code;
    SIZE_T oldSize;
    //oldSize = HeapSize(NPPheap, 0, block);
    code = HeapFree(NPPheap, 0, block);
    //if (code) InterlockedExchangeAdd(&StatSpecialBytesInUse, -((LONG)oldSize));
    return code;
}

/* lock-free queue */

PSLIST_HEADER
Tcl_WinIocpQCreate()
{
    PSLIST_HEADER pListHead;

    pListHead = (PSLIST_HEADER)_aligned_malloc(sizeof(SLIST_HEADER),
	    MEMORY_ALLOCATION_ALIGNMENT);
    // TODO: check for error
    InitializeSListHead(pListHead);
    return pListHead;
}

void
Tcl_WinIocpQDestroy(PSLIST_HEADER pListHead)
{
    _aligned_free(pListHead);
}

PSLIST_ENTRY
Tcl_WinIocpQNodeCreate()
{
    PSLIST_ENTRY pListNode;

    pListNode = (PSLIST_ENTRY)_aligned_malloc(sizeof(SLIST_ENTRY),
	MEMORY_ALLOCATION_ALIGNMENT);
    return pListNode;
}

void
Tcl_WinIocpQNodeDestroy(PSLIST_ENTRY pListNode)
{
    _aligned_free(pListNode);
}

LPVOID
Tcl_WinIocpQPoPFront(PSLIST_HEADER pListHead)
{
    return InterlockedPopEntrySList(pListHead);
}

void
Tcl_WinIocpQPushBack(PSLIST_HEADER pListHead, PSLIST_ENTRY pListNode)
{
    InterlockedPushEntrySList(pListHead, pListNode);
}

void
Tcl_WinIocpQPushFront(PSLIST_HEADER pListHead, PSLIST_ENTRY pListNode)
{
    // TODO 
}

void
Tcl_WinIocpQPopAllCompare(PSLIST_HEADER pListHead, LPVOID pItem)
{
    // TODO: unlink the first entry, walk the list pulling out all matches for item, splice fixed list at head
    InterlockedExchangePointer();
}

void
Tcl_WinIocpQPop(PSLIST_HEADER pListHead, PSLIST_ENTRY pListNode)
{
    // TODO: unsplice list, traverse it until node is found unlink it, replice list
}