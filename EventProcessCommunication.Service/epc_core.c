#include "epc_core.h"
#include <stdio.h>
#include <errno.h>

static uint64_t g_connection_counter = 1;
static epc_list_head g_clients;
static epc_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static epc_list_head g_channels;
static epc_mutex_t g_channels_lock = PTHREAD_MUTEX_INITIALIZER;
//static uint64_t g_next_channel_id = 1;

struct ipc_client* searchClientByConnectionId(uint64_t connectionId)
{
    struct ipc_client* clientSearch;
    struct ipc_client* ipc_client_found = NULL;

    pthread_mutex_lock(&g_lock);
    epc_list_for_each_entry(clientSearch, &g_clients, list) {
        if (!clientSearch) continue;
        if (clientSearch->connection_id == connectionId)
        {
            ipc_client_found = clientSearch;
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);

    return ipc_client_found;
}

static int send_control_reply(struct ipc_client* dest, uint64_t to_channel_id, const void* payload, size_t payload_len, uint8_t controlAction)
{
    struct ipc_message* reply = calloc(1, sizeof(*reply));
    if (!reply) return -ENOMEM;
    reply->header.from_connection_id = 0;
    reply->header.channel_id = to_channel_id;
    size_t payload_withCtrl_len = payload_len + 1; // Cambia la grandezza del payload in modo da mettere anche il controlAction
    reply->header.payload_len = payload_withCtrl_len;
    reply->header.msg_type = 0xFF;
    if (payload_len > 0 && payload)
    {
        reply->payload = malloc(payload_withCtrl_len);
        if (!reply->payload)
        {
            free(reply);
            return -ENOMEM;
        }
        reply->payload[0] = controlAction;
        memcpy(reply->payload + 1, payload, payload_len); // Il primo carattere è il controlAction, non sovrascriverlo
    }
    pthread_mutex_lock(&dest->lock);
    epc_list_add_tail(&reply->list, &dest->queue);
    pthread_mutex_unlock(&dest->lock);
    epc_wake_up(&dest->waitq, &dest->lock);
    return 0;
}

void epc_core_global_init(void)
{
    epc_INIT_LIST_HEAD(&g_clients);
    epc_INIT_LIST_HEAD(&g_channels);
    pthread_mutex_init(&g_lock, 0);
    pthread_mutex_init(&g_channels_lock, 0);
}

struct ipc_client* epc_core_add_client(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_connection_counter >= INT64_MAX) // usa il massimo delle connessioni assegnabili, se arriva al limite nessun nuovo client può essere creato
        return NULL;
    pthread_mutex_unlock(&g_lock);

    struct ipc_client* c = calloc(1, sizeof(*c));
    
    if (!c) return NULL;
    
    epc_INIT_LIST_HEAD(&c->queue);
    epc_init_waitqueue(&c->waitq, &c->lock);
    pthread_mutex_init(&c->lock, 0);
    //pthread_mutex_init(&c->buffer_lock, 0);
    epc_INIT_LIST_HEAD(&c->subscriptions);

    pthread_mutex_lock(&g_lock);

    c->connection_id = g_connection_counter++; // TODO connection id same of channel only for server?
    c->disconnected = false;
    
    epc_list_add_tail(&c->list, &g_clients);
    
    pthread_mutex_unlock(&g_lock);
    
    return c;
}

void disconnectAllClients(struct ipc_client* client)
{
    struct ipc_client_sub* sub, * subtmp;
    uint64_t channelId = 0;

    pthread_mutex_lock(&client->lock);
    epc_list_for_each_entry_safe(sub, subtmp, &client->subscriptions, list) {
        channelId = sub->channel_id;
    }
    pthread_mutex_unlock(&client->lock);

    if (client->paired_channel_id == 0 && channelId > 0) // maybe is a server
    {
        struct ipc_client* client_Search;
        pthread_mutex_lock(&g_lock);
        epc_list_for_each_entry(client_Search, &g_clients, list) {
            if(!client_Search)
                continue;
            if (client_Search->paired_channel_id == channelId)
            {
                //printf("Disconnecting client id %ld\n", client_Search->connection_id);
                client_Search->disconnected = true;
                //client_Search->alive = 0;
                epc_wake_up(&client_Search->waitq, &client_Search->lock); // unlock reader to receive disconnection
            }
        }
        pthread_mutex_unlock(&g_lock);

        struct ipc_channel* ch;
        pthread_mutex_lock(&g_channels_lock);
        epc_list_for_each_entry(ch, &g_channels, list) {
            if (ch->id == channelId)
            {
                epc_list_del(&ch->list);
                free(ch);
                break;
            }
        }
        pthread_mutex_unlock(&g_channels_lock);
    }
}

void epc_core_remove_client(struct ipc_client* client) {
    struct ipc_message* msg, * tmp;
    struct ipc_client_sub* sub, * subtmp;

    if (client->paired_channel_id > 0)
    {
        uint64_t channelId = client->paired_channel_id;

        // Search the server
        struct ipc_client* ipc_server = searchClientByConnectionId(channelId);

        // Now notify the server of your disconnection
        if (ipc_server)
        {
            send_control_reply(
                ipc_server,
                channelId, // send notification to server (channelId)
                &client->connection_id, sizeof(client->connection_id), // send to server who is connected (connection Id)
                IPC_CTRL_DISCONNECTED_CLIENT
            );
        }
    }

    disconnectAllClients(client);
    
    pthread_mutex_lock(&g_lock);
    if (!epc_list_empty(&client->list))
        epc_list_del(&client->list);
    
    pthread_mutex_unlock(&g_lock);
    
    pthread_mutex_lock(&client->lock);
    epc_list_for_each_entry_safe(msg, tmp, &client->queue, list) {
        epc_list_del(&msg->list);
        free(msg->payload);
        free(msg);
    }
    epc_list_for_each_entry_safe(sub, subtmp, &client->subscriptions, list) {
        epc_list_del(&sub->list);
        free(sub);
    }
    pthread_mutex_unlock(&client->lock);
    
    pthread_mutex_destroy(&client->lock);

    //pthread_mutex_destroy(&client->buffer_lock);
    free(client);
}

// == Messaggi ==

static int do_create_channel(struct ipc_client* client, const void* data, size_t len)
{
    if (len < sizeof(struct ipc_channel_req)) return -EINVAL;
    const struct ipc_channel_req* req = data;
    struct ipc_channel* ch;
    int ret = 0;
    
    pthread_mutex_lock(&g_channels_lock);
    
    epc_list_for_each_entry(ch, &g_channels, list) {
        if (strcmp(ch->name, req->name) == 0) {
            ret = -EEXIST; goto unlock;
        }
    }
    ch = calloc(1, sizeof(*ch));
    if (!ch) { ret = -ENOMEM; goto unlock; }
    
    //ch->id = g_next_channel_id++;
    ch->id = client->connection_id;

    // Avoid buffer overflow
    if (strnlen(req->name, MAX_CHANNEL_NAME) >= MAX_CHANNEL_NAME) {
        return -EINVAL;
    }
    strncpy(ch->name, req->name, MAX_CHANNEL_NAME - 1);
    ch->name[MAX_CHANNEL_NAME - 1] = 0;
    
    ch->creator_conn_id = client->connection_id;
    
    epc_list_add_tail(&ch->list, &g_channels);
    ret = ch->id;

unlock:
    pthread_mutex_unlock(&g_channels_lock);
    return ret;
}

static int do_lookup_channel(struct ipc_client* client, const void* data, size_t len)
{
    struct ipc_channel_req req;
    struct ipc_channel* ch;
    uint64_t channel_id = (uint64_t)-1;
    if (len < sizeof(struct ipc_channel_req)) return -EINVAL;
    memcpy(&req, data, sizeof(req));
    req.name[MAX_CHANNEL_NAME - 1] = 0;
    pthread_mutex_lock(&g_channels_lock);
    epc_list_for_each_entry(ch, &g_channels, list) {
        if (strcmp(ch->name, req.name) == 0) { channel_id = ch->id; break; }
    }
    pthread_mutex_unlock(&g_channels_lock);
    return (channel_id != (uint64_t)-1) ? channel_id : -ENOENT;
}

static int do_subscribe_channel(struct ipc_client* client, const void* data, size_t len)
{
    uint64_t channel_id;
    struct ipc_client_sub* s;
    if (len < sizeof(uint64_t)) return -EINVAL;
    memcpy(&channel_id, data, sizeof(uint64_t));
    pthread_mutex_lock(&client->lock);
    epc_list_for_each_entry(s, &client->subscriptions, list)
        if (s->channel_id == channel_id) { pthread_mutex_unlock(&client->lock); return 0; }
    s = malloc(sizeof(*s));
    if (!s) { pthread_mutex_unlock(&client->lock); return -ENOMEM; }
    s->channel_id = channel_id;
    epc_list_add_tail(&s->list, &client->subscriptions);
    pthread_mutex_unlock(&client->lock);
    return 0;
}

static int do_unsubscribe_channel(struct ipc_client* client, const void* data, size_t len)
{
    uint64_t channel_id;
    struct ipc_client_sub* s, * tmp;
    if (len < sizeof(uint64_t)) return -EINVAL;
    memcpy(&channel_id, data, sizeof(uint64_t));
    pthread_mutex_lock(&client->lock);
    epc_list_for_each_entry_safe(s, tmp, &client->subscriptions, list) {
        if (s->channel_id == channel_id) {
            epc_list_del(&s->list);
            free(s);
            pthread_mutex_unlock(&client->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&client->lock);
    return -ENOENT;
}

static int do_connect(struct ipc_client* client, const void* data, size_t len, struct ipc_connect_req* reqout)
{
    if (len < sizeof(struct ipc_connect_req))
    {
        reqout->channel_id = 0;
        reqout->my_connection_id = 0;
        return -EINVAL;
    }

    memcpy(reqout, data, sizeof(*reqout));
    
    client->paired_channel_id = reqout->channel_id;
    reqout->my_connection_id = client->connection_id;

    return 0;
}

static int do_connect_by_channel_id(struct ipc_client* client, uint64_t channelId, size_t len, struct ipc_connect_req* reqout)
{
    if (channelId <= 0)
    {
        reqout->channel_id = 0;
        reqout->my_connection_id = 0;
        return -EINVAL;
    }
    
    client->paired_channel_id = channelId;
    
    reqout->channel_id = channelId;
    reqout->my_connection_id = client->connection_id;
        
    return 0;
}

static int do_disconnect(struct ipc_client* client, const void* data, size_t len) {
    // For simplicity: just disconnect this client
    client->disconnected = true;
    return 0;
}

// -- API PUBBLICA -- I/O --
ssize_t epc_core_set(struct ipc_client* sender, const char* buf, size_t len)
{
    if (!sender || sender->disconnected)
    {
        epc_wake_up(&sender->waitq, &sender->lock);
        return -EPC_ERROR_DISCONNECTED;
    }
    if (len > MAX_MSG_SIZE) return -EINVAL;

    struct ipc_message* msg = calloc(1, sizeof(*msg));
    if (!msg) return -ENOMEM;
    msg->payload = malloc(len);
    if (!msg->payload) { free(msg); return -ENOMEM; }
    memcpy(msg->payload, buf, len);

    if (len < sizeof(struct ipc_header)) { free(msg->payload); free(msg); return -EINVAL; }
    memcpy(&msg->header, msg->payload, sizeof(struct ipc_header));
    char* real_payload = msg->payload + sizeof(struct ipc_header);
    size_t real_len = len - sizeof(struct ipc_header);

    if (msg->header.msg_type == 0xFF && real_len >= sizeof(struct ipc_control_msg)) {
        const struct ipc_control_msg* ctrl = (const void*)real_payload;
        const void* ctrl_data = &ctrl->data[0];
        int64_t res = 0;
        size_t dlen = real_len - sizeof(struct ipc_control_msg);
        switch (ctrl->ctrl_cmd)
        {
            case IPC_CTRL_CREATE_CHANNEL:
            {
                int64_t channelId = do_create_channel(sender, ctrl_data, dlen);
                if (channelId <= 0)
                {
                    res = send_control_reply(sender, sender->connection_id, &res, sizeof(res), IPC_CTRL_CREATE_CHANNEL);
                    break;
                }
                res = do_subscribe_channel(sender, (void*) &(channelId), sizeof(channelId));
                if (res != 0)
                    res = send_control_reply(sender, sender->connection_id, &res, sizeof(res), IPC_CTRL_CREATE_CHANNEL);
                else
                    res = send_control_reply(sender, sender->connection_id, &channelId, sizeof(channelId), IPC_CTRL_CREATE_CHANNEL);
            }
            break;
            
            case IPC_CTRL_SUBSCRIBE_CHANNEL:
                res = do_subscribe_channel(sender, ctrl_data, dlen);
                res = send_control_reply(sender, sender->connection_id, &res, sizeof(res), IPC_CTRL_SUBSCRIBE_CHANNEL);
                break;
            
            case IPC_CTRL_UNSUBSCRIBE_CHANNEL:
                res = do_unsubscribe_channel(sender, ctrl_data, dlen);
                break;

            case IPC_CTRL_CONNECT_TO_CHANNEL_NAME:
            {
                struct ipc_connect_req req;
                int64_t channelId = do_lookup_channel(sender, ctrl_data, dlen);
                if (channelId <= 0) // Invalid channel name/id
                {
                    req.channel_id = 0;
                    req.my_connection_id = 0;
                }
                else
                {
                    // Notify the client of sucessfull connection
                    res = do_connect_by_channel_id(sender, (uint64_t)channelId, dlen, &req);
                }
                res = send_control_reply(sender, sender->connection_id, &req, sizeof(req), IPC_CTRL_CONNECT_TO_CHANNEL_NAME);

                if (channelId > 0)
                {
                    // Search the server
                    struct ipc_client* ipc_server = searchClientByConnectionId((uint64_t)channelId);

                    // Now notify the server of your connection
                    if (ipc_server)
                    {
                        send_control_reply(
                            ipc_server,
                            channelId, // send notification to server (channelId)
                            &sender->connection_id, sizeof(sender->connection_id), // send to server who is connected (connection Id)
                            IPC_CTRL_CONNECTED_CLIENT
                        );
                    }
                }
            }
            break;
            
            case IPC_CTRL_CONNECT_TO_CHANNEL_ID:
            {
                struct ipc_connect_req req;
                res = do_connect(sender, ctrl_data, dlen, &req);
                if (res == 0)
                    res = send_control_reply(sender, req.channel_id, &req, sizeof(req), IPC_CTRL_CONNECT_TO_CHANNEL_ID);
            }
            break;
            
            case IPC_CTRL_DISCONNECT:
                res = do_disconnect(sender, ctrl_data, dlen);
                break;
            
            default:
                res = -EINVAL;
        }
        // Per semplicit� restituisco res. Potresti inviare un messaggio risposta!
        free(msg->payload);
        free(msg);
        return res;
    }

    // == Smistamento del messaggio "normale", broadcast agli abbonati
    pthread_mutex_lock(&g_lock);
    struct ipc_client* target;
    
    epc_list_for_each_entry(target, &g_clients, list)
    {
        if (target == sender) continue;

        bool ok = false;
        struct ipc_client_sub* s;
        
        pthread_mutex_lock(&target->lock);
        epc_list_for_each_entry(s, &target->subscriptions, list) {
            if (s->channel_id == msg->header.channel_id) { ok = true; break; }
        }
        pthread_mutex_unlock(&target->lock);
        
        // Permetti anche la delivery diretta, come nella tua versione, se necessario
        if (!ok && target->connection_id == msg->header.channel_id) {
            struct ipc_channel* ch = NULL;
            bool allowed = false;

            pthread_mutex_lock(&g_channels_lock);
            epc_list_for_each_entry(ch, &g_channels, list) {
                if (ch->id == target->paired_channel_id) {
                    allowed = true;
                    break;
                }
            }
            pthread_mutex_unlock(&g_channels_lock);

            if (allowed && sender->connection_id == ch->creator_conn_id)
            {
                ok = true;
            }
            else
            {
                printf("epc: DROP direct: conn=%lu wants to message client=%lu (not creator of channel=%lu)\n",
                    sender->connection_id, target->connection_id, target->paired_channel_id);
                //printf("epc: DROP direct client: creator_conn_id=%llu channel id=%llu\n",
                //    ch->creator_conn_id, ch->id);
            }
        }

        if (!ok) continue;

        struct ipc_message* copy = calloc(1, sizeof(*copy));
        if (!copy) continue;

        copy->payload = malloc(real_len);
        if (!copy->payload) { free(copy); continue; }

        memcpy(copy->payload, real_payload, real_len);
        copy->header.payload_len = real_len;

        copy->header.channel_id = msg->header.channel_id;
        copy->header.from_connection_id = sender->connection_id;   //  msg->header.from_connection_id;
        copy->header.msg_type = msg->header.msg_type;
                
        pthread_mutex_lock(&target->lock);
        epc_list_add_tail(&copy->list, &target->queue);
        pthread_mutex_unlock(&target->lock);
        
        epc_wake_up(&target->waitq, &target->lock);
    }
    pthread_mutex_unlock(&g_lock);
    
    free(msg->payload);
    free(msg);
    
    return len;
}

ssize_t epc_core_get(struct ipc_client* client, char* buf)
{
    if (!client || client->disconnected)
        return -EPC_ERROR_DISCONNECTED;

    struct ipc_message* msg;
    ssize_t total_size;

    pthread_mutex_lock(&client->lock);

    while (epc_list_empty(&client->queue))
    {
        if (!client || client->disconnected) // Esci su disconnect!
        {
            pthread_mutex_unlock(&client->lock);
            return -EPC_ERROR_DISCONNECTED;
        }
        
        pthread_cond_wait(&client->waitq.cond, &client->lock);
    }

    msg = epc_list_first_entry(&client->queue, struct ipc_message, list);

    total_size = sizeof(struct ipc_header) + msg->header.payload_len;

#if defined(DEBUG)
    printf("new message %d\n", total_size);
#endif

    memcpy(buf, &msg->header, sizeof(struct ipc_header));
    memcpy(buf + sizeof(struct ipc_header), msg->payload, msg->header.payload_len);

    epc_list_del(&msg->list);

    free(msg->payload);
    free(msg);

    pthread_mutex_unlock(&client->lock);

    return total_size;
}
