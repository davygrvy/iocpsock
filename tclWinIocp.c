/* ----------------------------------------------------------------------
 *
 * tclWinIocp.c --
 *
 *	Shared routines for managing and running a completion port for
 *	overlapped I/O on windows common to the native channel drivers
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
extern DWORD TclWinIocpAssocHandle(HANDLE hndl, mystruct ocp);

/* shared private protos */
extern DWORD InitializeIocpSubSystem();

/* local protos */
static DWORD WINAPI CompletionThreadProc(LPVOID lpParam);
Tcl_ExitProc IocpExitHandler;

/* file-scope globals */
static LONG initialized = 0;
static HANDLE cport;
static HANDLE cpthread;
static HANDLE NPPheap;

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
		0, NULL);
	if (cpthread == NULL) {
	    error = GetLastError();
	    HeapDestroy(NPPheap);
	    CloseHandle(cport);
	    goto error;
	}

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

extern HANDLE
TclWinIocpGetPort()
{
    return cport;
}

DWORD
TclWinIocpAssocHandle(HANDLE hndl, mystruct ocp)
{
    CreateIoCompletionPort(hndl, cport, ocp, 0);
    return NO_ERROR;
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
    DWORD bytes, err, error = NO_ERROR;
    BOOL ok;

    again:
	err = NO_ERROR;

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
		err = GetLastError();
	    }
	}

	/* Go handle the IO operation. */
	infoPtr->serviceProc(infoPtr, bufPtr, cport, bytes, err);
	goto again;
}
