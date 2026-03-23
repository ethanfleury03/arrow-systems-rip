/*
 * Direct TCP Sender for Memjet Printer
 * Bypasses JSL entirely - uses raw sockets like AnyFlow
 * 
 * Protocol discovered from network trace:
 * - Connect to 192.168.100.200:13002
 * - Send 34-byte control messages
 * - Receive 4-byte ACKs
 * - Send raster data in chunks
 */

#include <iostream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <chrono>
#include <thread>

namespace memjet {

class DirectTCPSender {
public:
    DirectTCPSender() : sockfd_(-1), connected_(false) {}
    
    ~DirectTCPSender() {
        disconnect();
    }
    
    // Connect to printer
    bool connect(const std::string& ip, uint16_t port) {
        std::cout << "[TCP] Connecting to " << ip << ":" << port << "..." << std::endl;
        
        // Create socket
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) {
            std::cerr << "[ERROR] Failed to create socket: " << strerror(errno) << std::endl;
            return false;
        }
        
        // Set timeout
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockfd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        
        // Connect
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            std::cerr << "[ERROR] Invalid IP address: " << ip << std::endl;
            return false;
        }
        
        if (::connect(sockfd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[ERROR] Failed to connect: " << strerror(errno) << std::endl;
            return false;
        }
        
        connected_ = true;
        std::cout << "[TCP] Connected successfully!" << std::endl;
        
        // Send initial handshake (34 bytes based on trace)
        if (!sendHandshake()) {
            std::cerr << "[ERROR] Handshake failed" << std::endl;
            return false;
        }
        
        return true;
    }
    
    // Send handshake (34-byte control message from trace)
    bool sendHandshake() {
        // This is the 34-byte pattern observed in the network trace
        // Based on the trace, this might be a status/heartbeat message
        uint8_t handshake[34] = {
            0x00, 0x00, 0x00, 0x00,  // Sequence/ID
            0x00, 0x00, 0x00, 0x00,  // 
            0x00, 0x00, 0x00, 0x00,  // 
            0x00, 0x00, 0x00, 0x00,  // 
            0x00, 0x00, 0x00, 0x00,  // 
            0x00, 0x00, 0x00, 0x00,  // 
            0x00, 0x00, 0x00, 0x00,  // 
            0x00, 0x00, 0x00, 0x00,  // 
            0x00, 0x00, 0x00, 0x00,  // 
            0x00, 0x00              // 
        };
        
        std::cout << "[TCP] Sending handshake (34 bytes)..." << std::endl;
        
        if (send(sockfd_, handshake, 34, 0) != 34) {
            std::cerr << "[ERROR] Failed to send handshake: " << strerror(errno) << std::endl;
            return false;
        }
        
        // Wait for 4-byte ACK
        uint8_t ack[4];
        int received = recv(sockfd_, ack, 4, 0);
        if (received != 4) {
            std::cerr << "[ERROR] Did not receive ACK (expected 4 bytes, got " << received << ")" << std::endl;
            return false;
        }
        
        std::cout << "[TCP] Received ACK: " 
                  << std::hex << (int)ack[0] << " " << (int)ack[1] << " " 
                  << (int)ack[2] << " " << (int)ack[3] << std::dec << std::endl;
        
        return true;
    }
    
    // Send raster job data
    bool sendJobData(const std::vector<uint8_t>& data) {
        if (!connected_) {
            std::cerr << "[ERROR] Not connected" << std::endl;
            return false;
        }
        
        std::cout << "[TCP] Sending job data (" << data.size() << " bytes)..." << std::endl;
        
        // Send data in chunks (based on trace showing 34, 22, 26 byte chunks)
        size_t offset = 0;
        while (offset < data.size()) {
            size_t chunkSize = std::min(size_t(34), data.size() - offset);
            
            if (send(sockfd_, data.data() + offset, chunkSize, 0) != (ssize_t)chunkSize) {
                std::cerr << "[ERROR] Failed to send chunk at offset " << offset << std::endl;
                return false;
            }
            
            // Wait for ACK
            uint8_t ack[4];
            int received = recv(sockfd_, ack, 4, 0);
            if (received != 4) {
                std::cerr << "[ERROR] Did not receive ACK for chunk at offset " << offset << std::endl;
                return false;
            }
            
            offset += chunkSize;
            
            // Progress
            if (offset % 1024 == 0 || offset == data.size()) {
                std::cout << "[TCP] Sent " << offset << "/" << data.size() << " bytes" << std::endl;
            }
        }
        
        std::cout << "[TCP] Job data sent successfully!" << std::endl;
        return true;
    }
    
    // Send test/simple data
    bool sendTestData() {
        if (!connected_) {
            std::cerr << "[ERROR] Not connected" << std::endl;
            return false;
        }
        
        // Create a simple test pattern (black square)
        std::vector<uint8_t> testData(640, 0xFF);  // 640 bytes of black
        
        return sendJobData(testData);
    }
    
    // Disconnect
    void disconnect() {
        if (sockfd_ >= 0) {
            close(sockfd_);
            sockfd_ = -1;
        }
        connected_ = false;
    }
    
    bool isConnected() const { return connected_; }

private:
    int sockfd_;
    bool connected_;
};

} // namespace memjet

// Simple test main
#ifdef DIRECT_TCP_TEST
int main(int argc, char* argv[]) {
    using namespace memjet;
    
    std::string ip = (argc > 1) ? argv[1] : "192.168.100.200";
    uint16_t port = (argc > 2) ? std::stoi(argv[2]) : 13002;
    
    DirectTCPSender sender;
    
    if (!sender.connect(ip, port)) {
        std::cerr << "Failed to connect to printer" << std::endl;
        return 1;
    }
    
    std::cout << "Connected! Sending test data..." << std::endl;
    
    if (!sender.sendTestData()) {
        std::cerr << "Failed to send test data" << std::endl;
        return 1;
    }
    
    std::cout << "Test complete!" << std::endl;
    return 0;
}
#endif
