/*
 * Memjet RIP - Direct TCP with Protocol Headers
 * 
 * Based on network trace analysis, we need to understand
 * the full protocol handshake before sending raster data.
 */

#include <iostream>
#include <vector>
#include <cstring>
#include <iomanip>
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

// Protocol constants based on network trace analysis
struct MemjetProtocol {
    // Header magic numbers (need to determine from actual AnyFlow traffic)
    static const uint32_t JOB_HEADER_MAGIC = 0x4D4A4A42; // "MJJB" - MemJet Job Begin?
    static const uint32_t PAGE_HEADER_MAGIC = 0x4D4A5048; // "MJPH" - MemJet Page Header?
    static const uint32_t DATA_CHUNK_MAGIC = 0x4D4A4443; // "MJDC" - MemJet Data Chunk?
    
    // Control codes
    static const uint8_t CMD_STATUS = 0x01;
    static const uint8_t CMD_PRINT = 0x02;
    static const uint8_t CMD_DATA = 0x03;
    static const uint8_t CMD_END = 0x04;
};

// Job header structure (inferred)
struct __attribute__((packed)) JobHeader {
    uint32_t magic;           // Magic number
    uint32_t version;         // Protocol version
    uint32_t jobId;           // Job identifier
    uint32_t totalPages;      // Number of pages
    uint32_t resolution;      // DPI (1600)
    uint32_t width;           // Page width in pixels
    uint32_t height;          // Page height in pixels
    uint32_t colorPlanes;     // Number of color planes (4 for CMYK)
    uint32_t dataSize;        // Total data size (or 0 if streaming)
    uint8_t  reserved[32];    // Reserved/padding
};

// Page header structure
struct __attribute__((packed)) PageHeader {
    uint32_t magic;
    uint32_t pageNumber;
    uint32_t width;
    uint32_t height;
    uint32_t dataSize;
    uint8_t  reserved[16];
};

class ProtocolSender {
public:
    ProtocolSender() : sockfd_(-1), connected_(false), seqNumber_(0) {}
    
    ~ProtocolSender() {
        disconnect();
    }
    
    bool connect(const std::string& ip, uint16_t port) {
        std::cout << "[PROTO] Connecting to " << ip << ":" << port << "..." << std::endl;
        
        sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd_ < 0) {
            std::cerr << "[ERROR] Socket creation failed: " << strerror(errno) << std::endl;
            return false;
        }
        
        // Set timeouts
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        setsockopt(sockfd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sockfd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        
        // Enable TCP_NODELAY for immediate sending
        int flag = 1;
        setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);
        
        if (::connect(sockfd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "[ERROR] Connection failed: " << strerror(errno) << std::endl;
            return false;
        }
        
        connected_ = true;
        std::cout << "[PROTO] Connected!" << std::endl;
        
        // Do initial handshake
        if (!doHandshake()) {
            std::cerr << "[ERROR] Handshake failed" << std::endl;
            return false;
        }
        
        return true;
    }
    
    bool sendJob(const std::vector<RasterPage>& pages, uint32_t dpi) {
        if (!connected_ || pages.empty()) {
            std::cerr << "[ERROR] Not connected or no pages" << std::endl;
            return false;
        }
        
        // Convert all pages to bilevel
        std::vector<std::vector<uint8_t>> bilevelPages;
        size_t totalDataSize = 0;
        
        BilevelConverter converter;
        
        for (const auto& page : pages) {
            std::cout << "[PROTO] Converting page " << bilevelPages.size() + 1 << "..." << std::endl;
            
            std::vector<uint8_t> bilevel = converter.convertToBilevelErrorDiffusion(
                page.data, page.width, page.height);
            
            std::vector<uint8_t> packed = converter.packToJSLFormat(
                bilevel, page.width, page.height);
            
            bilevelPages.push_back(packed);
            totalDataSize += packed.size();
        }
        
        // Send job header
        if (!sendJobHeader(pages.size(), dpi, pages[0].width, pages[0].height, 
                          4, totalDataSize)) {
            return false;
        }
        
        // Send each page
        for (size_t i = 0; i < bilevelPages.size(); i++) {
            if (!sendPage(i + 1, pages[i].width, pages[i].height, bilevelPages[i])) {
                return false;
            }
        }
        
        // Send job end
        if (!sendJobEnd()) {
            return false;
        }
        
        return true;
    }
    
    void disconnect() {
        if (sockfd_ >= 0) {
            close(sockfd_);
            sockfd_ = -1;
        }
        connected_ = false;
    }

private:
    bool doHandshake() {
        // Based on network trace: send 34 bytes, expect 4 byte ACK
        // For now, try a simple status query
        
        uint8_t statusCmd[34] = {
            MemjetProtocol::CMD_STATUS,  // Command
            0x00, 0x00, 0x00,            // Reserved
            0x00, 0x00, 0x00, 0x00,      // Sequence
            0x00, 0x00, 0x00, 0x00,      // 
            0x00, 0x00, 0x00, 0x00,      // 
            0x00, 0x00, 0x00, 0x00,      // 
            0x00, 0x00, 0x00, 0x00,      // 
            0x00, 0x00, 0x00, 0x00,      // 
            0x00, 0x00, 0x00, 0x00,      // 
            0x00, 0x00, 0x00, 0x00       // 
        };
        
        std::cout << "[PROTO] Sending handshake (34 bytes)..." << std::endl;
        
        if (!sendRaw(statusCmd, 34)) {
            return false;
        }
        
        // Try to receive ACK (might not come immediately)
        uint8_t ack[4];
        int received = recv(sockfd_, ack, 4, MSG_DONTWAIT);
        
        if (received == 4) {
            std::cout << "[PROTO] Handshake ACK: " 
                      << std::hex << std::setfill('0')
                      << std::setw(2) << (int)ack[0] << " "
                      << std::setw(2) << (int)ack[1] << " "
                      << std::setw(2) << (int)ack[2] << " "
                      << std::setw(2) << (int)ack[3] 
                      << std::dec << std::endl;
        } else {
            std::cout << "[PROTO] No immediate ACK (this might be OK)" << std::endl;
        }
        
        return true;
    }
    
    bool sendJobHeader(uint32_t totalPages, uint32_t dpi, uint32_t width, 
                       uint32_t height, uint32_t colorPlanes, size_t dataSize) {
        JobHeader header;
        memset(&header, 0, sizeof(header));
        
        header.magic = MemjetProtocol::JOB_HEADER_MAGIC;
        header.version = 0x00010000; // Version 1.0
        header.jobId = 0x00000001;
        header.totalPages = totalPages;
        header.resolution = dpi;
        header.width = width;
        header.height = height;
        header.colorPlanes = colorPlanes;
        header.dataSize = (uint32_t)dataSize;
        
        std::cout << "[PROTO] Sending job header..." << std::endl;
        
        if (!sendRaw((uint8_t*)&header, sizeof(header))) {
            return false;
        }
        
        return true;
    }
    
    bool sendPage(uint32_t pageNum, uint32_t width, uint32_t height, 
                  const std::vector<uint8_t>& data) {
        // Send page header
        PageHeader pheader;
        memset(&pheader, 0, sizeof(pheader));
        
        pheader.magic = MemjetProtocol::PAGE_HEADER_MAGIC;
        pheader.pageNumber = pageNum;
        pheader.width = width;
        pheader.height = height;
        pheader.dataSize = (uint32_t)data.size();
        
        std::cout << "[PROTO] Sending page " << pageNum << " header..." << std::endl;
        
        if (!sendRaw((uint8_t*)&pheader, sizeof(pheader))) {
            return false;
        }
        
        // Send page data
        std::cout << "[PROTO] Sending page " << pageNum << " data (" 
                  << data.size() << " bytes)..." << std::endl;
        
        if (!sendRaw(data.data(), data.size())) {
            return false;
        }
        
        return true;
    }
    
    bool sendJobEnd() {
        uint8_t endCmd[8] = {
            MemjetProtocol::CMD_END,
            0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00
        };
        
        std::cout << "[PROTO] Sending job end..." << std::endl;
        
        return sendRaw(endCmd, 8);
    }
    
    bool sendRaw(const uint8_t* data, size_t size) {
        size_t totalSent = 0;
        while (totalSent < size) {
            ssize_t sent = send(sockfd_, data + totalSent, size - totalSent, 0);
            if (sent < 0) {
                std::cerr << "[ERROR] Send failed: " << strerror(errno) << std::endl;
                return false;
            }
            totalSent += sent;
        }
        return true;
    }
    
    int sockfd_;
    bool connected_;
    uint32_t seqNumber_;
};

int main(int argc, char* argv[]) {
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
    
    std::string printerIP = args.pesIp.empty() ? "192.168.100.200" : args.pesIp;
    uint16_t printerPort = args.pesPort == 0 ? 13002 : args.pesPort;
    
    logInfo("Memjet RIP - Protocol Version");
    logInfo("Input: " + args.inputPdf);
    logInfo("Resolution: " + std::to_string(args.dpi) + " DPI");
    logInfo("Printer: " + printerIP + ":" + std::to_string(printerPort));
    
    try {
        // Rasterize
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
        
        // Send via protocol
        logInfo("Step 2: Connecting to printer...");
        
        ProtocolSender sender;
        if (!sender.connect(printerIP, printerPort)) {
            logError("Failed to connect");
            return 1;
        }
        
        logInfo("Step 3: Sending job with protocol headers...");
        
        if (!sender.sendJob(pages, args.dpi)) {
            logError("Failed to send job");
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
