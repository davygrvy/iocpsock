#include <windows.h>
#include <stdio.h>
#include <stdbool.h>

// Custom key to identify console input packets in the IOCP loop
#define COMPLETION_KEY_CONSOLE ((ULONG_PTR)0x99)
#define COMPLETION_KEY_SHUTDOWN ((ULONG_PTR)0x00)

// Structure to pass context to the threadpool callback
typedef struct {
    HANDLE hIOCP;
    HANDLE hConsoleInput;
    PTP_WAIT pTpWait;
} ConsoleWaitContext;

// Callback triggered by Windows when CONIN$ has data available
VOID CALLBACK ConsoleWaitCallback(
    PTP_CALLBACK_INSTANCE Instance,
    PVOID                 Context,
    PTP_WAIT              Wait,
    TP_WAIT_RESULT        WaitResult
) {
    ConsoleWaitContext* context = (ConsoleWaitContext*)Context;
    UNREFERENCED_PARAMETER(Instance);
    UNREFERENCED_PARAMETER(Wait);

    if (WaitResult != WAIT_OBJECT_0) {
        return; // Handle potential wait errors or timeouts
    }

    // Input is ready. Signal our main IOCP loop.
    // dwNumberOfBytesTransferred is set to 1 just to indicate an event is ready.
    PostQueuedCompletionStatus(
        context->hIOCP,
        1,                              
        COMPLETION_KEY_CONSOLE,         
        NULL                            // No overlapped structure needed here
    );

    // Note: Do NOT call ReadConsoleInput inside this callback. 
    // Let the main IOCP thread handle the reading to keep processing centralized.
}

// Simulated main IOCP worker thread function
DWORD WINAPI IocpWorkerThread(LPVOID lpParam) {
    ConsoleWaitContext* context = (ConsoleWaitContext*)lpParam;
    DWORD bytesTransferred = 0;
    ULONG_PTR completionKey = 0;
    LPOVERLAPPED pOverlapped = NULL;

    printf("[IOCP] Worker thread started. Type something and press Enter...\n");

    while (true) {
        // Block efficiently on the completion port
        BOOL result = GetQueuedCompletionStatus(
            context->hIOCP,
            &bytesTransferred,
            &completionKey,
            &pOverlapped,
            INFINITE
        );

        if (!result && pOverlapped == NULL) {
            fprintf(stderr, "[IOCP] GetQueuedCompletionStatus failed. Error: %lu\n", GetLastError());
            break;
        }

        // Check if the packet is our custom shutdown signal
        if (completionKey == COMPLETION_KEY_SHUTDOWN && bytesTransferred == 0 && pOverlapped == NULL) {
            printf("[IOCP] Shutdown packet received. Exiting worker thread.\n");
            break;
        }

        // Handle Console Input Packet
        if (completionKey == COMPLETION_KEY_CONSOLE) {
            INPUT_RECORD inputBuffer[128];
            DWORD eventsRead = 0;

            // Drain the console input buffer safely on the IOCP thread
            if (ReadConsoleInputW(context->hConsoleInput, inputBuffer, 128, &eventsRead)) {
                for (DWORD i = 0; i < eventsRead; ++i) {
                    if (inputBuffer[i].EventType == KEY_EVENT && inputBuffer[i].Event.KeyEvent.bKeyDown) {
                        printf("[IOCP] Key pressed: '%c'\n", inputBuffer[i].Event.KeyEvent.uChar.AsciiChar);
                        
                        // Example exit trigger (Escape key)
                        if (inputBuffer[i].Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
                            printf("[IOCP] Escape pressed. Triggering graceful exit...\n");
                            return 0; 
                        }
                    }
                }
            }

            // RE-ARM the wait object so it monitors the next input batch.
            // This must be done after draining the buffer to prevent an infinite callback loop.
            SetThreadpoolWait(context->pTpWait, context->hConsoleInput, NULL);
        }
    }

    return 0;
}

int main(void) {
    // 1. Create the I/O Completion Port
    HANDLE hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (!hIOCP) {
        fprintf(stderr, "Failed to create IOCP. Error: %lu\n", GetLastError());
        return 1;
    }

    // 2. Open CONIN$ synchronously
    HANDLE hConsoleInput = CreateFileW(
        L"CONIN$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        0, // FILE_FLAG_OVERLAPPED is intentionally omitted/ignored
        NULL
    );

    if (hConsoleInput == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to open CONIN$. Error: %lu\n", GetLastError());
        CloseHandle(hIOCP);
        return 1;
    }

    // Ensure window/mouse inputs don't flood the queue (optional)
    SetConsoleMode(hConsoleInput, ENABLE_EXTENDED_FLAGS | ENABLE_INSERT_MODE | ENABLE_QUICK_EDIT_MODE);

    // 3. Create context structure
    ConsoleWaitContext context;
    context.hIOCP = hIOCP;
    context.hConsoleInput = hConsoleInput;

    // 4. Create Threadpool Wait Object
    PTP_WAIT pTpWait = CreateThreadpoolWait(ConsoleWaitCallback, &context, NULL);
    if (!pTpWait) {
        fprintf(stderr, "Failed to create Threadpool Wait. Error: %lu\n", GetLastError());
        CloseHandle(hConsoleInput);
        CloseHandle(hIOCP);
        return 1;
    }
    context.pTpWait = pTpWait;

    // 5. Arm the wait object for the first time
    SetThreadpoolWait(pTpWait, hConsoleInput, NULL);

    // 6. Start the IOCP Worker thread using the standard Windows API
    HANDLE hWorkerThread = CreateThread(NULL, 0, IocpWorkerThread, &context, 0, NULL);
    if (!hWorkerThread) {
        fprintf(stderr, "Failed to create worker thread. Error: %lu\n", GetLastError());
        CloseThreadpoolWait(pTpWait);
        CloseHandle(hConsoleInput);
        CloseHandle(hIOCP);
        return 1;
    }

    // Block main execution until the worker thread exits (runs until Escape is pressed)
    WaitForSingleObject(hWorkerThread, INFINITE);
    CloseHandle(hWorkerThread);

    // 7. Graceful Leak-Free Cleanup
    printf("[Main] Cleaning up resources...\n");
    
    // Cancels pending threadpool waits and blocks until executing callbacks finish
    CloseThreadpoolWait(pTpWait); 
    
    CloseHandle(hConsoleInput);
    
    // Post exit package to clear any other blocked IOCP threads if necessary
    PostQueuedCompletionStatus(hIOCP, 0, COMPLETION_KEY_SHUTDOWN, NULL); 
    CloseHandle(hIOCP);

    printf("[Main] Application shut down cleanly without leaks.\n");
    return 0;
}
