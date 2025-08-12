# EPC - Event-based IPC (Inter-Process Communication) Library

**EPC** is a modern, header-only C++ IPC/event library for Linux and Windows, implementing robust and cross-platform inter-process, full-duplex communication channels.  
It allows you to create **event-based IPC servers and clients** with publish/subscribe semantics, channel creation and subscription, direct messaging, and event hooks - all without requiring any external dependencies (except for pthreads on Unix/Linux, standard on all POSIX platforms).

---

## Key Features

- **Header-only**: Just drop `epc.hpp`/`epc_core.h` in your project.
- **Cross-Platform**: Transparent support for Windows (NamedPipes) and Linux (Unix sockets).
- **No Dependencies**: No 3rd party libs needed, just pthreads (for Unix).
- **Event-based**: Easy callback handlers for all IPC events (messages, connects, disconnects, errors, etc).
- **Channels**: Supports dynamic channel creation, lookup, subscribing/unsubscribing, direct send.
- **Full Duplex**: Simultaneous bidirectional communication.
- **Simple API**: Minimal, modern C++ interface for your server/client code.
- **Performance**: Lightweight, multi-threaded, scalable with multiple clients.
- **Error Handling**: Control responses on connect/disconnect/errors.
- **Broadcast & Direct**: Pub/sub or 1:1 delivery.

---

## Basic Concepts

- **Server**: Creates/owns named channels. Receives connections, messages, and notifications about client (dis)connections.
- **Client**: Connects to an existing named channel, sends/receives messages, can also send direct messages to the server.
- **Channel**: Logical message bus for one or more clients; clients can subscribe (pub/sub).
- **Event Handlers**: Register handler functions for all events (messages, connection, etc).
- **Message Format**: All IPC is typed and headered for easy handling & deserialization.

---

## Example: Server (C++)

```cpp
// server.cpp
#include "epc.hpp"
#include <iostream>
#include <thread>
#include <chrono>

EventProcessComunication ipc(EPC_DEVICE);

void print_msg(const ipc_header& hdr, const std::vector<char>& data) {
    // Print received message payload as a string
    std::cout << " | Data: " << std::string(data.begin(), data.end()) << "\n";

    // Echo back "ciao" to sender
    std::string ciao = "ciao";
    ipc.sendStringToClient(hdr.from_connection_id, ciao);
}

void channel_created() {
    uint64_t channelId = ipc.getChannelId();
    if (channelId <= 0)
        std::cout << "Error creating channel, status: " << channelId << std::endl;
    else
        std::cout << "Created channel with id " << channelId << std::endl;
}

void on_client_connected(int64_t connectionId) {
    std::cout << "A new client with id " << connectionId << " is connected\n";
}

void on_client_disconnected(int64_t connectionId) {
    std::cout << "Client with id " << connectionId << " has disconnected\n";
}

void on_error(uint8_t control) {
    std::cout << "Error on control type " << (int)control << std::endl;
}

int main() {
    ipc.set_on_error_handler(on_error);
    ipc.set_message_handler(print_msg);
    ipc.set_on_channel_created_handler(channel_created);
    ipc.set_on_client_connected_handler(on_client_connected);
    ipc.set_on_client_disconnected_handler(on_client_disconnected);

    // Try to create channel named "custom"
    while (!ipc.createChannel("custom")) {
        std::cout << "Cannot create the channel, retrying...\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    // Main loop (keep running)
    while (true)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}
```

---

## Example: Client (C++)

```cpp
// client.cpp
#include "epc.hpp"
#include <iostream>
#include <thread>
#include <chrono>

EventProcessComunication ipc(EPC_DEVICE);

void print_msg(const ipc_header& hdr, const std::vector<char>& data) {
    std::cout << " | Data: " << std::string(data.begin(), data.end()) << "\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(89));
    ipc.sendString(":-)", 4); // Reply with smiley
}

void on_connected() {
    uint64_t channelId = ipc.getChannelId();
    if (channelId <= 0) {
        std::cout << "Error connecting to channel, status: " << channelId << std::endl;
        return;
    }
    std::cout << "Connected to server, channelId=" << channelId << std::endl;

    // Send JSON example
    std::string json = R"({"channelId":123,"path":"/hello","data":{"msg":"Hello EPC!"}})";
    ipc.sendString(json);

    // Periodically send "cycle"
    while (true) {
        ipc.sendString("cycle", 5);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void on_disconnected() {
    std::cout << "Disconnected from server/EPC\n";
}

void on_error(uint8_t control) {
    std::cout << "Error on control type " << (int)control << std::endl;
}

int main() {
    ipc.set_on_error_handler(on_error);
    ipc.set_message_handler(print_msg);
    ipc.set_on_connected_handler(on_connected);
    ipc.set_on_disconnected_handler(on_disconnected);

    // Connect to the channel named "custom"
    if (!ipc.Connect("custom"))
        std::cout << "Cannot connect to channel\n";

    while (true);
}
```

---

## Usage

### 1. Add Library

- Copy `epc.hpp` or `epc_core.h` (and headers) to your project.
- Include in your sources:  
  `#include "epc.hpp"`     (_C++ wrapper_)  
  or  
  `#include "epc_core.h"`  (_Low-level C API_)

### 2. Platform Notes

- Works on **Linux** (Unix sockets `/dev/nl_epc`) and **Windows** (NamedPipe `\\.\pipe\nl_epc`).  
- No extra dependencies except `pthread` (common on POSIX systems).
- Server process must be started first.

### 3. Compile

#### Linux

```sh
g++ -std=c++11 -pthread -o server server.cpp
g++ -std=c++11 -pthread -o client client.cpp
./server  # In one terminal
./client  # In another
```

#### Windows

- Build with any C++ compiler and link to pthreads if building on MinGW; on MSVC use Win32 default.

---

## API Overview

- **EventProcessComunication**: Main entrypoint (constructor with device name, ex: `"\\.\pipe\nl_epc"` or `"/dev/nl_epc"`).
- `.createChannel(name)`: Server creates a named channel.
- `.Connect(name)`: Client connects to a channel by name.
- `.sendString(...)`: Send a string to the current channel.
- `.sendStringToClient(client_id, msg)`: (Server) Direct send to a connected client.
- `.set_message_handler(...)`: Register callback for message receive event.
- `.set_on_connected_handler(...)`: Register handler for when connected.
- `.set_on_channel_created_handler(...)`: Handler for channel creation.
- `.set_on_error_handler(...)`: Register error handler.
- `.Disconnect()`: Disconnects client from server.
- Misc: see the examples above for idiomatic usage.

---

## Internals

- Uses platform-native IPC (`NamedPipe` for Windows, `SOCK_STREAM` on `/dev/nl_epc` for Linux).
- Threaded worker model for safe, full-duplex I/O and event delivery.
- All exchange is via a structured header (`ipc_header`) plus payload, allowing future extensibility and type/versioning.
- Supports subscriptions, direct, and broadcast message modes.

---

## License

BSD 3 License - free for personal/commercial use.

---

## Contributing

Contributions, bug-reports, and suggestions are welcome - just open an issue or PR!

---

## Author

NeuralLead developers
https://www.neurallead.com