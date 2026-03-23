/*
 * Memjet Thrift Client - Direct RPC (No JSL/SLP)
 * 
 * Implements the Apache Thrift Compact Protocol to communicate
 * directly with Kareela PES, bypassing JSL entirely.
 * 
 * Based on Wireshark capture analysis of AnyFlow traffic.
 */

#ifndef MEMJET_THRIFT_CLIENT_H
#define MEMJET_THRIFT_CLIENT_H

#include <vector>
#include <string>
#include <cstdint>
#include <sys/socket.h>
#include <netinet/in.h>

namespace memjet {

// Thrift Compact Protocol constants
namespace ThriftCompact {
    // Protocol version
    static const uint8_t PROTOCOL_ID = 0x82;
    static const uint8_t VERSION = 0x21;
    
    // Message types
    static const uint8_t CALL = 0x01;
    static const uint8_t REPLY = 0x02;
    static const uint8_t EXCEPTION = 0x03;
    static const uint8_t ONEWAY = 0x04;
    
    // Type IDs
    static const uint8_t STOP = 0x00;
    static const uint8_t TRUE_TYPE = 0x01;
    static const uint8_t FALSE_TYPE = 0x02;
    static const uint8_t I8 = 0x03;
    static const uint8_t I16 = 0x04;
    static const uint8_t I32 = 0x05;
    static const uint8_t I64 = 0x06;
    static const uint8_t DOUBLE = 0x07;
    static const uint8_t BINARY = 0x08;
    static const uint8_t LIST = 0x09;
    static const uint8_t SET = 0x0A;
    static const uint8_t MAP = 0x0B;
    static const uint8_t STRUCT = 0x0C;
}

// Job ID structure (from captured data)
struct JobId {
    uint32_t id;  // 32-bit job ID
};

// Thrift protocol encoder
class ThriftEncoder {
public:
    ThriftEncoder();
    
    // Protocol header
    void writeMessageBegin(const std::string& methodName, uint8_t msgType, int32_t seqId);
    void writeMessageEnd();
    
    // Types
    void writeStructBegin(const std::string& name);
    void writeStructEnd();
    void writeFieldBegin(const std::string& name, uint8_t type, int16_t id);
    void writeFieldEnd();
    void writeFieldStop();
    
    void writeI32(int32_t value);
    void writeI64(int64_t value);
    void writeDouble(double value);
    void writeString(const std::string& value);
    void writeBinary(const std::vector<uint8_t>& data);
    void writeBool(bool value);
    void writeByte(uint8_t value);
    
    // Varint encoding (Thrift compact)
    void writeVarint(uint64_t value);
    void writeZigzag(int64_t value);
    
    // Get encoded buffer
    const std::vector<uint8_t>& getBuffer() const { return buffer_; }
    std::vector<uint8_t>& getBuffer() { return buffer_; }
    void clear() { buffer_.clear(); }
    
private:
    std::vector<uint8_t> buffer_;
    int16_t lastFieldId_;
    std::vector<int16_t> lastFieldStack_;
};

// Thrift protocol decoder
class ThriftDecoder {
public:
    ThriftDecoder(const std::vector<uint8_t>& data);
    
    // Protocol header
    void readMessageBegin(std::string& methodName, uint8_t& msgType, int32_t& seqId);
    void readMessageEnd();
    
    // Types
    void readStructBegin(std::string& name);
    void readStructEnd();
    void readFieldBegin(std::string& name, uint8_t& type, int16_t& id);
    void readFieldEnd();
    
    int32_t readI32();
    int64_t readI64();
    double readDouble();
    std::string readString();
    std::vector<uint8_t> readBinary();
    bool readBool();
    uint8_t readByte();
    
    // Varint decoding
    uint64_t readVarint();
    int64_t readZigzag();
    
    // Check if more data available
    bool hasMoreData() const { return pos_ < data_.size(); }
    
private:
    std::vector<uint8_t> data_;
    size_t pos_;
    int16_t lastFieldId_;
};

// Kareela PES Thrift Client
class KareelaThriftClient {
public:
    KareelaThriftClient();
    ~KareelaThriftClient();
    
    // Connection
    bool connect(const std::string& ip, uint16_t port);
    void disconnect();
    bool isConnected() const { return connected_; }
    
    // Thrift RPC methods (from capture analysis)
    JobId generateJobId();
    void clearJobQueue();
    void prepareToPrint(double intendedSpeed);
    void startPrinting();
    void finishPrinting();
    void startServicing(int serviceType);
    void indexWipers(int count);
    
    // Data streaming (for print data)
    bool sendPrintData(const std::vector<uint8_t>& data);
    
    // Status
    std::string getStatus();
    
private:
    bool sendRequest(const ThriftEncoder& encoder);
    bool receiveReply(ThriftDecoder& decoder);
    bool sendRaw(const std::vector<uint8_t>& data);
    bool receiveRaw(std::vector<uint8_t>& data, size_t expectedLen);
    
    int sockfd_;
    bool connected_;
    int32_t sequenceId_;
    
    // Buffer for reading
    std::vector<uint8_t> readBuffer_;
};

} // namespace memjet

#endif // MEMJET_THRIFT_CLIENT_H
