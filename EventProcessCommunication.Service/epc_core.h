#ifndef EPC_CORE_H
#define EPC_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

//#define DEBUG true

#if defined(_WIN32) || defined(_WIN64)
typedef __int64 ssize_t;
#else
#include <sys/types.h>
#endif

#define MAX_MSG_SIZE 65536
#define MAX_CHANNEL_NAME 64

#define EPC_ERROR_DISCONNECTED 55

// -- Linked List Utilities --
typedef struct epc_list_head {
    struct epc_list_head* next, * prev;
} epc_list_head;

#define EPC_LIST_HEAD_INIT(name) { &(name), &(name) }
#define EPC_LIST_HEAD(name) \
    struct epc_list_head name = EPC_LIST_HEAD_INIT(name)

static inline void epc_INIT_LIST_HEAD(epc_list_head* list)
{
    list->next = list; list->prev = list;
}

static inline void epc_list_add_tail(epc_list_head* _new, epc_list_head* head)
{
    epc_list_head* prev = head->prev;
    _new->next = head;
    _new->prev = prev;
    prev->next = _new;
    head->prev = _new;
}

static inline void epc_list_del(epc_list_head* entry)
{
    entry->prev->next = entry->next;
    entry->next->prev = entry->prev;
    entry->next = entry->prev = NULL;
}

static inline int epc_list_empty(const epc_list_head* head)
{
    return head->next == head;
}

#define epc_list_entry(ptr, type, member) \
    ((type *)((char *)(ptr)-(unsigned long)(&((type *)0)->member)))

#define epc_list_first_entry(ptr, type, member) \
    epc_list_entry((ptr)->next, type, member)

#define epc_list_for_each_entry(pos, head, member) \
    for (pos = epc_list_entry((head)->next, typeof(*pos), member); \
        &pos->member != (head); pos = epc_list_entry(pos->member.next, typeof(*pos), member))

#define epc_list_for_each_entry_safe(pos, n, head, member) \
    for (pos = epc_list_entry((head)->next, typeof(*pos), member), \
        n = epc_list_entry(pos->member.next, typeof(*pos), member); \
        &pos->member != (head); \
        pos = n, n = epc_list_entry(n->member.next, typeof(*n), member))

//#define epc_list_count(ptr, type, member) \
//    for (pos = epc_list_entry((head)->next, typeof(*pos), member);

// -- Mutex --
#define epc_mutex_t      pthread_mutex_t
#define epc_mutex_init   pthread_mutex_init
#define epc_mutex_lock   pthread_mutex_lock
#define epc_mutex_unlock pthread_mutex_unlock
#define epc_mutex_destroy pthread_mutex_destroy

// -- Waitqueue condvar --
typedef struct
{
    pthread_cond_t cond;
//    pthread_mutex_t mutex;
    int ready;
} epc_waitqueue_t;

static inline void epc_init_waitqueue(epc_waitqueue_t* q, pthread_mutex_t* mutex)
{
    pthread_cond_init(&q->cond, NULL);
//    pthread_mutex_init(mutex, NULL);
    q->ready = 0;
}

static inline void epc_wake_up(epc_waitqueue_t* q, pthread_mutex_t* mutex)
{
    pthread_mutex_lock(mutex);
    q->ready = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(mutex);
}

static inline void epc_wait_for(epc_waitqueue_t* q, pthread_mutex_t* mutex)
{
    pthread_mutex_lock(mutex);

    while (!q->ready)
        pthread_cond_wait(&q->cond, mutex);

    q->ready = 0;

    pthread_mutex_unlock(mutex);
}

// -- Structs --
#pragma pack(push, 1)
struct ipc_header
{
    uint64_t from_connection_id;
    uint64_t channel_id;
    uint32_t payload_len;
    uint8_t msg_type;
};
#pragma pack(pop)

struct ipc_message
{
    struct ipc_header header;
    char* payload;
    epc_list_head list;
};

struct ipc_client_sub
{
    uint64_t channel_id;
    epc_list_head list;
};

struct ipc_client
{
    uint64_t connection_id;
    uint64_t paired_channel_id;
    epc_list_head queue;
    epc_waitqueue_t waitq;
    epc_mutex_t lock;
//    epc_mutex_t buffer_lock;
    epc_list_head list;
    epc_list_head subscriptions;
    bool disconnected;
};

struct ipc_channel
{
    uint64_t id;
    char name[MAX_CHANNEL_NAME];
    uint64_t creator_conn_id;
    epc_list_head list;
};

struct ipc_channel_req
{
    char name[MAX_CHANNEL_NAME];
    uint64_t id;
};

struct ipc_connect_req
{
    uint64_t channel_id;
    uint64_t my_connection_id;
};

enum ControlTypes
{
    IPC_CTRL_CREATE_CHANNEL = 1,
    IPC_CTRL_SUBSCRIBE_CHANNEL = 2,
    IPC_CTRL_UNSUBSCRIBE_CHANNEL = 3,
    IPC_CTRL_CONNECT_TO_CHANNEL_NAME = 4,
    IPC_CTRL_CONNECT_TO_CHANNEL_ID = 5,
    IPC_CTRL_DISCONNECT = 6,
    IPC_CTRL_CONNECTED_CLIENT = 7,
    IPC_CTRL_DISCONNECTED_CLIENT = 8,
};

struct ipc_control_msg
{
    uint8_t ctrl_cmd;
    char pad[7];
    uint8_t data[]; // variable flex array
};

void epc_core_global_init(void);
struct ipc_client* epc_core_add_client(void);
void epc_core_remove_client(struct ipc_client* client);
ssize_t epc_core_set(struct ipc_client* sender, const char* buf, size_t len);
ssize_t epc_core_get(struct ipc_client* client, char* buf);
void disconnectAllClients(struct ipc_client* client);

// Optionally: implement get_client_by_connid(), etc.

#endif
