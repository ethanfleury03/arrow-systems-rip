#ifndef PES_ORCHESTRATOR_H
#define PES_ORCHESTRATOR_H

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <cstdint>
#include "jsl_wrapper.h"

namespace memjet {

// PES Engine State enumeration
enum class PesEngineState {
    UNKNOWN,
    OFF,
    INITIALISING,
    PRIMED_IDLE,
    DEPRIMED_IDLE,  // Added per Memjet docs
    PREPARING,
    PRE_JOB,
    PRINT_READY,
    PRINTING,
    MID_JOB,
    PAUSED,
    SESSION_COMPLETE,
    SHUTTING_DOWN,
    FAULT
};

// Print Session Phase enumeration
enum class SessionPhase {
    IDLE,
    DATA_SUBMITTING,
    WAITING_QUEUE,
    PREPARING,
    WAITING_PRINT_READY,
    STARTING,
    PRINTING,
    FINISHING,
    COMPLETE,
    FAILED
};

// PES Status structure
struct PesStatus {
    PesEngineState state;
    int queueLen;
    std::string queueHeadJobId;
    std::string raw;
    std::string extraction;  // "attribute" or "fallback_regex"
    bool isReadyForPrintData;  // Ready flag from PES
    std::string stateAfter;  // Latest parsed state_after string (source of truth)
    int engineStatusNumeric;  // Raw numeric state if available
};

// PES Orchestrator class
class PesOrchestrator {
public:
    PesOrchestrator(const std::string& thriftControllerPath,
                    const std::string& pesIp,
                    int controlPort);
    
    ~PesOrchestrator();
    
    // Engine readiness
    bool ensureEngineReady(int timeoutSec = 30);
    
    // Status polling
    PesStatus getStatus();
    bool waitForState(PesEngineState target, int timeoutMs);
    bool waitForQueueReady(int timeoutMs);  // C2: flexible gate
    
    // Distinct phase wait functions (per Memjet docs)
    bool waitEngineReadyForSubmit(int timeoutMs);
    bool waitJobQueuedAfterSubmit(int timeoutMs);
    bool waitEnginePrintReadyAfterPrepare(int timeoutMs);
    bool waitSessionCompleteAfterStart(int timeoutMs);
    bool waitIdleAfterFinish(int timeoutMs);
    
    // State-guarded commands
    bool guardedPrepare(int mediaSpeed);
    bool guardedStart();  // C3: strict PRINT_READY gate
    bool guardedFinish();
    
    // Thread management
    bool timedJoin(std::thread& t, int timeoutMs);  // C4: hard timeout
    
    // Main orchestration flow
    bool runPrintSession(JSLWrapper& jsl,
                        const JobConfig& config,
                        const std::vector<ColorPlane>& colors,
                        const std::vector<PageData>& planes,
                        bool legacy);
    
    // Phase tracking
    SessionPhase currentPhase() const { return phase_; }
    
    // Configuration
    static const int DATA_THREAD_TIMEOUT = 120000;  // 120s default, env-overridable

private:
    std::string runThriftCmd(const std::string& command);
    PesEngineState parseEngineState(const std::string& stateStr);
    PesEngineState parseEngineStateFromNumeric(int numericState);
    
    // Helper to extract and normalize state from JSON
    std::string extractAndNormalizeState(const PesStatus& st);
    
    // Enhanced logging with all required fields
    void logOrchestrationStep(const std::string& step, const std::string& stateBefore,
                             const std::string& stateAfter, bool isReadyForPrintData,
                             int queueLen, const std::string& result,
                             const std::string& errorCode, int timeoutMs);
    
    std::string thriftControllerPath_;
    std::string pesIp_;
    int controlPort_;
    SessionPhase phase_;
    std::atomic<bool> dataDone_;
};

} // namespace memjet

#endif // PES_ORCHESTRATOR_H
