/* ----------------------------------------------------------------------
 *
 * tclWinIOCP.c --
 *
 *	Routines for managing and running completion ports for I/O
 *	including the lock-free atomic queues.
 *
 * ----------------------------------------------------------------------
 * RCS: @(#) $Id: $
 * ----------------------------------------------------------------------
 */

#include "iocpsockInt.h"

struct iocpheader {

};

/* locals */
HANDLE cport;
HANDLE cpthread;

void InitializeIOCPSubSystem()
{
    // start thread
    // validate thread.
    // set exit handler
}

extern HANDLE
TclWinIOCPGetPort()
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
 *	Without direct interaction from Tcl, incoming I/O with be managed
 *	in their queue and replaced for the next operation.  Tcl will
 *	service them when the event loop is ready to.
 * 
 * ----------------------------------------------------------------------
 */

static DWORD WINAPI
CompletionThreadProc(LPVOID lpParam)
{
    CompletionPortInfo* cpinfo = (CompletionPortInfo*)lpParam;
    SocketInfo* infoPtr;
    BufferInfo* bufPtr;
    OVERLAPPED* ol;
    DWORD bytes, flags, WSAerr, error = NO_ERROR;
    BOOL ok;

#ifdef _DEBUG
#else
    __try {
#endif
    again:
	WSAerr = NO_ERROR;
	flags = 0;

	ok = GetQueuedCompletionStatus(cpinfo->port, &bytes,
	    (PULONG_PTR)&infoPtr, &ol, INFINITE);

	if (ok && !infoPtr) {
	    /* A NULL key indicates closure time for this thread. */
#ifdef _DEBUG
	    return error;
#else
	    __leave;
#endif
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
	     * the operation, call WSAGetOverlappedResult() to
	     * translate the error into a Winsock error code.
	     */

	    ok = WSAGetOverlappedResult(infoPtr->socket,
		ol, &bytes, FALSE, &flags);

	    if (!ok) {
		WSAerr = WSAGetLastError();
	    }
	}

	/* Go handle the IO operation. */
	HandleIo(infoPtr, bufPtr, cpinfo->port, bytes, WSAerr, flags);
	goto again;
#ifdef _DEBUG
#else
    }
    __except (error = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
	Tcl_Panic("Big ERROR!  IOCP Completion thread died with exception"
	    " code: %#x\n", error);
    }

    return error;
#endif
}
