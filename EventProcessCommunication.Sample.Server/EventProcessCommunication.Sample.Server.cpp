#include "epc.hpp"
#include <iostream>
#include <thread>
#include <chrono>

EventProcessComunication ipc(EPC_DEVICE);

void print_msg(const ipc_header& hdr, const std::vector<char>& data) {
    /*std::cout << "From: " << hdr.from_connection_id
            << " | Channel: " << hdr.channel_id;*/

    if (hdr.payload_len < 1024)
        std::cout << " | Data: " << std::string(data.begin(), data.end());

    std::cout << "\n";

    std::string ciao = "ciao";
    ipc.sendStringToClient(hdr.from_connection_id, ciao);
    
    //ipc.DisconnectClient(hdr.from_connection_id);
}

void channel_created()
{
    uint64_t channelId = ipc.getChannelId();
    
    if (channelId <= 0)
    {
        std::cout << "Error creating channel, status: " << channelId << std::endl;
        return;
    }

    std::cout << "create channel with id " << channelId << std::endl;
}

void on_server_disconnected() {
    
    std::cout << "Server disconnected" << std::endl;
}

void on_client_connected(int64_t connectionId) {

    std::cout << "A new client with id " << std::to_string(connectionId) << " is connected" << std::endl;
}

void on_client_disconnected(int64_t connectionId) {

    std::cout << "A new client with id " << std::to_string(connectionId) << " was disconnected" << std::endl;
}

void on_error(uint8_t control)
{
    std::cout << "Error when ask control type " << (int)control << std::endl;
}

int main() {

    ipc.set_on_error_handler(on_error);
    ipc.set_message_handler(print_msg);
    ipc.set_on_channel_created_handler(channel_created);

    ipc.set_on_client_connected_handler(on_client_connected);
    ipc.set_on_client_disconnected_handler(on_client_disconnected);

    while (!ipc.createChannel("custom"))
    {
        std::cout << "Cannot create the channel" << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    while (true) std::this_thread::sleep_for(std::chrono::seconds(1));
}
