/*
 * Xitron API Sender for Memjet DuraFlex Printers
 * 
 * This module uses the Xitron Web API (HTTP-based) to communicate with
 * Navigator Server and send print jobs to Memjet DuraFlex printers.
 * 
 * Key differences from direct TCP:
 * - Uses HTTP protocol instead of raw TCP sockets
 * - Requires login/session management (SessionID)
 * - Sends ZX files via SendFileToPrinter command
 * - Communicates with Navigator Server, not directly with printer
 * 
 * Xitron Web API Reference: SDK for Duraflex 3/Xitron Web API Version 3.1.md
 * 
 * Workflow:
 * 1. Login to Navigator Server (get SessionID)
 * 2. (Optional) Query device status
 * 3. Send ZX file to printer using SendFileToPrinter
 * 4. Logout
 * 
 * COMPILATION:
 *   g++ -o xitron_sender xitron_api_sender.cpp -lcurl -ljsoncpp -std=c++11
 * 
 * TESTING:
 *   ./xitron_sender <navigator_ip> <port> <username> <password> <printer_id> <zx_file>
 * 
 * Example:
 *   ./xitron_sender 192.168.1.100 80 admin admin123 192.168.100.200 myjob.zx
 */

#include <iostream>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <memory>
#include <sstream>
#include <fstream>
#include <curl/curl.h>
#include <json/json.h>

namespace memjet {

// ============================================================================
// Xitron API Response Structure
// ============================================================================
struct XitronResponse {
    bool success;
    std::string errorMessage;
    std::string warningMessage;
    std::string infoMessage;
    Json::Value data;
    
    XitronResponse() : success(false) {}
};

// ============================================================================
// Xitron API Session Info
// ============================================================================
struct SessionInfo {
    std::string sessionId;
    int securityLevel;
    std::string productName;
    std::string version;
    std::string serverIp;
    int serverPort;
    bool isValid;
    
    SessionInfo() : securityLevel(0), serverPort(0), isValid(false) {}
};

// ============================================================================
// CURL Callback for writing response data
// ============================================================================
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// ============================================================================
// Xitron API Sender Class
// ============================================================================
class XitronAPISender {
public:
    XitronAPISender() : curl_(nullptr), serverAddress_(""), serverPort_(80), 
                        connected_(false), verbose_(false) {
        curl_global_init(CURL_GLOBAL_ALL);
    }
    
    ~XitronAPISender() {
        disconnect();
        curl_global_cleanup();
    }
    
    void setVerbose(bool verbose) { verbose_ = verbose; }
    const SessionInfo& getSessionInfo() const { return sessionInfo_; }
    bool isConnected() const { return connected_ && !sessionInfo_.sessionId.empty(); }

    bool connect(const std::string& serverIp, int port = 80) {
        serverAddress_ = serverIp;
        serverPort_ = port;
        
        std::cout << "[Xitron] Targeting Navigator Server at " 
                  << serverAddress_ << ":" << serverPort_ << std::endl;
        
        curl_ = curl_easy_init();
        if (!curl_) {
            std::cerr << "[ERROR] Failed to initialize CURL" << std::endl;
            return false;
        }
        
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, 30L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 0L);
        
        connected_ = true;
        return true;
    }
    
    bool login(const std::string& username, const std::string& password) {
        if (!curl_) {
            std::cerr << "[ERROR] Not connected to server" << std::endl;
            return false;
        }
        
        std::cout << "[Xitron] Logging in as '" << username << "'..." << std::endl;
        
        Json::Value params;
        params["username"] = username;
        params["password"] = password;
        
        std::string jsonParams = params.toStyledString();
        std::string url = buildUrl("login");
        std::string responseStr;
        
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, jsonParams.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &responseStr);
        
        CURLcode res = curl_easy_perform(curl_);
        curl_slist_free_all(headers);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);
        
        if (res != CURLE_OK) {
            std::cerr << "[ERROR] Login request failed: " << curl_easy_strerror(res) << std::endl;
            return false;
        }
        
        XitronResponse response = parseResponse(responseStr);
        
        if (!response.success) {
            std::cerr << "[ERROR] Login failed: " << response.errorMessage << std::endl;
            return false;
        }
        
        if (response.data.isMember("SessionID")) {
            sessionInfo_.sessionId = response.data["SessionID"].asString();
            sessionInfo_.securityLevel = response.data.get("SecurityLevel", 0).asInt();
            sessionInfo_.productName = response.data.get("ProductName", "").asString();
            sessionInfo_.version = response.data.get("Version", "").asString();
            sessionInfo_.serverIp = response.data.get("ServerIP", "").asString();
            sessionInfo_.serverPort = response.data.get("ServerPort", 0).asInt();
            sessionInfo_.isValid = true;
            
            std::cout << "[Xitron] Login successful!" << std::endl;
            std::cout << "[Xitron] Session ID: " << sessionInfo_.sessionId << std::endl;
            std::cout << "[Xitron] Server: " << sessionInfo_.productName 
                      << " v" << sessionInfo_.version << std::endl;
            
            return true;
        }
        
        std::cerr << "[ERROR] Login response missing SessionID" << std::endl;
        return false;
    }
    
    bool logout() {
        if (!isConnected()) return true;
        
        std::cout << "[Xitron] Logging out..." << std::endl;
        
        Json::Value params;
        params["sessionid"] = sessionInfo_.sessionId;
        
        XitronResponse response = sendGetRequest("logout", params);
        
        sessionInfo_ = SessionInfo();
        connected_ = false;
        
        if (curl_) {
            curl_easy_cleanup(curl_);
            curl_ = nullptr;
        }
        
        return response.success;
    }
    
    bool sendFileToPrinter(const std::string& zxFilename, const std::string& printerId) {
        if (!connected_) {
            std::cerr << "[ERROR] Not connected to server" << std::endl;
            return false;
        }
        
        // Remove .ZX extension if present
        std::string baseFilename = zxFilename;
        size_t extPos = baseFilename.rfind(".ZX");
        if (extPos == std::string::npos) {
            extPos = baseFilename.rfind(".zx");
        }
        if (extPos != std::string::npos && extPos == baseFilename.length() - 3) {
            baseFilename = baseFilename.substr(0, extPos);
        }
        
        std::cout << "[Xitron] Sending ZX file '" << baseFilename 
                  << "' to printer " << printerId << "..." << std::endl;
        
        Json::Value params;
        params["filename"] = baseFilename;
        params["printer_id"] = printerId;
        
        XitronResponse response = sendGetRequest("sendfiletoprinter", params);
        
        if (response.success) {
            std::cout << "[Xitron] File queued successfully!" << std::endl;
            return true;
        } else {
            std::cerr << "[ERROR] Failed to send file: " << response.errorMessage << std::endl;
            return false;
        }
    }
    
    void disconnect() {
        if (isConnected()) {
            logout();
        }
    }

private:
    CURL* curl_;
    std::string serverAddress_;
    int serverPort_;
    bool connected_;
    bool verbose_;
    SessionInfo sessionInfo_;
    
    std::string buildUrl(const std::string& command) {
        std::ostringstream url;
        url << "http://" << serverAddress_ << ":" << serverPort_ << "/" << command;
        return url.str();
    }
    
    XitronResponse sendGetRequest(const std::string& command, const Json::Value& params) {
        XitronResponse response;
        
        if (!curl_) {
            response.errorMessage = "Not connected";
            return response;
        }
        
        Json::StreamWriterBuilder builder;
        std::string jsonParams = Json::writeString(builder, params);
        
        // URL-encode the JSON params
        std::string encodedParams = curl_easy_escape(curl_, jsonParams.c_str(), jsonParams.length());
        
        std::string url = buildUrl(command) + "?params=" + encodedParams + "&callback=?";
        std::string responseStr;
        
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &responseStr);
        
        CURLcode res = curl_easy_perform(curl_);
        curl_free((void*)encodedParams.c_str());
        
        if (res != CURLE_OK) {
            response.errorMessage = curl_easy_strerror(res);
            return response;
        }
        
        return parseResponse(responseStr);
    }
    
    XitronResponse parseResponse(const std::string& responseStr) {
        XitronResponse response;
        
        Json::CharReaderBuilder builder;
        Json::CharReader* reader = builder.newCharReader();
        Json::Value root;
        std::string errors;
        
        bool parsingSuccessful = reader->parse(
            responseStr.c_str(), 
            responseStr.c_str() + responseStr.size(), 
            &root, 
            &errors
        );
        delete reader;
        
        if (!parsingSuccessful) {
            response.errorMessage = "JSON parse error: " + errors;
            return response;
        }
        
        response.data = root;
        
        // Check for Message object
        if (root.isMember("Message")) {
            const Json::Value& msg = root["Message"];
            if (msg.isMember("Error")) {
                response.errorMessage = msg["Error"].asString();
            }
            if (msg.isMember("Warning")) {
                response.warningMessage = msg["Warning"].asString();
            }
            if (msg.isMember("Info")) {
                response.infoMessage = msg["Info"].asString();
            }
        }
        
        // Success if no error message
        response.success = response.errorMessage.empty();
        
        return response;
    }
};

} // namespace memjet

// ============================================================================
// Main - Example usage
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 7) {
        std::cerr << "Usage: " << argv[0] << " <navigator_ip> <port> <username> <password> <printer_id> <zx_file>" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Example:" << std::endl;
        std::cerr << "  " << argv[0] << " 192.168.1.100 80 admin admin123 192.168.100.200 myjob.zx" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Note: The ZX file must already exist in the Navigator Server's file system." << std::endl;
        std::cerr << "      This command only sends a print command to the printer." << std::endl;
        return 1;
    }
    
    std::string navigatorIp = argv[1];
    int port = std::stoi(argv[2]);
    std::string username = argv[3];
    std::string password = argv[4];
    std::string printerId = argv[5];
    std::string zxFile = argv[6];
    
    memjet::XitronAPISender sender;
    sender.setVerbose(true);
    
    // Connect to Navigator Server
    if (!sender.connect(navigatorIp, port)) {
        std::cerr << "[FATAL] Failed to connect to Navigator Server" << std::endl;
        return 1;
    }
    
    // Login
    if (!sender.login(username, password)) {
        std::cerr << "[FATAL] Login failed" << std::endl;
        return 1;
    }
    
    // Send file to printer
    bool success = sender.sendFileToPrinter(zxFile, printerId);
    
    // Logout
    sender.logout();
    
    return success ? 0 : 1;
}
