#include "epc_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#define FREE_PERMISSIONS true
#define IO_TIMEOUT 100 // 100 ms of timeout w/r (Windows)

// -- Macro cross-io (solo qui; tutto il resto in epc_core � portabile e pthread!) --
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <io.h>

#define EPC_PIPE_NAME "\\\\.\\pipe\\nl_epc"
#define epc_fd_invalid(x) ((x)==INVALID_HANDLE_VALUE)

typedef HANDLE epc_fd_t;

#define epc_fd_close(x) CloseHandle(x)

#else /* Linux / UNIX */

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define EPC_PIPE_NAME "/dev/nl_epc"
#define epc_fd_invalid(x) ((x)<0)

typedef int epc_fd_t;

#define epc_fd_close(x) close(x)

#endif

/* ASYNC WRITE QUEUE */

typedef struct write_item {
    size_t len;
    char data[MAX_MSG_SIZE];
    struct write_item* next;
} write_item_t;

typedef struct write_queue {
    write_item_t* head;
    write_item_t* tail;
    pthread_mutex_t lock;
    pthread_cond_t notempty;
    int alive;
} write_queue_t;

static void write_queue_init(write_queue_t* q)
{
    q->head = q->tail = NULL;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->notempty, NULL);
    q->alive = 1;
}

/* End ASYNC WRITE QUEUE */

#if defined(_WIN32) || defined(_WIN64)
// Struttura per gestire operazioni OVERLAPPED
typedef struct {
    OVERLAPPED overlapped;
    HANDLE event;
    char buffer[MAX_MSG_SIZE];
    DWORD bytes_transferred;
    BOOL operation_pending;
} epc_overlapped_op_t;
#endif

// ---------- struttura per worker ----------
typedef struct {
    epc_fd_t io;
    struct ipc_client* cli;
#if defined(_WIN32) || defined(_WIN64)
    HANDLE reader, writer;
#else
    pthread_t reader, writer;
#endif
    int alive;
    bool canWrite;
    bool connectionClosed;
    bool destroing;

#if defined(_WIN32) || defined(_WIN64)
    epc_overlapped_op_t read_op;
    epc_overlapped_op_t write_op;
#endif

    write_queue_t async_writer_queue;
//#if !defined(_WIN32) && !defined(_WIN64)
    pthread_t async_writer_thread; // TODO
//#endif
} epc_worker_t;

static void epc_destroy_worker(epc_worker_t* w, bool isCallerWriter);

#if defined(_WIN32) || defined(_WIN64)
static int init_overlapped_op(epc_overlapped_op_t* op) {
    memset(&op->overlapped, 0, sizeof(OVERLAPPED));
    op->event = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (op->event == NULL) return -1;
    op->overlapped.hEvent = op->event;
    op->operation_pending = FALSE;
    return 0;
}

// Cleanup operazione OVERLAPPED
static void cleanup_overlapped_op(epc_overlapped_op_t* op) {
    if (op->event) {
        CloseHandle(op->event);
        op->event = NULL;
    }
}
#endif

static void* epc_async_writer_dispatcher(void* arg);
static void write_queue_stop(write_queue_t* q);
static int write_queue_pop(write_queue_t* q, void* out, size_t* outlen);
static void write_queue_push(write_queue_t* q, const void* data, size_t len);
static void write_queue_init(write_queue_t* q);

#if defined(_WIN32) || defined(_WIN64)
static ssize_t epc_fd_write(OVERLAPPED* overlappedWrite, epc_fd_t fd, const void* buf, size_t n)
#else
static ssize_t epc_fd_write(epc_fd_t fd, const void* buf, size_t n)
#endif
{
#if defined(_WIN32) || defined(_WIN64)
    DWORD written = 0;

    if (!overlappedWrite->hEvent) return -1;

    BOOL result = WriteFile(fd, buf, (DWORD)n, &written, overlappedWrite);

    if (!result)
    {
        DWORD error = GetLastError();
        
        if (error == ERROR_IO_PENDING)
        {
            // Operazione in corso, aspetta il completamento
            DWORD wait_result = WaitForSingleObject(overlappedWrite->hEvent, IO_TIMEOUT);

            switch (wait_result) {
            case WAIT_OBJECT_0:
                if (GetOverlappedResult(fd, overlappedWrite, &written, FALSE)) {
                    //CloseHandle(overlappedWrite.hEvent);
                    return (ssize_t)written;
                }
                return -1;
                break;

            case WAIT_TIMEOUT:
                //CancelIo(fd);
                //CloseHandle(overlappedWrite.hEvent);
                return 0; // Timeout specifico

            default:
                return -1;
            }

            // Timeout o errore
            //CancelIo(fd);
        }
        
        switch (error) {
            case ERROR_BROKEN_PIPE:
            case ERROR_PIPE_NOT_CONNECTED:
                return -EPC_ERROR_DISCONNECTED;

            default:
                return -1;
        }
        
        //CloseHandle(overlappedWrite.hEvent);
        return -1;
    }

    //CloseHandle(overlappedWrite.hEvent);
#if defined(DEBUG)
    printf("[WRTE  X] %d\n", written);
#endif
    return (ssize_t)written;
#else
    
    // UNIX
    return write(fd, buf, n);
#endif
}

/*static ssize_t epc_fd_read(OVERLAPPED overlappedRead, epc_fd_t fd, void* buf, size_t n)
{
#if defined(_WIN32) || defined(_WIN64)
    DWORD readed = 0;

    // Crea evento per questa operazione
    if(!overlappedRead.hEvent)
        overlappedRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!overlappedRead.hEvent) return -1;

    BOOL result = ReadFile(fd, buf, (DWORD)n, &readed, &overlappedRead);

    if (!result) {
        DWORD error = GetLastError();

        if (error == ERROR_IO_PENDING) {
            // Operazione asincrona in corso, aspetta il completamento
            DWORD wait_result = WaitForSingleObject(overlappedRead.hEvent, INFINITE);

            if (wait_result == WAIT_OBJECT_0) {
                if (GetOverlappedResult(fd, &overlappedRead, &readed, FALSE)) {
                    //CloseHandle(overlappedRead.hEvent);
                    return (ssize_t)readed;
                }
            }

            // Errore nel GetOverlappedResult o WaitForSingleObject
            //CancelIo(fd);
            //CloseHandle(overlappedRead.hEvent);
            return -1;
        }

        // Altri errori
        //CloseHandle(overlappedRead.hEvent);

        switch (error) {
        case ERROR_BROKEN_PIPE:
        case ERROR_PIPE_NOT_CONNECTED:
            return -EPC_ERROR_DISCONNECTED;  // Connection closed gracefully
        case ERROR_INVALID_HANDLE:
            return -1;
        default:
            //printf("ReadFile error: %lu\n", error);
            return -1;
        }
    }

    // Operazione completata immediatamente
    //CloseHandle(overlappedRead.hEvent);
#if defined(DEBUG)
    printf("[READ  X] %d\n", readed);
#endif
    return (ssize_t)readed;
#else
    return read(fd, buf, n);
#endif
}*/

// VERSIONE ALTERNATIVA: ReadFile con timeout per permettere controllo alive
#if defined(_WIN32) || defined(_WIN64)
static ssize_t epc_fd_read_with_timeout(OVERLAPPED* overlappedRead, epc_fd_t fd, void* buf)
#else
static ssize_t epc_fd_read_with_timeout(epc_fd_t fd, void* buf)
#endif
{
#if defined(_WIN32) || defined(_WIN64)
    DWORD readed = 0;

    if (!overlappedRead->hEvent) return -1;

    BOOL result = ReadFile(fd, buf, (DWORD)MAX_MSG_SIZE, &readed, overlappedRead);
#if defined(DEBUG)
    printf("[READ  X] %d\n", readed);
#endif

    if (!result) {
        DWORD error = GetLastError();

        if (error == ERROR_IO_PENDING) {
            DWORD wait_result = WaitForSingleObject(overlappedRead->hEvent, IO_TIMEOUT);

            switch (wait_result) {
            case WAIT_OBJECT_0:
                if (GetOverlappedResult(fd, overlappedRead, &readed, FALSE)) {
                    //CloseHandle(overlappedRead.hEvent);
                    //overlappedRead.hEvent = NULL;
                    return (ssize_t)readed;
                }
                return -1;
                break;

            case WAIT_TIMEOUT:
                //CancelIo(fd);
                //CloseHandle(overlappedRead.hEvent);
                return 0; // Timeout specifico

            default:
                //CancelIo(fd);
                //CloseHandle(overlappedRead.hEvent);
                return -1;
            }
        }

        //CloseHandle(overlappedRead.hEvent);

        switch (error) {
            case ERROR_BROKEN_PIPE:
            case ERROR_PIPE_NOT_CONNECTED:
                return -EPC_ERROR_DISCONNECTED;
            
            default:
                return -1;
        }
    }

    //CloseHandle(overlappedRead.hEvent);
    //overlappedRead.hEvent = NULL;
    return (ssize_t)readed;
#else

    // UNIX
    return read(fd, buf, MAX_MSG_SIZE);
#endif
}

// ---------- READER THREAD (dal client -> core) ----------
static void* epc_worker_reader(void* arg)
{
    epc_worker_t* w = (epc_worker_t*)arg;
    char buf[MAX_MSG_SIZE];
    
    while (!w->canWrite) ; // Aspetta che pure il thread write sia pronto
    
#if defined(_WIN32) || defined(_WIN64)
    OVERLAPPED overlappedRead = { 0 };

    if (!overlappedRead.hEvent)
        overlappedRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
#endif

    while (w && w->alive)
    {
#if defined(_WIN32) || defined(_WIN64)        
        // Usa timeout per permettere controllo periodico di w->alive
        ssize_t n = epc_fd_read_with_timeout(&overlappedRead, w->io, buf);
#else
        ssize_t n = epc_fd_read_with_timeout(w->io, buf); // 1 sec timeout
#endif
        
        if (n == 0)
        {
            // Timeout - continua il loop per controllare w->alive

#if !defined(_WIN32) && !defined(_WIN64) && !defined(DKMS_MODE)
            // Only in linux socket mode, call break if == 0
            break;
#endif

            continue;
        }
        if (n < 0) break;
        
        // Scrittura principale in core (smista a code/abbonati)
        epc_core_set(w->cli, buf, n);
        //write_queue_push(&w->async_writer_queue, buf, n); // async
    }

#if defined(_WIN32) || defined(_WIN64)
    if(overlappedRead.hEvent)
        CloseHandle(overlappedRead.hEvent);
#endif
    
    epc_destroy_worker(w, false);

    printf("Disconnection READER done\n");

    return NULL;
}

// --------- WRITER THREAD (da core verso il client) ---------
static void* epc_worker_writer(void* arg)
{
    epc_worker_t* w = (epc_worker_t*)arg;
    char buf[MAX_MSG_SIZE];

    w->canWrite = true; // Informa che il thread write è pronto

#if defined(_WIN32) || defined(_WIN64)
    OVERLAPPED overlappedWrite = { 0 };

    // Crea evento per questa operazione
    if (!overlappedWrite.hEvent)
        overlappedWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
#endif

    while (w && w->alive)
    {
        ssize_t n = epc_core_get(w->cli, buf);
        
        if (n == 0)
            continue;
        
        if (n < 0) break;
//            if (n == -EPC_ERROR_DISCONNECTED || !w->alive) break;
        
        ssize_t wr = 0;
        while (wr == 0)
        {
#if defined(_WIN32) || defined(_WIN64)
            wr = epc_fd_write(&overlappedWrite, w->io, buf, n);
#else
            wr = epc_fd_write(w->io, buf, n);
#endif
        }

        if (wr != n) { break; }
    }

#if defined(_WIN32) || defined(_WIN64)
    if (overlappedWrite.hEvent)
        CloseHandle(overlappedWrite.hEvent);
#endif

    epc_destroy_worker(w, true);

    printf("Disconnection WRITER done\n");

    return NULL;
}

// --------- CLEANUP ----------
static void epc_destroy_worker(epc_worker_t* w, bool isCallerWriter)
{
    if (!w) return;
    if (w->destroing) return;
    w->destroing = true;

    if(w->cli)
        printf("Disconnecting %lld\n", w->cli->connection_id);

    // Segnala terminazione
    w->alive = 0;
    w->canWrite = false;

    // Forza terminazione thread bloccati
    if (w->cli)
    {
        w->cli->disconnected = true;

//        if(w->cli->lock != NULL)
            epc_wake_up(&w->cli->waitq, &w->cli->lock);
    }

    // Stop async writer
    /*write_queue_stop(&w->async_writer_queue);
#if defined(_WIN32) && defined(_WIN64)
    if (w->async_writer_thread.p)
        pthread_join(w->async_writer_thread, NULL);
#else
    if (w->async_writer_thread)
        pthread_join(w->async_writer_thread, NULL);
#endif*/

    // Chiudi handle per sbloccare eventuali ReadFile/WriteFile
    if (!w->connectionClosed)
    {
        w->connectionClosed = true;

#if defined(_WIN32) || defined(_WIN64)
        // Cancella eventuali operazioni I/O in corso
        CancelIo(w->io);
#endif

        if(w->io)
            epc_fd_close(w->io);
    }

    // Aspetta terminazione thread (con timeout)
#if defined(_WIN32) || defined(_WIN64)
    if (isCallerWriter && w->reader) {
        DWORD wait_result = WaitForSingleObject(w->reader, 3000); // 3 sec timeout
        if (wait_result == WAIT_TIMEOUT) {
#if defined(DEBUG)
            printf("Warning: Reader thread timeout, forcing termination\n");
#endif
            TerminateThread(w->reader, 0);
        }
        CloseHandle(w->reader);
    }

    if (!isCallerWriter && w->writer) {
        DWORD wait_result = WaitForSingleObject(w->writer, 3000);
        if (wait_result == WAIT_TIMEOUT) {
#if defined(DEBUG)
            printf("Warning: Writer thread timeout, forcing termination\n");
#endif
            TerminateThread(w->writer, 0);
        }
        CloseHandle(w->writer);
    }

#else
    // ASPETTA che i thread terminino prima di liberare la memoria
    if (w->reader && isCallerWriter) pthread_join(w->reader, NULL);
    if (w->writer && !isCallerWriter) pthread_join(w->writer, NULL);
#endif

    // Solo ora è sicuro rimuovere il client e liberare w
    if (w->cli) epc_core_remove_client(w->cli);
    if(w) free(w);

    printf("Disconnection done\n");
}

// ================= MAIN ACCEPTOR/PARENT ======================

// Fork worker: crea client, thread per lettura e scrittura, tutto legato al fd.
static void epc_spawn_worker(epc_fd_t cfd) {
    epc_worker_t* w = calloc(1, sizeof(*w));
    if (!w) {
        epc_fd_close(cfd);
        return;
    }

    w->io = cfd;
    w->cli = epc_core_add_client();

    if (!w->cli) {
        //printf("Failed to add client\n");
        epc_fd_close(cfd);
        free(w);
        return;
    }

    w->alive = 1;
    w->canWrite = false;
    w->connectionClosed = false;
    w->destroing = false;

    write_queue_init(&w->async_writer_queue);
//#if !defined(_WIN32) && !defined(_WIN64)
//    pthread_create(&w->async_writer_thread, NULL, epc_async_writer_dispatcher, w); // TODO
//#endif

#if defined(_WIN32) || defined(_WIN64)
    if (init_overlapped_op(&w->read_op) != 0 ||
        init_overlapped_op(&w->write_op) != 0) {
#if defined(DEBUG)
        printf("Failed to initialize OVERLAPPED operations\n");
#endif
        cleanup_overlapped_op(&w->read_op);
        cleanup_overlapped_op(&w->write_op);
        epc_core_remove_client(w->cli);
        epc_fd_close(cfd);
        free(w);
        return;
    }
#endif

    // Crea thread
#if defined(_WIN32) || defined(_WIN64)
    w->writer = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)epc_worker_writer, w, 0, NULL);
    w->reader = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)epc_worker_reader, w, 0, NULL);

    if (w->writer == NULL || w->reader == NULL) {
        //printf("Failed to create worker threads\n");
        if (w->writer) CloseHandle(w->writer);
        if (w->reader) CloseHandle(w->reader);
        cleanup_overlapped_op(&w->read_op);
        cleanup_overlapped_op(&w->write_op);
        
        epc_core_remove_client(w->cli);
        epc_fd_close(cfd);
        free(w);
        return;
    }
#else
    int ret1 = pthread_create(&w->writer, NULL, epc_worker_writer, w);
    int ret2 = pthread_create(&w->reader, NULL, epc_worker_reader, w);

    if (ret1 != 0 || ret2 != 0) {
        //printf("Failed to create worker threads\n");
        w->alive = 0;
        if (ret1 == 0) pthread_join(w->writer, NULL);
        if (ret2 == 0) pthread_join(w->reader, NULL);
        
        epc_core_remove_client(w->cli);
        epc_fd_close(cfd);
        free(w);
    }
#endif
}

// ---- SERVER ENTRYPOINT CROSS-PLATFORM -----
int main()
{
    epc_core_global_init();
#if defined(_WIN32) || defined(_WIN64)
    printf("EPC SERVER - WINDOWS - full duplex namedpipe '%s'\n", EPC_PIPE_NAME);

    for (;;)
    {
        HANDLE hPipe = CreateNamedPipeA(
            EPC_PIPE_NAME, 
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, /*  */
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            0, 0, 
            0, NULL);

        if (epc_fd_invalid(hPipe))
        {
            printf("CreateNamedPipe failed: %lu\n", GetLastError());
            return 1;
        }
        
        BOOL ok = ConnectNamedPipe(hPipe, NULL) || 
            (GetLastError() == ERROR_PIPE_CONNECTED);
        
        if (ok)
        {
            //printf("New client accepted\n");
            epc_spawn_worker(hPipe);
        }
        else
        {
            epc_fd_close(hPipe);
        }
    }
#else
    printf("EPC SERVER - LINUX - unixsock at %s\n", EPC_PIPE_NAME);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("epc_service cannot open socket"); return 1; }
    
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, EPC_PIPE_NAME);
    unlink(EPC_PIPE_NAME);
    
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { perror("epc_service cannot bind"); return 2; }
    
#if defined(FREE_PERMISSIONS)
    if (chmod(EPC_PIPE_NAME, 0666) < 0) {
        perror("epc_service cannot chmod");
        return 3;
    }
#endif
    
    listen(fd, 16);
    for (;;) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) { perror("epc_service cannot accept"); continue; }
        //printf("New client accepted\n");
        epc_spawn_worker(cfd);
    }
    epc_fd_close(fd);
#endif
}







/* ASYNC WRITE QUEUE */

static void* epc_async_writer_dispatcher(void* arg)
{
    epc_worker_t* w = (epc_worker_t*)arg;
    char buf[MAX_MSG_SIZE];
    size_t n;
    while (w->alive) {
        if (!write_queue_pop(&w->async_writer_queue, buf, &n))
            break;
        epc_core_set(w->cli, buf, n);
    }
    return NULL;
}

static void write_queue_push(write_queue_t* q, const void* data, size_t len)
{
    write_item_t* item = malloc(sizeof(write_item_t));
    item->len = len;
    memcpy(item->data, data, len);
    item->next = NULL;
    pthread_mutex_lock(&q->lock);
    if (q->tail)
        q->tail->next = item;
    else
        q->head = item;
    q->tail = item;
    pthread_cond_signal(&q->notempty);
    pthread_mutex_unlock(&q->lock);
}

static int write_queue_pop(write_queue_t* q, void* out, size_t* outlen)
{
    pthread_mutex_lock(&q->lock);
    while (q->alive && !q->head)
        pthread_cond_wait(&q->notempty, &q->lock);
    if (!q->head) { pthread_mutex_unlock(&q->lock); return 0; }
    write_item_t* item = q->head;
    q->head = item->next;
    if (!q->head) q->tail = NULL;
    *outlen = item->len;
    memcpy(out, item->data, item->len);
    free(item);
    pthread_mutex_unlock(&q->lock);
    return 1;
}

static void write_queue_stop(write_queue_t* q)
{
    pthread_mutex_lock(&q->lock);
    q->alive = 0;
    pthread_cond_broadcast(&q->notempty);
    pthread_mutex_unlock(&q->lock);
}