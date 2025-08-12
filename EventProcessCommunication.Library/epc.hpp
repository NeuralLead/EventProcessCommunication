#ifndef EPC_HPP
#define EPC_HPP

#define MAX_MSG_SIZE 65536

#if defined(_WIN32) || defined(_WIN64)
#include <cstdint>
#include <cstring>
#include <io.h>
#include <fcntl.h>
typedef __int64 ssize_t;
#define EPC_DEVICE "\\\\.\\pipe\\nl_epc"
#include <Windows.h>
#include <io.h>
#include <string>
#else
#include <sys/types.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <bits/stdc++.h>
#include <fcntl.h>
#define EPC_DEVICE "/dev/nl_epc"

#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))

#if !defined(DKMS_MODE)
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#endif

#endif

#include <vector>
#include <thread>
#include <atomic>
#include <exception>
#include <stdexcept>
#include <iostream>
#include <queue>
#include <mutex>
#include <functional>
#include <unordered_map>
#include <future>

#include "thread_pool.hpp"

enum class EPC_NodeTypes : int8_t
{
    None = -1,
    Server = 0,
    Client = 1
};

enum class EPC_OpcodeTypes : uint8_t
{
    Fragment = 0,
    Text = 1,
    Binary = 2,
    Control = 0xFF,
};

#pragma pack(push, 1)
struct ipc_header
{
    uint64_t from_connection_id;
    uint64_t channel_id;
    uint32_t payload_len;
    EPC_OpcodeTypes msg_type;
};
#pragma pack(pop)

struct ipc_channel_req
{
    char name[64];
    uint64_t id;
};

struct ipc_connect_req
{
    uint64_t channel_id;
    uint64_t my_connection_id;
};

struct ipc_control_msg
{
    uint8_t ctrl_cmd;
    char pad[7];
    uint8_t data[];
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

struct PartialMsg
{
    std::vector<char> data;
    ipc_header last_hdr;
};

// Callback types
using MessageHandler = std::function<void(const ipc_header&, const std::vector<char>&)>;
using OnConnectedHandler = std::function<void()>;
using OnChannelCreatedHandler = std::function<void()>;
using OnDisconnected = std::function<void()>;
using OnError = std::function<void(uint8_t)>;
using OnClientConnectionStatus = std::function<void(int64_t)>;

// Struttura per eventi asincroni
struct AsyncEvent
{
    enum Type { MESSAGE = 0, CHANNEL_CREATED, CONNECTED_TO_SERVER, DISCONNECTED, CLIENT_CONNECTED, CLIENT_DISCONNECTED, ON_ERROR = 0xFF};
    Type type;
    ipc_header header;
    std::vector<char> data;
    uint8_t errorControlNo;
    int64_t status;

    AsyncEvent(Type t) : type(t) {}
    AsyncEvent(Type t, const ipc_header& h, const std::vector<char>& d)
        : type(t), header(h), data(d) {
    }

    AsyncEvent(Type t, ControlTypes errorControl) : type(t)
    {
        errorControlNo = errorControl;
    }

    AsyncEvent(Type t, int64_t s) : type(t)
    {
        status = s;
    }
};

class EventProcessComunication
{
private:
    // Core I/O
    intptr_t fd;
#if !defined(_WIN32) && !defined(_WIN64) && defined(DKMS_MODE)
    int epfd;
#endif

    // State atomico (thread-safe senza mutex)
    std::atomic<uint64_t> channelId{ 0 };
    std::atomic<uint64_t> clientConnectionId{ 0 };
    std::atomic<EPC_NodeTypes> nodeType{ EPC_NodeTypes::None };
    std::atomic<bool> stopFlag{ false };

    // Handlers
    MessageHandler handlerOnMessage;
    OnConnectedHandler handlerOnConnected;
    OnChannelCreatedHandler handlerOnChannelCreated;
    OnDisconnected handlerOnDisconnected;
    OnError handlerOnError;
    OnClientConnectionStatus handlerClientDisconnected;
    OnClientConnectionStatus handlerClientConnected;

    // Threading
    std::thread readerThread;
    std::thread processingThread;
    std::thread eventThread;

    // Event queue per handlers asincroni
    std::queue<AsyncEvent> eventQueue;
    std::mutex eventQueueMutex;
    std::condition_variable eventCondition;

    // Raw message queue per processing asincrono
    struct RawMessage {
        std::vector<char> buffer;
        size_t size;
        RawMessage(const char* data, size_t len) : buffer(data, data + len), size(len) {}
    };
    std::queue<RawMessage> rawMessageQueue;
    std::mutex rawMessageMutex;
    std::condition_variable rawMessageCondition;

    // Messaggi parziali (protetti da mutex specifico)
    std::unordered_map<uint64_t, PartialMsg> partialMessages;
    std::mutex partialMutex;

    // I/O mutex (solo per le operazioni di lettura/scrittura)
    std::mutex ioMutex_w;

#if defined(_WIN32) || defined(_WIN64)
    OVERLAPPED overlappedRead = { 0 };
    OVERLAPPED overlappedWrite = { 0 };
#endif

public:
    EventProcessComunication(const char* device = EPC_DEVICE) : fd(-1)
#if !defined(_WIN32) && !defined(_WIN64) && defined(DKMS_MODE)
        , epfd(-1)
#endif
    {
        InitEPC(device);
    }

    ~EventProcessComunication()
    {
        std::cout << "Stopping EPC..." << std::endl;

        stopFlag = true;
        eventCondition.notify_all();
        rawMessageCondition.notify_all();

        if (readerThread.joinable()) readerThread.join();
        if (processingThread.joinable()) processingThread.join();
        if (eventThread.joinable()) eventThread.join();

#if defined(_WIN32) || defined(_WIN64)
        if (fd != -1 && fd != (intptr_t)INVALID_HANDLE_VALUE)
            CloseHandle((HANDLE)fd);
#else
#if defined(DKMS_MODE)
        if (epfd >= 0) close(epfd);
#endif
        if (fd >= 0) close(fd);
#endif
    }

    bool createChannel(const std::string& ch_name)
    {
        if (nodeType.load() != EPC_NodeTypes::None)
            throw std::runtime_error("Solo un server può creare un canale. Inizializzare questa classe e creare il canale");

        ipc_channel_req req = {};
        std::strncpy(req.name, ch_name.c_str(), sizeof(req.name) - 1);
        req.id = 0;

        ssize_t r = sendCommand(IPC_CTRL_CREATE_CHANNEL, &req, sizeof(req));
        return r > 0;
    }

    bool Connect(const std::string& ch_name)
    {
        EPC_NodeTypes currentType = nodeType.load();
        if (currentType == EPC_NodeTypes::Server)
            throw std::runtime_error("A server cannot connect directly to a client");
        if (currentType == EPC_NodeTypes::Client)
            throw std::runtime_error("You are already connected. Please disconnect before making a new connection");

        ipc_channel_req req = {};
        std::strncpy(req.name, ch_name.c_str(), sizeof(req.name) - 1);
        req.id = 0;

        ssize_t r = sendCommand(IPC_CTRL_CONNECT_TO_CHANNEL_NAME, &req, sizeof(req));
        return r > 0;
    }

    bool Connect(uint64_t channelid)
    {
        ipc_connect_req req{};
        req.channel_id = channelid;
        ssize_t r = sendCommand(IPC_CTRL_CONNECT_TO_CHANNEL_ID, &req, sizeof(req));
        return r > 0;
    }

    void Disconnect()
    {
        if (nodeType.load() == EPC_NodeTypes::None)
            throw std::runtime_error("You need to connect to a channel first");

        int64_t nullParameter = 0;
        sendCommand(IPC_CTRL_DISCONNECT, &nullParameter, sizeof(nullParameter));
    }

    void DisconnectClient(uint64_t clientConnectionId)
    {
        EPC_NodeTypes currentType = nodeType.load();
        if (currentType == EPC_NodeTypes::None)
            throw std::runtime_error("You need to create a channel and subscribe to it first");
        if (currentType == EPC_NodeTypes::Client)
            throw std::runtime_error("Only a server can disconnect a client");

        sendCommand(IPC_CTRL_DISCONNECT, &clientConnectionId, sizeof(clientConnectionId));
    }

    bool subscribeChannel(uint64_t channel_id)
    {
        ssize_t r = sendCommand(IPC_CTRL_SUBSCRIBE_CHANNEL, &channel_id, sizeof(channel_id));
        if (r > 0) {
            nodeType = EPC_NodeTypes::Server;
            return true;
        }
        return false;
    }

    bool unsubscribeChannel(uint64_t channel_id)
    {
        ssize_t r = sendCommand(IPC_CTRL_UNSUBSCRIBE_CHANNEL, &channel_id, sizeof(channel_id));
        return r > 0;
    }

    void set_message_handler(MessageHandler h)
    {
        handlerOnMessage = std::move(h);
        startEventLoop();
    }

    void set_on_connected_handler(OnConnectedHandler h)
    {
        handlerOnConnected = std::move(h);
    }

    void set_on_channel_created_handler(OnChannelCreatedHandler h)
    {
        handlerOnChannelCreated = std::move(h);
    }

    void set_on_disconnected_handler(OnDisconnected h)
    {
        handlerOnDisconnected = std::move(h);
    }

    void set_on_error_handler(OnError h)
    {
        handlerOnError = std::move(h);
    }

    void set_on_client_connected_handler(OnClientConnectionStatus h)
    {
        handlerClientConnected = std::move(h);
    }

    void set_on_client_disconnected_handler(OnClientConnectionStatus h)
    {
        handlerClientDisconnected = std::move(h);
    }

    // Send methods per client
    size_t send(void* data, size_t length, EPC_OpcodeTypes messageType)
    {
        if (nodeType.load() != EPC_NodeTypes::Client)
            throw std::runtime_error("You are not connected");
        return internalSend(channelId.load(), data, length, messageType);
    }

    size_t sendString(const std::string& data)
    {
        return send((void*)data.data(), data.size(), EPC_OpcodeTypes::Text);
    }

    size_t sendString(const char* data, size_t length)
    {
        return send((void*)data, length, EPC_OpcodeTypes::Text);
    }

    size_t sendBytes(void* data, size_t length)
    {
        return send(data, length, EPC_OpcodeTypes::Binary);
    }

    // Send methods per server
    size_t sendStringToClient(uint64_t channelId, const std::string& data)
    {
        return sendToClient(channelId, (void*)data.data(), data.size(), EPC_OpcodeTypes::Text);
    }

    size_t sendStringToClient(uint64_t channelId, const char* data, size_t length)
    {
        return sendToClient(channelId, (void*)data, length, EPC_OpcodeTypes::Text);
    }

    size_t sendBytesToClient(uint64_t channelId, void* data, size_t length)
    {
        return sendToClient(channelId, data, length, EPC_OpcodeTypes::Binary);
    }

    size_t sendToClient(uint64_t channelId, void* data, size_t length, EPC_OpcodeTypes messageType)
    {
        if (nodeType.load() != EPC_NodeTypes::Server)
            throw std::runtime_error("Only a listening server can send to a client");
        return internalSend(channelId, data, length, messageType);
    }

    uint64_t getChannelId() const
    {
        return channelId.load();
    }

private:
    void InitEPC(const char* device)
    {
#if defined(_WIN32) || defined(_WIN64)
        fd = (intptr_t)CreateFile(
            device,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED /* | FILE_FLAG_NO_BUFFERING */ ,
            NULL);

        if (fd == (intptr_t)INVALID_HANDLE_VALUE)
            throw std::runtime_error("Cannot open device");

        DWORD le = GetLastError();
        if (le != 0 && le != ERROR_PIPE_BUSY)
            throw std::runtime_error("Could not open pipe. GLE=" + std::to_string(le));
#else
#if defined(DKMS_MODE)
        fd = open(device, O_RDWR);
        if (fd < 0)
            throw std::runtime_error("Cannot open device");

        epfd = epoll_create1(0);
        if (epfd < 0)
            throw std::runtime_error("Cannot create epoll");

        epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
#else
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error("socket failed");

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strcpy(addr.sun_path, device);

        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
            throw std::runtime_error("Cannot open device");
#endif
#endif
    }

    std::atomic<int> ready{ 0 };

    void startEventLoop()
    {
        ready = 0;

        if (!readerThread.joinable()) {
            readerThread = std::thread([this]() { readerLoop(); });
        }
        if (!processingThread.joinable()) {
            processingThread = std::thread([this]() { processingLoop(); });
        }
        if (!eventThread.joinable()) {
            eventThread = std::thread([this]() { eventLoop(); });
        }

        while (ready < 3);
    }

    void readerLoop()
    {
        std::cout << "[EPC READER] started" << std::endl;

        char buffer[MAX_MSG_SIZE];
        memset(buffer, 0, sizeof(char) * MAX_MSG_SIZE);

#if defined(_WIN32) || defined(_WIN64)
        if (!overlappedRead.hEvent)
        {
            overlappedRead.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            if (overlappedRead.hEvent == NULL) {
                std::cerr << "Errore nella creazione dell'evento. Codice: " << GetLastError() << "\n";
                CloseHandle((HANDLE)fd);
                return;
            }
        }
#endif

        ready++;

        while (!stopFlag.load())
        {
#if !defined(_WIN32) && !defined(_WIN64) && defined(DKMS_MODE)
            epoll_event events[1];
            int nfds = epoll_wait(epfd, events, 1, 100);
            if (nfds == -1) {
                if (errno == EINTR) continue;
                perror("epoll_wait");
                break;
            }
            if (nfds == 0) continue;
#endif

            ssize_t n = safeRead(buffer);
            if (n == 0) continue; // n == 0, riprova
            if (n < 0) {
                printf("ERRORE CRITICO SMISTAMENTO %d\n", n); break; // Errore critico
            }

            // Metti immediatamente il messaggio in coda per processing asincrono
            pushRawMessage(buffer, n);
        }

#if defined(_WIN32) || defined(_WIN64)
        CloseHandle(overlappedRead.hEvent);
#endif

        stopFlag = true;
        pushRawMessage(buffer, 0); // make a void message to stop all other threads (process/events)

        std::cout << "[EPC READER] stopped" << std::endl;
    }

    void processingLoop()
    {
        std::cout << "[EPC PROCESSING] started" << std::endl;

        ready++;

        while (!stopFlag.load())
        {
            std::unique_lock<std::mutex> lock(rawMessageMutex);
            rawMessageCondition.wait(lock, [this] {
                return !rawMessageQueue.empty() || stopFlag.load();
            });

            while (!rawMessageQueue.empty() && !stopFlag.load())
            {
                RawMessage msg = std::move(rawMessageQueue.front());
                rawMessageQueue.pop();
                lock.unlock();
                
                // Processa il messaggio senza bloccare la lettura
                if (!processMessage((char *) msg.buffer.data(), msg.size)) {
                    std::cerr << "[EPC PROCESSING] Error processing message" << std::endl;
                }

                lock.lock();
            }
        }

        std::cout << "[EPC PROCESSING] stopped" << std::endl;

        // Reset state quando il processing si ferma
        channelId = 0;
        clientConnectionId = 0;
        nodeType = EPC_NodeTypes::None;

        // Signal disconnection
        pushEvent(AsyncEvent(AsyncEvent::DISCONNECTED));
    }

    void eventLoop()
    {
        std::cout << "[EPC EVENT] started" << std::endl;

        ready++;

        while (!stopFlag.load())
        {
            std::unique_lock<std::mutex> lock(eventQueueMutex);
            eventCondition.wait(lock, [this] {
                return !eventQueue.empty() || stopFlag.load();
                });

            while (!eventQueue.empty() && !stopFlag.load())
            {
                AsyncEvent event = eventQueue.front();
                eventQueue.pop();
                lock.unlock();

                //std::cout << "New event" << std::endl;

                // Chiama handlers senza lock

                std::thread([this, event]() { // avvia in un thread senò gli eventi successivi non vengono lanciati
                    this->handleEvent(event);
                }).detach();

                lock.lock();
            }
        }

        std::cout << "[EPC EVENT] stopped" << std::endl;
    }

    void handleEvent(const AsyncEvent& event)
    {
        switch (event.type)
        {
            case AsyncEvent::MESSAGE:
                if (handlerOnMessage) handlerOnMessage(event.header, event.data);
                break;
            
            case AsyncEvent::CONNECTED_TO_SERVER:
                if (handlerOnConnected) handlerOnConnected();
                break;
            
            case AsyncEvent::CHANNEL_CREATED:
                if (handlerOnChannelCreated) handlerOnChannelCreated();
                break;
            
            case AsyncEvent::DISCONNECTED:
                if (handlerOnDisconnected) handlerOnDisconnected();

#if defined(_WIN32) || defined(_WIN64)
                if (overlappedWrite.hEvent) CloseHandle((HANDLE)fd);
                fd = -1;
#endif
                break;

            case AsyncEvent::CLIENT_CONNECTED:
                if (handlerClientConnected) handlerClientConnected(event.status);
                break;

            case AsyncEvent::CLIENT_DISCONNECTED:
                if (handlerClientDisconnected) handlerClientDisconnected(event.status);
                break;

            case AsyncEvent::ON_ERROR:
                if (handlerOnError) handlerOnError(event.errorControlNo);
                break;

            default:
                break;
            }
    }

    void pushEvent(const AsyncEvent& event)
    {
        std::lock_guard<std::mutex> lock(eventQueueMutex);
        eventQueue.push(event);
        eventCondition.notify_one();
    }

    void pushRawMessage(const char* buffer, size_t size)
    {
        std::lock_guard<std::mutex> lock(rawMessageMutex);
        rawMessageQueue.emplace(buffer, size);
        rawMessageCondition.notify_one();
    }

    bool processMessage(char* buffer, size_t n)
    {
        const size_t min_size = sizeof(ipc_header);
        if (n < min_size) return false;

        ipc_header* hdr = reinterpret_cast<ipc_header*>(buffer);

        if (hdr->payload_len > MAX_MSG_SIZE) {
            std::cerr << "[Security] Payload troppo grande: " << hdr->payload_len << std::endl;
            return false;
        }

        const size_t expected_total_size = sizeof(ipc_header) + hdr->payload_len;
        if (n < expected_total_size) {
            std::cerr << "[Security] Buffer incompleto" << std::endl;
            return false;
        }

        const char* payload = buffer + sizeof(ipc_header);
        uint64_t key = hdr->from_connection_id ^ (hdr->channel_id << 32);

        if (hdr->msg_type == EPC_OpcodeTypes::Fragment) {
            handleFragment(key, *hdr, payload, hdr->payload_len);
        }
        else if (hdr->msg_type == EPC_OpcodeTypes::Text || hdr->msg_type == EPC_OpcodeTypes::Binary) {
            handleCompleteMessage(key, *hdr, payload, hdr->payload_len);
        }
        else if (hdr->msg_type == EPC_OpcodeTypes::Control) {
            handleControlMessage(payload, hdr->payload_len);
        }

        return true;
    }

    void handleFragment(uint64_t key, const ipc_header& hdr, const char* payload, size_t len)
    {
        std::lock_guard<std::mutex> lock(partialMutex);
        auto& msg = partialMessages[key];
        if (msg.data.empty()) msg.last_hdr = hdr;
        msg.data.insert(msg.data.end(), payload, payload + len);
    }

    void handleCompleteMessage(uint64_t key, const ipc_header& hdr, const char* payload, size_t len)
    {
        std::vector<char> msgData;
        ipc_header outHdr = hdr;

        {
            std::lock_guard<std::mutex> lock(partialMutex);
            auto it = partialMessages.find(key);
            if (it != partialMessages.end()) {
                msgData = std::move(it->second.data);
                if (!msgData.empty()) outHdr = it->second.last_hdr;
                partialMessages.erase(it);
            }
        }

        msgData.insert(msgData.end(), payload, payload + len);
        outHdr.msg_type = hdr.msg_type;
        outHdr.payload_len = (uint32_t)msgData.size();

        pushEvent(AsyncEvent(AsyncEvent::MESSAGE, outHdr, msgData));
    }

    void handleControlMessage(const char* payload, size_t len)
    {
        if (len < sizeof(uint8_t)) return;

        uint8_t controlAction = payload[0];

        switch (controlAction)
        {
        case IPC_CTRL_CREATE_CHANNEL: {
            if (len < sizeof(uint8_t) + sizeof(int64_t)) return;
            int64_t value = 0;
            memcpy(&value, payload + 1, sizeof(value));
            if (value <= 0)
            {
                pushEvent(AsyncEvent(AsyncEvent::ON_ERROR, /* error = */ IPC_CTRL_CREATE_CHANNEL));
            }
            else
            {
                channelId = value;
                nodeType = EPC_NodeTypes::Server;
                pushEvent(AsyncEvent(AsyncEvent::CHANNEL_CREATED));
            }
            break;
        }
        
        case IPC_CTRL_SUBSCRIBE_CHANNEL: {
            if (len < sizeof(uint8_t) + sizeof(int64_t)) return;
            int64_t value = 0;
            memcpy(&value, payload + 1, sizeof(value));
            channelId = value;
            nodeType = EPC_NodeTypes::Server;
            break;
        }
        
        case IPC_CTRL_CONNECT_TO_CHANNEL_NAME:
        case IPC_CTRL_CONNECT_TO_CHANNEL_ID: {
            if (len < sizeof(uint8_t) + sizeof(ipc_connect_req)) return;
            ipc_connect_req reply;
            memcpy(&reply, payload + 1, sizeof(reply));
            clientConnectionId = reply.my_connection_id;
            channelId = reply.channel_id;
            nodeType = EPC_NodeTypes::Client;
            pushEvent(AsyncEvent(AsyncEvent::CONNECTED_TO_SERVER));
            break;
        }
        
        case IPC_CTRL_DISCONNECT:
            stopFlag = true;
            break;
        
        case IPC_CTRL_CONNECTED_CLIENT:
        {
            if (len < sizeof(uint8_t) + sizeof(int64_t)) return;
            int64_t value = 0;
            memcpy(&value, payload + 1, sizeof(value));
            pushEvent(AsyncEvent(AsyncEvent::CLIENT_CONNECTED, value));
            break;
        }

        case IPC_CTRL_DISCONNECTED_CLIENT:
        {
            if (len < sizeof(uint8_t) + sizeof(int64_t)) return;
            int64_t value = 0;
            memcpy(&value, payload + 1, sizeof(value));
            pushEvent(AsyncEvent(AsyncEvent::CLIENT_DISCONNECTED, value));
            break;
        }

        default:
            break;
        }
    }

    ssize_t safeRead(void* buf)
    {
#if defined(_WIN32) || defined(_WIN64)

        DWORD readed = 0;
        BOOL ok = ReadFile((HANDLE)fd, buf, (DWORD)MAX_MSG_SIZE, &readed, &overlappedRead);

        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            // L'operazione è in corso, attendiamo completamento
            DWORD wait = WaitForSingleObject(overlappedRead.hEvent, 1000); // Timeout 5s

            switch (wait) {
            case WAIT_OBJECT_0:
                ok = GetOverlappedResult((HANDLE)fd, &overlappedRead, &readed, FALSE);
                
                if (ok)
                    return readed;

                std::cerr << "GetOverlappedResult fallita. Codice: " << GetLastError() << "\n";
                return -1;
                break;
            
            case WAIT_TIMEOUT:
                return 0;
                break;
            
            default:
                std::cerr << "Errore in WaitForSingleObject. Codice: " << GetLastError() << "\n";
                return -1;
            }
        }

        //printf("[R] %d\n", readed);
        return (ssize_t)readed;
#else
        return read(fd, buf, size);
#endif
    }

    ssize_t safeWrite(const void* buf, size_t size)
    {
        std::lock_guard<std::mutex> lock(ioMutex_w);

#if defined(_WIN32) || defined(_WIN64)

        // OVERLAPPED structure
        if (!overlappedWrite.hEvent)
        {
            overlappedWrite.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
            if (overlappedWrite.hEvent == NULL) {
                std::cerr << "Errore nella creazione dell'evento. Codice: " << GetLastError() << "\n";
                CloseHandle((HANDLE)fd);
                return -1;
            }
        }

        DWORD written = 0;
        BOOL ok = WriteFile((HANDLE)fd, buf, (DWORD)size, &written, &overlappedWrite);

        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            // L'operazione è in corso, attendiamo completamento
            //std::cout << "Operazione asincrona in attesa...\n";
            DWORD wait = WaitForSingleObject(overlappedWrite.hEvent, 5000); // Timeout 5s

            switch (wait) {
            case WAIT_OBJECT_0:
                ok = GetOverlappedResult((HANDLE)fd, &overlappedWrite, &written, FALSE);
                if (ok)
                    return written;

                std::cerr << "GetOverlappedResult fallita. Codice: " << GetLastError() << "\n";
                return -1;

                break;

            case WAIT_TIMEOUT:
                return 0;
                break;

            default:
                std::cerr << "Errore in WaitForSingleObject. Codice: " << GetLastError() << "\n";
                //CloseHandle(overlappedWrite.hEvent);
                return -1;
            }
        }
        
        //printf("[W] %d\n", written);
        return (ssize_t)written;
#else
        return write(fd, buf, size);
#endif
    }

    ssize_t sendCommand(uint8_t commandId, void* data, size_t length)
    {
        if (length > MAX_MSG_SIZE - sizeof(ipc_control_msg)) return -1;

        ipc_header hdr = {};
        hdr.from_connection_id = 0;
        hdr.channel_id = 0;
        hdr.payload_len = sizeof(ipc_control_msg) + length;
        hdr.msg_type = EPC_OpcodeTypes::Control;

        const size_t total_size = sizeof(hdr) + sizeof(ipc_control_msg) + length;
        std::vector<char> buf(total_size);

        size_t off = 0;
        memcpy(buf.data() + off, &hdr, sizeof(hdr));
        off += sizeof(hdr);

        ipc_control_msg cmd = {};
        cmd.ctrl_cmd = commandId;
        memcpy(buf.data() + off, &cmd, sizeof(cmd));
        off += sizeof(cmd);

        if (length > 0 && data != nullptr) {
            memcpy(buf.data() + off, data, length);
        }

        return safeWrite(buf.data(), buf.size());
    }

    size_t internalSend(uint64_t targetChannelId, void* data, size_t length, EPC_OpcodeTypes msgType)
    {
        size_t total_sent = 0;
        const char* data_ptr = static_cast<const char*>(data);
        size_t bytes_left = length;
        size_t maxChunk = (size_t)MAX_MSG_SIZE - sizeof(ipc_header);

        while (bytes_left > 0) {
            size_t chunk_size = min(bytes_left, maxChunk);
            ipc_header hdr = {};
            hdr.msg_type = (bytes_left > maxChunk) ? EPC_OpcodeTypes::Fragment : msgType;
            hdr.from_connection_id = 0;
            hdr.channel_id = targetChannelId;
            hdr.payload_len = static_cast<uint32_t>(chunk_size);

            std::vector<char> buf(sizeof(hdr) + chunk_size);
            memcpy(buf.data(), &hdr, sizeof(hdr));
            memcpy(buf.data() + sizeof(hdr), data_ptr, chunk_size);

            ssize_t sent = safeWrite(buf.data(), buf.size());
            //if (sent != (ssize_t)buf.size()) return total_sent; // TODO ?
            if (sent != (ssize_t)buf.size())
                return -1;

            total_sent += chunk_size;
            data_ptr += chunk_size;
            bytes_left -= chunk_size;
        }
        return total_sent;
    }
};

#endif // EPC_HPP