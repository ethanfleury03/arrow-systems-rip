#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Real Kareela PES Thrift command runner (Python 2.7 compatible).

Usage:
  python thrift_controller_fullcycle.py <printer_ip> <control_port> <command1> [command2 ...]

Commands:
  clear
  initialise
  prepare[=<media_speed>[=<timeout_sec>]]   (retries on transient not-ready, default speed=0 timeout=12s)
  start
  finish
  shutdown
  status
  statusjson                  (machine-parseable JSON status)
  waitstate=<state>[=<timeout>]  (wait for engine state, default timeout 30s)
  pulse=<multiplier>            (storeSettings: pulseWidthCustMultiplier, e.g. 0.95)
  sleep=<seconds>
  waitready[=<timeout_sec>]     (polls getStatus)
  fullcycle[=<media_speed>]     (clear -> initialise -> prepare -> start)
  endcycle                       (finish -> shutdown)
"""

import os
import sys
import time
import traceback
import json
import re

try:
    integer_types = (int, long)
except NameError:
    integer_types = (int,)


def _setup_import_paths():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    candidates = []

    env_root = os.environ.get('PDL_THRIFT_ROOT')
    if env_root:
        env_root = env_root.strip().strip("\"").strip("'")
        candidates.append(env_root)

    candidates.append(script_dir)
    candidates.append(os.path.abspath(os.path.join(script_dir, 'pdl-source', 'PDL', 'MJ6.5.0-2.el7')))
    candidates.append(os.path.abspath(os.path.join(script_dir, '..', 'pdl-source', 'PDL', 'MJ6.5.0-2.el7')))

    for root in candidates:
        if not root:
            continue
        memjet_dir = os.path.join(root, 'memjet')
        Memjet_dir = os.path.join(root, 'Memjet')
        thrift_dir = os.path.join(root, 'thrift')
        if (os.path.isdir(memjet_dir) or os.path.isdir(Memjet_dir)) and os.path.isdir(thrift_dir):
            if root not in sys.path:
                sys.path.insert(0, root)
            return root
    return None


def _parse_prepare_speed(token):
    parts = token.split('=', 1)
    if len(parts) == 2 and parts[1] != '':
        try:
            return int(parts[1])
        except ValueError:
            raise ValueError("Invalid prepare speed '%s'" % parts[1])
    return 0


def _parse_prepare_args(token):
    """Parse prepare token formats:
      prepare
      prepare=<speed>
      prepare=<speed>=<timeout_sec>
    """
    parts = token.split('=')
    speed = 0
    timeout_sec = 12

    if len(parts) >= 2 and parts[1] != '':
        try:
            speed = int(parts[1])
        except ValueError:
            raise ValueError("Invalid prepare speed '%s'" % parts[1])

    if len(parts) >= 3 and parts[2] != '':
        try:
            timeout_sec = int(parts[2])
        except ValueError:
            raise ValueError("Invalid prepare timeout '%s'" % parts[2])

    if timeout_sec < 1:
        timeout_sec = 1

    return speed, timeout_sec


def _parse_value(token, name, default_value):
    parts = token.split('=', 1)
    if len(parts) == 2 and parts[1] != '':
        try:
            return int(parts[1])
        except ValueError:
            raise ValueError("Invalid %s '%s'" % (name, parts[1]))
    return default_value


def _status_to_text(status_obj):
    try:
        return str(status_obj)
    except Exception:
        return repr(status_obj)


def _looks_ready(status_obj):
    txt = _status_to_text(status_obj).lower()
    bad_words = ['fault', 'error', 'fatal', 'jam', 'offline']
    good_words = ['ready', 'idle', 'prepared', 'initialised', 'initialized']
    for w in bad_words:
        if w in txt:
            return False
    for w in good_words:
        if w in txt:
            return True
    return False


def _engine_state_name(state_value):
    """Convert Thrift engine state enum value to string name."""
    # Map common state values to names
    state_map = {
        0: "OFF",
        1: "INITIALISING",
        2: "PRIMED_IDLE",
        3: "PREPARING",
        4: "PRE_JOB",
        5: "PRE_JOB",
        6: "PRINT_READY",
        7: "PRINTING",
        8: "MID_JOB",
        9: "PAUSED",
        10: "SESSION_COMPLETE",
        11: "SHUTTING_DOWN",
        12: "FAULT"
    }
    if isinstance(state_value, integer_types):
        return state_map.get(state_value, "UNKNOWN")
    # If it's already a string or enum name, try to extract
    state_str = str(state_value).upper()
    for _, name in state_map.items():
        if name in state_str:
            return name
    return "UNKNOWN"


def _status_to_json(status_obj):
    """Extract machine-parseable fields -- attribute access first, repr parse as fallback (C1)."""
    result = {
        "raw": str(status_obj),
        "engineState": "UNKNOWN",
        "state_after": "UNKNOWN",  # Normalized uppercase state for C++ parser
        "queueLen": 0,
        "queueHeadJobId": "",
        "isReadyForPrintData": False,  # Ready flag from PES
        "extraction": "attribute"
    }
    
    try:
        # Primary path: direct Thrift attribute access (C1)
        if hasattr(status_obj, 'engineState'):
            result["engineState"] = _engine_state_name(status_obj.engineState)
        elif hasattr(status_obj, 'state'):
            result["engineState"] = _engine_state_name(status_obj.state)
        
        # Normalize engine state (Python 2-safe)
        engine_state = result.get("engineState", "UNKNOWN")
        if engine_state is None:
            engine_state = "UNKNOWN"
        if not isinstance(engine_state, basestring):
            engine_state = str(engine_state)
        engine_state = engine_state.strip().upper() or "UNKNOWN"
        result["engineState"] = engine_state
        result["state_after"] = engine_state
        
        # Extract nested engineStatus.state when available (authoritative in this PES build)
        if hasattr(status_obj, 'engineStatus') and status_obj.engineStatus is not None:
            es = status_obj.engineStatus
            if hasattr(es, 'state'):
                try:
                    result["engineStatus"] = int(es.state)
                except Exception:
                    pass
                result["engineState"] = _engine_state_name(es.state)
                result["state_after"] = result["engineState"]
            if hasattr(es, 'isReadyForPrintData'):
                result["isReadyForPrintData"] = bool(es.isReadyForPrintData)

        # Extract isReadyForPrintData flag
        if hasattr(status_obj, 'isReadyForPrintData'):
            result["isReadyForPrintData"] = bool(status_obj.isReadyForPrintData)
        elif hasattr(status_obj, 'isReady'):
            result["isReadyForPrintData"] = bool(status_obj.isReady)
        elif hasattr(status_obj, 'readyForPrintData'):
            result["isReadyForPrintData"] = bool(status_obj.readyForPrintData)
        
        # Extract job queue
        q = None
        if hasattr(status_obj, 'jobQueue'):
            q = status_obj.jobQueue
        elif hasattr(status_obj, 'queue'):
            q = status_obj.queue
        
        if q is not None:
            if isinstance(q, (list, tuple)):
                result["queueLen"] = len(q)
                if len(q) > 0:
                    first_job = q[0]
                    if hasattr(first_job, 'jobId'):
                        result["queueHeadJobId"] = str(first_job.jobId)
                    elif isinstance(first_job, (str, unicode)):
                        result["queueHeadJobId"] = str(first_job)
            elif hasattr(q, '__len__'):
                result["queueLen"] = len(q)
        
    except Exception as e:
        # Fallback: regex parse of repr string
        result["extraction"] = "fallback_regex"
        txt = result["raw"]
        
        # Try to extract engineState from string representation
        # Common patterns: "engineState=5", "state: PRINT_READY", "PRINT_READY"
        state_patterns = [
            r'engineState[=:]\s*(\d+)',
            r'state[=:]\s*(\d+)',
            r'(PRIMED_IDLE|PRINT_READY|PRE_JOB|PRINTING|PAUSED|SESSION_COMPLETE|OFF|FAULT|INITIALISING|PREPARING|MID_JOB|SHUTTING_DOWN)'
        ]
        engine_state = "UNKNOWN"
        for pattern in state_patterns:
            match = re.search(pattern, txt, re.IGNORECASE)
            if match:
                if match.group(1).isdigit():
                    engine_state = _engine_state_name(int(match.group(1)))
                else:
                    engine_state = match.group(1).upper()
                break
        
        # Normalize engine state (Python 2-safe)
        if engine_state is None:
            engine_state = "UNKNOWN"
        if not isinstance(engine_state, basestring):
            engine_state = str(engine_state)
        engine_state = engine_state.strip().upper() or "UNKNOWN"
        result["engineState"] = engine_state
        result["state_after"] = engine_state
        
        # Try to extract isReadyForPrintData from string representation
        ready_patterns = [
            r'isReadyForPrintData[=:]\s*(true|false|1|0)',
            r'isReady[=:]\s*(true|false|1|0)',
            r'readyForPrintData[=:]\s*(true|false|1|0)'
        ]
        for pattern in ready_patterns:
            match = re.search(pattern, txt, re.IGNORECASE)
            if match:
                ready_val = match.group(1).lower()
                result["isReadyForPrintData"] = (ready_val == "true" or ready_val == "1")
                break
        
        # Try to extract jobQueue length
        queue_patterns = [
            r'jobQueue\s*=\s*\[([^\]]*)\]',
            r'queue\s*=\s*\[([^\]]*)\]',
            r'queueLen[=:]\s*(\d+)'
        ]
        for pattern in queue_patterns:
            match = re.search(pattern, txt, re.IGNORECASE)
            if match:
                if match.group(1).strip():
                    # Count items in the list representation
                    items = match.group(1).split(',')
                    result["queueLen"] = len([i for i in items if i.strip()])
                    if result["queueLen"] > 0:
                        # Try to extract first job ID
                        job_id_match = re.search(r'jobId[=:]\s*([0-9a-fA-F]+)', match.group(1))
                        if job_id_match:
                            result["queueHeadJobId"] = job_id_match.group(1)
                else:
                    result["queueLen"] = 0
                break
    
    return result


def _run_one(client, cmd_ttypes, raw):
    token = raw.strip().lower()

    # Ignore shell redirection/control tokens accidentally passed through wrappers.
    if token in ('2>&1', '1>&2', '2>nul', '1>nul', '&&', '|'):
        print("[Thrift Real] Ignoring shell token: %s" % raw)
        return 'IGNORED_SHELL_TOKEN'

    # Some wrappers append punctuation to commands (e.g. "statusjson,").
    token = token.rstrip(',;')

    print("[Thrift Real] Running command: %s" % raw)

    if token == 'clear':
        result = client.clearJobQueue()
    elif token == 'initialise' or token == 'initialize':
        result = client.initialiseEngine()
    elif token.startswith('prepare'):
        speed, timeout_sec = _parse_prepare_args(token)
        start_ts = time.time()
        attempt = 0
        last_err = None
        while True:
            attempt += 1
            try:
                result = client.prepareToPrint(speed)
                print("[Thrift Real] prepare success after %d attempt(s)" % attempt)
                break
            except Exception as e:
                last_err = e
                msg = str(e).lower()
                if ('engine must be idle' in msg) or ('engine must idle' in msg):
                    print("[Thrift Real] prepare skipped: already primed (%s)" % e)
                    result = 'PREPARE_SKIPPED_ALREADY_PRIMED'
                    break
                transient = ('not ready' in msg) or ('busy' in msg)
                elapsed = time.time() - start_ts
                if (not transient) or (elapsed >= timeout_sec):
                    raise
                print("[Thrift Real] prepare retry %d (elapsed=%.2fs): %s" % (attempt, elapsed, e))
                time.sleep(0.4)
    elif token == 'start':
        result = client.startPrinting()
    elif token == 'finish':
        result = client.finishPrinting()
    elif token == 'shutdown':
        params = cmd_ttypes.ShutDownEngineParams(printheadPos=cmd_ttypes.PrintheadPosition.CAPPED)
        result = client.shutDownEngine(params)
    elif token == 'status':
        result = client.getStatus()
    elif token == 'statusjson':
        status_obj = client.getStatus()
        result_json = _status_to_json(status_obj)
        
        # Force non-empty state_after + flush (Python 2-safe)
        state_after = result_json.get("state_after", "")
        if not state_after:
            state_after = result_json.get("engineState", "UNKNOWN")
        if state_after is None:
            state_after = "UNKNOWN"
        if not isinstance(state_after, basestring):
            state_after = str(state_after)
        
        result_json["state_after"] = state_after.strip().upper() or "UNKNOWN"
        
        print(json.dumps(result_json))
        sys.stdout.flush()
        return result_json
    elif token == 'settings':
        result = client.getSettings()
    elif token.startswith('xadjust='):
        x_val = float(token.split('=',1)[1])
        settings = client.getSettings()
        before = getattr(settings, 'xAdjust', None)
        settings.xAdjust = x_val
        client.storeSettings(settings)
        after = client.getSettings().xAdjust
        result = 'xAdjust: %s -> %s' % (before, after)
    elif token.startswith('yoffset='):
        y_val = float(token.split('=',1)[1])
        settings = client.getSettings()
        stage = settings.engineStage[1]
        stage.mediaReadyOffset.isFactoryDefault = False
        before = stage.mediaReadyOffset.value
        stage.mediaReadyOffset.value = y_val
        client.storeSettings(settings)
        after = client.getSettings().engineStage[1].mediaReadyOffset.value
        result = 'mediaReadyOffset(stage1): %s -> %s' % (before, after)
    elif token.startswith('justify='):
        raw = token.split('=',1)[1].strip().lower()
        mapping = {'left':0,'center':1,'centre':1,'right':2}
        if raw in mapping:
            j_val = mapping[raw]
        else:
            j_val = int(raw)
            if j_val not in (0,1,2):
                raise ValueError('justify must be 0, 1, or 2')
        settings = client.getSettings()
        before = getattr(settings, 'imageJustification', None)
        settings.imageJustification = j_val
        client.storeSettings(settings)
        after = client.getSettings().imageJustification
        result = 'imageJustification: %s -> %s' % (before, after)
    elif token.startswith('pulse='):
        p_val = float(token.split('=',1)[1])
        if p_val <= 0.0 or p_val > 1.5:
            raise ValueError('pulse must be >0.0 and <=1.5')
        settings = client.getSettings()
        before_obj = getattr(settings, 'pulseWidthCustMultiplier', None)
        before = getattr(before_obj, 'value', None) if before_obj is not None else None
        try:
            from Memjet.KareelaPesApi.Common import ttypes as common_ttypes
        except ImportError:
            from memjet.KareelaPesApi.Common import ttypes as common_ttypes
        pwm = common_ttypes.DoubleWithFactoryDefault()
        pwm.isFactoryDefault = False
        pwm.value = p_val
        settings.pulseWidthCustMultiplier = pwm
        client.storeSettings(settings)
        after_obj = client.getSettings().pulseWidthCustMultiplier
        after = getattr(after_obj, 'value', None) if after_obj is not None else None
        result = 'pulseWidthCustMultiplier: %s -> %s' % (before, after)
    elif token.startswith('waitstate='):
        parts = token.split('=', 1)
        if len(parts) < 2:
            raise ValueError("waitstate requires target state: waitstate=<state>[=<timeout>]")
        state_timeout = parts[1].split('=', 1)
        target_state_raw = state_timeout[0]
        timeout_sec = int(state_timeout[1]) if len(state_timeout) > 1 and state_timeout[1] else 30
        
        # Normalize accepted states list once
        accepted_states = [target_state_raw.strip().upper()]
        accepted_states = [s.strip().upper() for s in accepted_states]
        
        start = time.time()
        result = None
        last_state = "UNKNOWN"
        
        while True:
            st = client.getStatus()
            status_json = _status_to_json(st)
            
            # Exact state selection logic
            state_after = (status_json.get("state_after") or "").strip().upper()
            engine_state = (status_json.get("engineState") or "").strip().upper()
            state = state_after or engine_state or "UNKNOWN"
            
            if state != "UNKNOWN":
                last_state = state
            
            print("[Thrift Real] waitstate: current=%s target=%s" % (state, accepted_states[0]))
            
            if state in accepted_states:
                result = {"result": "OK", "state_after": state}
                break
            
            if (time.time() - start) >= timeout_sec:
                # Timeout error uses last_state variable
                raise RuntimeError('waitstate timed out after %ss (last=%s, target=%s)' % 
                                 (timeout_sec, last_state, accepted_states[0]))
            time.sleep(0.5)
    elif token.startswith('sleep='):
        seconds = _parse_value(token, 'sleep seconds', 1)
        print("[Thrift Real] sleeping %s sec" % seconds)
        time.sleep(seconds)
        result = 'slept %s sec' % seconds
    elif token.startswith('waitready'):
        timeout_sec = _parse_value(token, 'waitready timeout', 30)
        start = time.time()
        result = None
        while True:
            st = client.getStatus()
            print("[Thrift Real] status: %s" % _status_to_text(st))
            if _looks_ready(st):
                result = st
                break
            if (time.time() - start) >= timeout_sec:
                raise RuntimeError('waitready timed out after %ss' % timeout_sec)
            time.sleep(1)
    elif token.startswith('fullcycle'):
        speed = _parse_prepare_speed(token.replace('fullcycle', 'prepare', 1))
        r1 = client.clearJobQueue(); print("[Thrift Real] clear -> %s" % r1)
        r2 = client.initialiseEngine(); print("[Thrift Real] initialise -> %s" % r2)
        r3 = client.prepareToPrint(speed); print("[Thrift Real] prepare(%s) -> %s" % (speed, r3))
        r4 = client.startPrinting(); print("[Thrift Real] start -> %s" % r4)
        result = {'clear': r1, 'initialise': r2, 'prepare': r3, 'start': r4}
    elif token == 'endcycle':
        r1 = client.finishPrinting(); print("[Thrift Real] finish -> %s" % r1)
        params = cmd_ttypes.ShutDownEngineParams(printheadPos=cmd_ttypes.PrintheadPosition.CAPPED)
        r2 = client.shutDownEngine(params); print("[Thrift Real] shutdown -> %s" % r2)
        result = {'finish': r1, 'shutdown': r2}
    else:
        raise ValueError("Unsupported command '%s'" % raw)

    print("[Thrift Real] %s -> %s" % (raw, result))
    return result


def run_commands(host, port, commands):
    sdk_root = _setup_import_paths()
    if sdk_root is None:
        raise RuntimeError("Cannot locate SDK python packages. Set PDL_THRIFT_ROOT or copy memjet/ and thrift/ beside thrift_controller_fullcycle.py")

    from thrift.transport import TSocket
    from thrift.transport import TTransport
    from thrift.protocol import TCompactProtocol

    try:
        from Memjet.KareelaPesApi.Command import KPesCommand
        from Memjet.KareelaPesApi.Command import ttypes as cmd_ttypes
    except ImportError:
        from memjet.KareelaPesApi.Command import KPesCommand
        from memjet.KareelaPesApi.Command import ttypes as cmd_ttypes

    sock = TSocket.TSocket(host, int(port))
    transport = TTransport.TFramedTransport(sock)
    protocol = TCompactProtocol.TCompactProtocol(transport)
    client = KPesCommand.Client(protocol)

    print("--- Thrift Controller (REAL) ---")
    print("SDK root: %s" % sdk_root)
    print("Target: %s:%s" % (host, port))
    print("Commands: %s" % ', '.join(commands))

    transport.open()
    try:
        for raw in commands:
            _run_one(client, cmd_ttypes, raw)
    finally:
        transport.close()
        print("--- Thrift Controller (REAL) Finished ---")


def main():
    if len(sys.argv) < 4:
        sys.stderr.write("Usage: python thrift_controller_fullcycle.py <printer_ip> <control_port> <command1> [command2 ...]\n")
        return 1

    host = sys.argv[1]
    port = sys.argv[2]
    commands = sys.argv[3:]

    try:
        run_commands(host, port, commands)
        return 0
    except Exception as exc:
        sys.stderr.write("[Thrift Real] ERROR: %s\n" % exc)
        traceback.print_exc()
        return 1


if __name__ == '__main__':
    sys.exit(main())


