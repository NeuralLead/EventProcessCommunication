#include "epc.hpp"
#include <iostream>
#include <thread>
#include <chrono>

EventProcessComunication ipc(EPC_DEVICE);

void print_msg(const ipc_header& hdr, const std::vector<char>& data) {
    /*std::cout << "From: " << hdr.from_connection_id
        << " | To (conn id): " << hdr.channel_id
        */

    std::cout << " | Data: " << std::string(data.begin(), data.end()) << "\n";

    std::this_thread::sleep_for(std::chrono::milliseconds(89));

    ipc.sendString(":-)", 4);
}

void on_connected() {

    uint64_t channelId = ipc.getChannelId();

    if (channelId <= 0)
    {
        std::cout << "Error connecting channel, status: " << channelId << std::endl;
        return;
    }
    
    std::cout << "Connected to server with channel Id " << channelId << std::endl;

    std::string json = R"({"channelId":123,"path":"/hello","data":{"msg":"Hello EPC!"}})";
    ipc.sendString(json);

    //return;

    while (true)
    {
        ipc.sendString("cycle", 5);
        //ipc.sendString("multi", 6);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        //std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    ipc.Disconnect();
}

void on_disconnected()
{
    std::cout << "Disconnected from server and EPC" << std::endl;
}

void on_error(uint8_t control)
{
    std::cout << "Error when ask control type " << (int)control << std::endl;
}

int main() {
    ipc.set_on_error_handler(on_error);
    ipc.set_message_handler(print_msg);
    ipc.set_on_connected_handler(on_connected);
    ipc.set_on_disconnected_handler(on_disconnected);
    
    if (!ipc.Connect("custom"))
        std::cout << "Cannot lookup the channel\n";

    //if (!ipc.Connect(channelId))
    //    std::cout << "Cannot connect\n";

    while (true);
}
