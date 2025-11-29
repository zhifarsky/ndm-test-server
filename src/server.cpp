#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>

typedef uint8_t u8;
typedef int8_t i8;
typedef uint16_t u16;
typedef int16_t i16;
typedef uint32_t u32;
typedef int32_t i32;
typedef uint64_t u64;
typedef int64_t i64;

#define StringsEqual(strA, strB) (strcmp((strA), (strB)) == 0)

#define WAIT_INFINITE -1
#define TEMP_BUFFER_SIZE 4 * 1024
#define MAX_EVENTS 64
#define MAX_PENDING 64

//
// Server state
//

struct server_state {
    i32 epoll, socket;
    u64 clientsConnected, totalClientsConnected;
    bool isRunning;
};

void DisconnectClient(server_state* server, i32 clientSocket) {
    if (epoll_ctl(server->epoll, EPOLL_CTL_DEL, clientSocket, NULL) != -1) {
        close(clientSocket);
        server->clientsConnected--;
        printf("Client disconnected\n");
    }
    else {
        perror("epoll_ctl(EPOLL_CTL_DEL)");
    }
}

//
// Dynamic buffer
//

// нужно обнулить для инициализации или использовать DynamicBuffer()
struct dynamic_buffer {
    u8* base;
    u64 size, capacity;
};

dynamic_buffer DynamicBuffer(u64 capacity) {
    dynamic_buffer buffer;
    buffer.base = (u8*)malloc(capacity);
    buffer.capacity = capacity;
    buffer.size = 0;
    return buffer;
}

#define PushStruct(buffer, value) Push(buffer, &(value), sizeof(value))
#define PushArray(buffer, array, count) Push(buffer, (array), (count) * sizeof(*(array)))
void Push(dynamic_buffer* buffer, void* data, u64 size) {
    if (size == 0)
        return;
    
    if (buffer->size + size > buffer->capacity) {
        buffer->capacity = (buffer->capacity + size) * 2;
        buffer->base = (u8*)realloc(buffer->base, buffer->capacity);
    }
    
    memcpy(buffer->base + buffer->size, data, size);
    buffer->size += size;
}

void Clear(dynamic_buffer* buffer) { buffer->size = 0; }


void ProcessMessage(server_state* server, dynamic_buffer* inBuffer, dynamic_buffer* outBuffer) {
    char* msg = (char*)inBuffer->base;
    char buf[1024];
    
    if (inBuffer->size > 0 && inBuffer->base[0] == '/') { 
        // /time
        if (StringsEqual("/time", msg)) {
            time_t rawTime;
            time(&rawTime);

            // tm* UTCTime = gmtime(&rawTime);
            tm* localTime = localtime(&rawTime);

            size_t size = strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localTime);
            PushArray(outBuffer, buf, size + 1);
        }

        // /stats
        else if (StringsEqual("/stats", msg)) {
            size_t size = snprintf(buf, sizeof(buf), 
                "Clients connected: %ld, total clients connected %ld", 
                server->clientsConnected, server->totalClientsConnected);
            PushArray(outBuffer, buf, size + 1);
        }
    
        // /shutdown
        else if (StringsEqual("/shutdown", msg)) {
            server->isRunning = false;
        }
        
        // команда не найдена
        else {
            char response[] = "Unknown command.";
            PushArray(outBuffer, response, strlen(response) + 1);
        }
    }
    
    // зеркалируем
    else {
        Push(outBuffer, inBuffer->base, inBuffer->size);
    }
}

i32 TCPServer(u16 port) {
    printf("TCP server: port %d\n", port);

    //
    // Create epoll
    //

    server_state server = {0};
    server.epoll = epoll_create(1); // агрумент игнорируется, но должен быть больше 0

    if (server.epoll != -1) {
        
        //
        // Create server socket
        //

        server.socket = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0); // SOCK_STREAM - TCP, SOCK_DGAM - UDP

        if (server.socket != -1) {
            i32 option = 1;
            // переиспользовать адрес, чтобы избежать ошибки EADDRINUSE в bind()
            if (setsockopt(server.socket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) != -1) { 
                sockaddr_in serverAddress = {0};
                serverAddress.sin_family = AF_INET;
                serverAddress.sin_addr.s_addr = INADDR_ANY;
                serverAddress.sin_port = htons(port);

                if (bind(server.socket, (sockaddr*)&serverAddress, sizeof(serverAddress)) != -1) {
                    if (listen(server.socket, MAX_PENDING) != -1) {
                        epoll_event event;
                        event.events = EPOLLIN;
                        event.data.fd = server.socket;

                        if (epoll_ctl(server.epoll, EPOLL_CTL_ADD, server.socket, &event) != -1) {
                            dynamic_buffer inBuffer = DynamicBuffer(1024); 
                            dynamic_buffer outBuffer = DynamicBuffer(1024);

                            server.isRunning = true;

                            while (server.isRunning) {
                                
                                //
                                // Wait for events
                                //
                                
                                printf("Waiting for clients...\n");
                                
                                epoll_event events[MAX_EVENTS];
                                i32 eventsCount = epoll_wait(server.epoll, events, MAX_EVENTS, WAIT_INFINITE);

                                for (i32 i = 0; i < eventsCount; i++)
                                {
                                    epoll_event* event = &events[i];
                                    Clear(&inBuffer);
                                    Clear(&outBuffer);

                                    //
                                    // Connect new clients
                                    //

                                    if (event->data.fd == server.socket) {
                                        while (true) {
                                            sockaddr_in clientAddress = {0};
                                            u32 clientAddressSize = sizeof(clientAddress);

                                            i32 clientSocket = accept(server.socket, (sockaddr*)&clientAddress, &clientAddressSize);
                                            if (clientSocket != -1) {
                                                
                                                //
                                                // Set connection as non-blocking
                                                //
                                                
                                                i32 flags = fcntl(clientSocket, F_GETFL, 0);
                                                if (flags != -1) {
                                                    if (fcntl(clientSocket, F_SETFL, flags | O_NONBLOCK) != -1) {
                                                        
                                                        //
                                                        // Add connection to interest list
                                                        //

                                                        epoll_event event;
                                                        event.events = EPOLLIN; // read
                                                        event.data.fd = clientSocket;

                                                        if (epoll_ctl(server.epoll, EPOLL_CTL_ADD, clientSocket, &event) != -1) {
                                                            printf("Listening from IP: %s, Port: %d\n",
                                                                inet_ntoa(clientAddress.sin_addr), ntohs(clientAddress.sin_port));

                                                            server.clientsConnected++;
                                                            server.totalClientsConnected++;
                                                        }
                                                        else {
                                                            perror("epoll_ctl(EPOLL_CTL_ADD)");
                                                        }
                                                    }
                                                    else {
                                                        perror("fcntl(F_SETFL)");
                                                    }
                                                }
                                                else {
                                                    perror("fcntl(F_GETFL)");
                                                }
                                            }
                                            else {
                                                // все соединения обработаны
                                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                                    break;
                                                } 
                                                else {
                                                    perror("accept()");
                                                    break;
                                                }
                                            }
                                        }
                                    }

                                    //
                                    // Process client requests
                                    //

                                    else if (event->events == EPOLLIN) {
                                        
                                        //
                                        // Read from client
                                        //
                                                                            
                                        char tempBuffer[TEMP_BUFFER_SIZE];
                                        u64 bytesRecievedTotal = 0;
                                        
                                        // читаем пока данные доступны (если не вместились в буфер)
                                        while (true) {
                                            ssize_t bytesRecieved = read(event->data.fd, tempBuffer, sizeof(tempBuffer));
                                            
                                            if (bytesRecieved > 0) {
                                                Push(&inBuffer, tempBuffer, bytesRecieved);
                                                bytesRecievedTotal += bytesRecieved;
                                            }
                                            // клиент отключился
                                            else if (bytesRecieved == 0) {
                                                DisconnectClient(&server, event->data.fd);
                                                break;
                                            }
                                            else {
                                                // все данные прочитаны 
                                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                                    printf("Total bytes recieved: %lu\n", bytesRecievedTotal);
                                                    
                                                    char nullTerminator = 0;
                                                    PushStruct(&inBuffer, nullTerminator);
                                                    
                                                    //
                                                    // Process command
                                                    //

                                                    ProcessMessage(&server, &inBuffer, &outBuffer);
                                               
                                                    //
                                                    // Write to client
                                                    //
                                                    
                                                    if (server.isRunning) {
                                                        if (send(event->data.fd, outBuffer.base, outBuffer.size, MSG_NOSIGNAL) != -1) {
                                                            printf("%ld bytes sent\n", outBuffer.size);
                                                        }
                                                        else {
                                                            // EPIPE - соединение закрыто
                                                            // ECONNRESET - соединение разорвано
                                                            if (errno == EPIPE || errno == ECONNRESET) {
                                                                DisconnectClient(&server, event->data.fd);
                                                            }
                                                            else {
                                                                perror("send()");
                                                            }
                                                        }
                                                    }
                                                    
                                                    break;
                                                }
                                                else {
                                                    perror("read()");
                                                    break;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        else {
                            perror("epoll_ctl()");
                        }
                    }
                    else {
                        perror("listen()");
                    }
                }
                else {
                    perror("bind()");
                }
            }
            else {
                perror("setsockopt()");
            }
            
            close(server.socket);
        }
        else {
            perror("socket()");
        }
        
        close(server.epoll);
    } 
    else {
        perror("epoll_create()");
    }

    printf("Server shutdown\n");
    return EXIT_SUCCESS;
}

i32 UDPServer(u16 port) {
    printf("UDP server: port %d\n", port);

    //
    // Create epoll
    //

    server_state server = {0};
    server.epoll = epoll_create(1); // агрумент игнорируется, но должен быть больше 0

    if (server.epoll != -1) {
        
        //
        // Create server socket
        //

        server.socket = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0); // SOCK_STREAM - TCP, SOCK_DGAM - UDP

        if (server.socket != -1) {
            i32 option = 1;
            // переиспользовать адрес, чтобы избежать ошибки EADDRINUSE в bind()
            if (setsockopt(server.socket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option)) != -1) {
                sockaddr_in serverAddress = {0};
                serverAddress.sin_family = AF_INET;
                serverAddress.sin_addr.s_addr = INADDR_ANY;
                serverAddress.sin_port = htons(port);
                
                if (bind(server.socket, (sockaddr*)&serverAddress, sizeof(serverAddress)) != -1) {
                    epoll_event event;
                    event.events = EPOLLIN;
                    event.data.fd = server.socket;
                    
                    if (epoll_ctl(server.epoll, EPOLL_CTL_ADD, server.socket, &event) != -1) {
                        dynamic_buffer inBuffer = DynamicBuffer(1024); 
                        dynamic_buffer outBuffer = DynamicBuffer(1024);
                        
                        server.isRunning = true;
                        
                        while (server.isRunning) {
                            
                            //
                            // Wait for events
                            //
                            
                            printf("Waiting for clients...\n");
                            
                            epoll_event events[MAX_EVENTS];
                            i32 eventsCount = epoll_wait(server.epoll, events, MAX_EVENTS, WAIT_INFINITE);
                            
                            //
                            // Process client requests
                            //
                            
                            for (i32 i = 0; i < eventsCount; i++)
                            {
                                epoll_event* event = &events[i];
                                Clear(&inBuffer);
                                Clear(&outBuffer);
                                
                                char tempBuffer[TEMP_BUFFER_SIZE];
                                u64 bytesRecievedTotal = 0;

                                sockaddr_in clientAddress = {0};
                                socklen_t clientAddressSize = sizeof(clientAddress);
                                
                                server.clientsConnected++;
                                server.totalClientsConnected++;
                                
                                // читаем пока данные доступны (если не вместились в буфер)
                                while(true) {
                                    
                                    //
                                    // Read from client
                                    //
                                    
                                    ssize_t bytesRecieved = recvfrom(
                                        event->data.fd, tempBuffer, sizeof(tempBuffer), 
                                        0,
                                        (sockaddr*)&clientAddress, &clientAddressSize);

                                    printf("Received message from client IP: %s, Port: %d\n",
                                        inet_ntoa(clientAddress.sin_addr), ntohs(clientAddress.sin_port));

                                    if (bytesRecieved > 0) {
                                        Push(&inBuffer, tempBuffer, bytesRecieved);
                                        bytesRecievedTotal += bytesRecieved;
                                    }
                                    else if (bytesRecieved == 0) {
                                        printf("Client disconnected\n");
                                        break;
                                    }
                                    else {
                                        // все данные прочитаны 
                                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                            printf("Total bytes recieved: %lu\n", bytesRecievedTotal);
                                            
                                            char nullTerminator = 0;
                                            PushStruct(&inBuffer, nullTerminator);
                                            
                                            //
                                            // Process command
                                            //
                                            
                                            ProcessMessage(&server, &inBuffer, &outBuffer);
                                            
                                            //
                                            // Write to client
                                            //
                                            
                                            if (server.isRunning) {
                                                if (sendto(server.socket, 
                                                    outBuffer.base, outBuffer.size, 
                                                    MSG_CONFIRM, // пометить как ответ на запрос
                                                    (sockaddr*)&clientAddress, sizeof(clientAddress)) != -1)  
                                                {
                                                    printf("%ld bytes sent\n", outBuffer.size);
                                                }
                                                else {
                                                    perror("send()");
                                                }
                                            }
                                            break;
                                        }
                                        else {
                                            perror("recvfrom()");
                                            break;
                                        }
                                    }
                                }
                                
                                server.clientsConnected--;
                            }
                        }
                    }
                    else {
                        perror("epoll_ctl()");
                    }
                }
                else {
                    perror("bind()");
                }
            }
            else {
                perror("setsockopt()");
            }
            
            close(server.socket);
        }
        else {
            perror("socket()");
        }
        
        close(server.epoll);
    }
    else {
        perror("epoll_create()");
    }

    printf("Server shutdown\n");
    return EXIT_SUCCESS;
}

// i32 main(i32 argc, char* argv[]) {
//     u16 port = 8080;
    
//     // port
//     if (argc > 2) {
//         u16 portArg = atoi(argv[2]);
//         if (portArg) port = portArg;
//     }

//     // protocol
//     if (argc > 1) {
//         char* protocol = argv[1];
        
//         if (StringsEqual(protocol, "tcp"))      return TCPServer(port);
//         if (StringsEqual(protocol, "udp"))      return UDPServer(port);
        
//         printf("Unknown protocol: \"%s\". Using default protocol.\n", protocol);
//         return TCPServer(port);
//     }
//     else {
//         printf("usage: ndm-test-server [protocol] [port]\n");
//         return EXIT_SUCCESS;
//     }
// }

enum server_protocol {
    Protocol_None = 0,
    Protocol_UDP,
    Protocol_TCP,
};

struct server_config {
    server_protocol protocol;
    u16 port;
};

server_config ParseConfig(const char* path) {    
    server_config config;
    memset(&config, 0, sizeof(config));
    
    FILE* file = fopen(path, "rt");
    if (file) {
        char line[256];
        
        while (fgets(line, sizeof(line), file)) {
            line[strcspn(line, "\n")] = 0;
            
            if (StringsEqual(line, "protocol=tcp"))
                config.protocol = Protocol_TCP;
            else if (StringsEqual(line, "protocol=udp"))
                config.protocol = Protocol_UDP;
            else 
                sscanf(line, "port=%hd", &config.port);
        }
        
        fclose(file);
    }
    else {
        printf("Unable to open config file.\n");
    }
    
    if (config.protocol == Protocol_None) {
        printf("No protocol specified. Using default protocol.\n");
        config.protocol = Protocol_TCP;
    }
    if (config.port == 0) {
        printf("No port specified. Using default port.\n");
        config.port = 8080;
    }
    
    return config;
}

i32 main(i32 argc, char* argv[]) {
    if (argc > 2) {
        if (StringsEqual(argv[1], "--config")) {
            const char* configPath = argv[2];
            server_config config = ParseConfig(configPath);
            switch (config.protocol)
            {
            case Protocol_TCP:
                return TCPServer(config.port);
            case Protocol_UDP:
                return UDPServer(config.port);
            default:
                printf("Unknown protocol. Using default protocol.\n");
                return TCPServer(config.port);
            }
        }
    }
 
    printf("usage: ndm-test-server --config <config path>\n");
    return EXIT_SUCCESS;
}