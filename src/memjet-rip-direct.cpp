/*
 * Memjet RIP - Direct TCP Version
 * Bypasses JSL and uses raw TCP sockets like AnyFlow
 */

#include <iostream>
#include <vector>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <chrono>
#include <thread>

#include "pdf_rasterizer.h"
#include "bilevel_converter.h"
#include "utils.h"

using namespace memjet;
using namespace memjet::utils;

// Direct TCP sender class
class DirectTCPSender {
public:
    DirectTCPSender() : sockfd_(-1), connected_(false) {}
    
    ~DirectTCPSender() {
        disconnect();
    }
    
    bool connect(const std::string& ip, uint16_t port) {
        std::cout << "[TCP] Connecting to " << ip << ":" << port << "..." << std::endl;
        
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) {
            std::cerr << "[ERROR] Failed to create socket: " << strerror(errno) << std::endl;
            return false;
        }
        
        // Set timeout
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockfd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            std::cerr << "[ERROR] Invalid IP address" << std::endl;
            return false;
        }
        
        if (::connect(sockfd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[ERROR] Failed to connect: " << strerror(errno) << std::endl;
            return false;
        }
        
        connected_ = true;
        std::cout << "[TCP] Connected successfully!" << std::endl;
        return true;
    }
    
    bool sendData(const std::vector<uint8_t>& data) {
        if (!connected_) {
            std::cerr << "[ERROR] Not connected" << std::endl;
            return false;
        }
        
        std::cout << "[TCP] Sending " << data.size() << " bytes..." << std::endl;
        
        // Send all data
        size_t totalSent = 0;
        while (totalSent < data.size()) {
            ssize_t sent = send(sockfd_, data.data() + totalSent, 
                               data.size() - totalSent, 0);
            if (sent < 0) {
                std::cerr << "[ERROR] Send failed: " << strerror(errno) << std::endl;
                return false;
            }
            totalSent += sent;
        }
        
        std::cout << "[TCP] Sent " << totalSent << " bytes" << std::endl;
        return true;
    }
    
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

int main(int argc, char* argv[]) {
    // Parse command line
    CommandLineArgs args = parseCommandLine(argc, argv);
    
    if (args.inputPdf.empty()) {
        logError("No input PDF specified");
        printUsage(argv[0]);
        return 1;
    }
    
    if (!fileExists(args.inputPdf)) {
        logError("Input file not found: " + args.inputPdf);
        return 1;
    }
    
    // Use discovered IP from network trace
    std::string printerIP = args.pesIp.empty() ? "192.168.100.200" : args.pesIp;
    uint16_t printerPort = args.pesPort == 0 ? 13002 : args.pesPort;
    
    logInfo("Memjet RIP - Direct TCP Version (No JSL)");
    logInfo("Input: " + args.inputPdf);
    logInfo("Resolution: " + std::to_string(args.dpi) + " DPI");
    logInfo("Printer: " + printerIP + ":" + std::to_string(printerPort));
    
    try {
        // Step 1: Rasterize PDF
        logInfo("Step 1: Rasterizing PDF...");
        
        PDFRasterizer rasterizer;
        if (!rasterizer.initialize()) {
            logError("Failed to initialize rasterizer");
            return 1;
        }
        
        RasterParams rasterParams;
        rasterParams.dpi = args.dpi;
        rasterParams.pageNumber = args.pageNumber;
        rasterParams.paperSize = args.paperSize;
        
        std::vector<RasterPage> pages = rasterizer.rasterize(args.inputPdf, rasterParams);
        
        if (pages.empty()) {
            logError("No pages rasterized");
            return 1;
        }
        
        logInfo("Rasterized " + std::to_string(pages.size()) + " page(s)");
        
        // Step 2: Convert to bilevel
        logInfo("Step 2: Converting to bilevel...");
        
        BilevelConverter converter;
        std::vector<uint8_t> allBilevelData;
        
        for (const auto& page : pages) {
            std::vector<uint8_t> bilevel = converter.convertToBilevelErrorDiffusion(
                page.data, page.width, page.height);
            
            std::vector<uint8_t> packed = converter.packToJSLFormat(
                bilevel, page.width, page.height);
            
            allBilevelData.insert(allBilevelData.end(), packed.begin(), packed.end());
            
            logInfo("Packed " + std::to_string(packed.size()) + " bytes");
        }
        
        // Step 3: Connect and send via direct TCP
        logInfo("Step 3: Connecting to printer...");
        
        DirectTCPSender sender;
        if (!sender.connect(printerIP, printerPort)) {
            logError("Failed to connect to printer");
            return 1;
        }
        
        logInfo("Step 4: Sending print data...");
        
        if (!sender.sendData(allBilevelData)) {
            logError("Failed to send print data");
            return 1;
        }
        
        sender.disconnect();
        
        logInfo("Print job sent successfully!");
        logInfo("Done!");
        return 0;
        
    } catch (const std::exception& e) {
        logError(std::string("Exception: ") + e.what());
        return 1;
    }
}
