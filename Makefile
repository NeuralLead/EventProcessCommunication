ARCH=x86_64
COMPILATION_MODE=Release

# Targets
TARGET_SERVER = bin/Linux/$(ARCH)/$(COMPILATION_MODE)/server_sample
TARGET_CLIENT = bin/Linux/$(ARCH)/$(COMPILATION_MODE)/client_sample
TARGET_MODULE = bin/Linux/$(ARCH)/$(COMPILATION_MODE)/epc_module

# Source paths
SERVER_SRC = EventProcessCommunication.Sample.Server/EventProcessCommunication.Sample.Server.cpp
CLIENT_SRC = EventProcessCommunication.Sample.Client/EventProcessCommunication.Sample.Client.cpp
MODULE_SRC = EventProcessCommunication.Service/epc_core.c EventProcessCommunication.Service/EventProcessCommunication.Service.c

# Include paths
INCLUDE_LIB = -I EventProcessCommunication.Library/
INCLUDE_SERVICE = -I EventProcessCommunication.Service/

# Compiler and flags
CXX = g++
CC = gcc
CFLAGS = -lpthread -g # -O2 ?

# Kernel module (if needed)
obj-m += epc_module.o

all: prepare_dirs module server client
	@echo "Building completed."

server:
	$(CXX) $(INCLUDE_LIB) -o $(TARGET_SERVER) $(CFLAGS) $(SERVER_SRC)

client:
	$(CXX) $(INCLUDE_LIB) -o $(TARGET_CLIENT) $(CFLAGS) $(CLIENT_SRC)

module:
	$(CC) $(INCLUDE_SERVICE) -o $(TARGET_MODULE) $(CFLAGS) $(MODULE_SRC)

prepare_dirs:
	mkdir -p bin/Linux/$(ARCH)/$(COMPILATION_MODE)

clean:
	rm -f $(TARGET_SERVER) $(TARGET_CLIENT) $(TARGET_MODULE)
	rm -f *.o *.symvers *.order
	@echo "Cleaned build artifacts."
