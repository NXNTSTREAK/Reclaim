import asyncio
import json
import os
import signal
import time
import urllib.request
from typing import Optional, Set, List
from fastapi import APIRouter, Request
from fastapi.responses import StreamingResponse

from api_bridge.services.ai_service import analyze_with_ai
from api_bridge.services.payment_service import (
    create_payment_link, 
    schedule_twenty_four_hour_retry, 
    mark_payment_successful, 
    check_if_already_paid
)
from api_bridge.services.email_service import send_recovery_email

router = APIRouter()

PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
ENGINE_BINARY = os.path.join(PROJECT_ROOT, "build", "recovery_engine")

engine_process: Optional[asyncio.subprocess.Process] = None
ollama_process: Optional[asyncio.subprocess.Process] = None
log_subscribers: Set[asyncio.Queue] = set()
log_history: List[str] = []
reader_task: Optional[asyncio.Task] = None


def emit_log(line: str):
    """Broadcasting helper to stream python backend logs to SSE clients."""
    print(line)
    log_history.append(line)
    if len(log_history) > 1000:
        log_history.pop(0)
    for q in list(log_subscribers):
        try:
            q.put_nowait(line)
        except Exception:
            pass


async def _check_and_start_ollama():
    global ollama_process
    try:
        req = urllib.request.Request("http://localhost:11434/api/tags", method="GET")
        with urllib.request.urlopen(req, timeout=1.5):
            emit_log("[OLLAMA SERVICE] Ollama server is online at http://localhost:11434")
            return
    except Exception:
        emit_log("[OLLAMA SERVICE] Starting background Ollama server (ollama serve)...")
        try:
            ollama_process = await asyncio.create_subprocess_exec(
                "ollama", "serve",
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.DEVNULL
            )
            await asyncio.sleep(2.0)
            emit_log("[OLLAMA SERVICE] Background Ollama service launched.")
        except Exception as e:
            emit_log(f"[OLLAMA WARNING] Could not auto-launch Ollama: {e}")


async def _read_engine_stdout():
    global engine_process
    if not engine_process or not engine_process.stdout:
        return
    try:
        while engine_process.returncode is None:
            line_bytes = await engine_process.stdout.readline()
            if not line_bytes:
                break
            line = line_bytes.decode("utf-8", errors="replace").rstrip("\r\n")
            if not line:
                continue
            emit_log(line)
    except asyncio.CancelledError:
        pass
    except Exception as e:
        emit_log(f"[ENGINE STREAM ERROR] {e}")


def is_engine_active() -> bool:
    return engine_process is not None and engine_process.returncode is None


@router.post("/api/engine/start")
async def start_engine():
    global engine_process, reader_task
    if is_engine_active():
        return {"status": "already_running", "pid": engine_process.pid}

    if not os.path.exists(ENGINE_BINARY):
        return {"status": "error", "message": f"Binary not found at {ENGINE_BINARY}"}

    # Clear log history for a fresh dashboard session
    log_history.clear()

    # 1. Start background Ollama model service
    asyncio.create_task(_check_and_start_ollama())

    # 2. Inject RESET_DB=true so fresh engine start clears past data
    from api_bridge.config import RAZORPAY_KEY_ID, RAZORPAY_KEY_SECRET
    env = os.environ.copy()
    env["RESET_DB"] = "true"
    env["DEMO_MODE"] = "true"
    env["PYTHONUNBUFFERED"] = "1"
    env["RAZORPAY_KEY_ID"] = RAZORPAY_KEY_ID
    env["RAZORPAY_KEY_SECRET"] = RAZORPAY_KEY_SECRET

    emit_log("[SYSTEM] Starting C++ Recovery Engine daemon (Fresh Session)...")

    engine_process = await asyncio.create_subprocess_exec(
        ENGINE_BINARY,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
        cwd=PROJECT_ROOT,
        env=env
    )

    reader_task = asyncio.create_task(_read_engine_stdout())

    return {"status": "started", "pid": engine_process.pid}


@router.post("/api/engine/stop")
async def stop_engine():
    global engine_process, reader_task
    if not is_engine_active():
        return {"status": "not_running"}

    pid = engine_process.pid
    try:
        engine_process.send_signal(signal.SIGINT)
        try:
            await asyncio.wait_for(engine_process.wait(), timeout=5.0)
        except asyncio.TimeoutError:
            engine_process.terminate()
            await engine_process.wait()
        emit_log("[SYSTEM] Engine daemon stopped successfully.")
    except Exception as e:
        emit_log(f"[ENGINE STOP ERROR] {e}")

    if reader_task:
        reader_task.cancel()
        reader_task = None

    log_history.clear()
    engine_process = None
    return {"status": "stopped", "pid": pid}


@router.get("/api/engine/status")
async def engine_status():
    is_running = engine_process is not None and engine_process.returncode is None
    pid = engine_process.pid if is_running else None
    return {"status": "running" if is_running else "stopped", "pid": pid}


@router.get("/api/metrics")
async def get_metrics():
    if not is_engine_active():
        return {"revenue_at_risk": 0.0, "revenue_recovered": 0.0}
    try:
        from api_bridge.services.payment_service import get_db_connection
        conn = get_db_connection()
        cursor = conn.cursor()
        # Revenue at risk counts active recoverable payments currently in FAILED or RECOVERING states
        cursor.execute("SELECT COALESCE(SUM(amount_paise), 0) FROM payments WHERE current_state IN ('FAILED', 'RECOVERING')")
        at_risk = cursor.fetchone()[0]
        cursor.execute("SELECT COALESCE(SUM(amount_paise), 0) FROM financial_ledger WHERE entry_type = 'RECOVERY_CREDIT'")
        recovered = cursor.fetchone()[0]
        conn.close()
        return {
            "revenue_at_risk": at_risk / 100.0,
            "revenue_recovered": recovered / 100.0
        }
    except Exception as e:
        return {"revenue_at_risk": 0.0, "revenue_recovered": 0.0}


@router.get("/api/logs")
async def get_logs(request: Request):
    async def log_generator():
        queue: asyncio.Queue = asyncio.Queue()
        log_subscribers.add(queue)
        try:
            if is_engine_active():
                for old_line in log_history:
                    yield f"data: {old_line}\n\n"

            while True:
                if await request.is_disconnected():
                    break
                try:
                    line = await asyncio.wait_for(queue.get(), timeout=1.0)
                    yield f"data: {line}\n\n"
                except asyncio.TimeoutError:
                    yield ": keep-alive\n\n"
        except asyncio.CancelledError:
            pass
        finally:
            log_subscribers.discard(queue)

    return StreamingResponse(log_generator(), media_type="text/event-stream")


@router.post("/generate_link")
async def generate_link(request: Request):
    if not is_engine_active():
        emit_log("[REJECTED] Request blocked — Engine daemon is currently STOPPED.")
        return {
            "status": "error",
            "message": "Engine daemon is STOPPED. Please click [ START ENGINE ] on the dashboard to process requests.",
            "action": "NONE",
            "link": "Engine Stopped"
        }

    try:
        data = await request.json()
    except Exception:
        data = request.query_params

    payment_id = data.get("payment_id", f"pay_unknown_{int(time.time())}")
    amount_paise = int(data.get("amount", 0))
    rupees = amount_paise / 100.0
    
    error_code = data.get("error_code", "UNKNOWN")
    error_reason = data.get("error_reason", "unknown")
    error_description = data.get("error_description", "No description provided")
    error_source = data.get("error_source", "unknown")
    is_simulation = data.get("is_simulation", False) or payment_id.startswith("pay_sim_") or payment_id.startswith("pay_UDP_STRESS_")
    
    from api_bridge.config import RECEIVER_EMAIL
    target_email = data.get("email") or RECEIVER_EMAIL

    t_request_start = time.time()
    emit_log(f"[GENERATE_LINK] Request received: payment_id={payment_id} amount_paise={amount_paise} error_reason={error_reason} error_source={error_source} is_simulation={is_simulation} email={target_email}")
    emit_log(f"\n[ENGINE EVENT] Processing {payment_id} | Amount: ₹{rupees:.2f} | Reason: {error_reason}")

    emit_log(f"[IDEMPOTENCY CHECK] Checking if {payment_id} was already paid...")
    if check_if_already_paid(payment_id):
        emit_log(f"[IDEMPOTENCY GUARD] Payment {payment_id} already resolved by user. Skipping.")
        return {"status": "skipped", "action": "NONE", "link": "Already Paid"}
    emit_log(f"[IDEMPOTENCY CHECK] Not yet paid. Continuing.")

    if "insufficient_funds" in error_reason and not data.get("force_email", False):
        emit_log(f"[FAST PATH] insufficient_funds detected. Bypassing AI. Scheduling payday retry directly.")
        schedule_twenty_four_hour_retry(payment_id, amount_paise)
        emit_log(f"[SCHEDULER] Insufficient funds bypass: 2-minute payday retry scheduled for {payment_id}")
        return {
            "status": "success",
            "action": "SCHEDULE_PAYDAY_RETRY",
            "link": "2-Min Retry Scheduled"
        }

    # 2. Risk Guard Fast-Path: Fraud / Risk Check Failed -> ABANDON (Bypass AI & Link)
    if any(k in error_reason.lower() for k in ["risk_check_failed", "fraud", "blacklisted", "risk"]) and not data.get("force_email", False):
        emit_log(f"[RISK GUARD] Fraud/Risk check failed for {payment_id} (reason: {error_reason}). Action: ABANDON. Bypassing AI & Link generation.")
        return {
            "status": "success",
            "action": "ABANDON",
            "link": "Abandoned (Fraud Risk)"
        }

    # Fast-Path check: Deterministic Gateway Error or Manual Live Test -> Instant Link Generation (<5ms)
    is_gateway_error = any(k in error_reason.lower() for k in ["gateway_technical_error", "payment_cancelled", "bank_timeout", "gateway_error", "timeout"])
    
    if is_gateway_error or data.get("force_email", False):
        decision = "GENERATE_NEW_LINK"
        emit_log(f"[FAST PATH AI] Gateway/Timeout error classified: Action INSTANTLY set to GENERATE_NEW_LINK.")
        # Trigger background LLM audit for logging without blocking the HTTP response
        asyncio.create_task(_async_ai_eval(payment_id, error_reason, error_source))
    else:
        prompt = f"""
        You are an RBI-compliant Payment Recovery AI Agent.
        Analyze this failed transaction:
        - PAYMENT_ID: {payment_id}
        - ERROR_REASON: "{error_reason}"
        - ERROR_SOURCE: "{error_source}"

        POLICY RULES:
        1. If ERROR_REASON contains "gateway_technical_error" or "payment_cancelled" or "bank_timeout": Action MUST BE "GENERATE_NEW_LINK".
        2. If ERROR_REASON contains "risk_check_failed" or "fraud" or "risk": Action MUST BE "ABANDON".
        3. Otherwise: Action MUST BE "ESCALATE_TO_HUMAN".

        Return ONLY a valid JSON object: {{"decision": "<GENERATE_NEW_LINK|ABANDON|ESCALATE_TO_HUMAN>", "reasoning": "<explanation>"}}
        """

        emit_log(f"[AI AGENT] Dispatching context evaluation to local LLM (phi4-mini) for transaction {payment_id}...")
        t_ai_start = time.time()
        ai_decision = analyze_with_ai(prompt)
        t_ai_end = time.time()
        decision = ai_decision.get("decision", "ESCALATE_TO_HUMAN")
        emit_log(f"[AI AGENT] LLM evaluation complete in {t_ai_end - t_ai_start:.2f}s. Decision: {decision} | Rationale: {ai_decision.get('reasoning')}")

    if decision == "GENERATE_NEW_LINK" or data.get("force_email", False):
        try:
            emit_log(f"[LINK GEN] Generating local Reclaim payment link for {payment_id} (Amount: {amount_paise} paise)...")
            t_link_start = time.time()
            link = create_payment_link(payment_id, amount_paise, expire_minutes=2, is_simulation=is_simulation)
            t_link_end = time.time()
            emit_log(f"[LINK GEN] Payment link generated in {(t_link_end - t_link_start)*1000:.2f}ms: {link}")
            emit_log(f"[RECOVERY LINK GENERATED] Link: {link} (TTL: 120s Expiry Window)")
            
            # Asynchronous background email dispatch (non-blocking)
            asyncio.create_task(_async_dispatch_email(payment_id, rupees, link, target_email))
            
            total_elapsed = time.time() - t_request_start
            emit_log(f"[GENERATE_LINK] Link generated INSTANTLY in {total_elapsed*1000:.2f}ms.")
            return {"status": "success", "action": "GENERATE_NEW_LINK", "link": link}
        except Exception as e:
            emit_log(f"[RAZORPAY ERROR] Failed link creation: {e}")
            return {"status": "error", "action": "ESCALATE_TO_HUMAN", "link": "Failed API Generation"}

    emit_log(f"[GENERATE_LINK] Decision was '{decision}', no link generated.")
    return {"status": "success", "action": decision, "link": "Not Applicable"}


async def _async_ai_eval(payment_id: str, error_reason: str, error_source: str):
    try:
        prompt = f"Analyze failed transaction {payment_id} reason {error_reason} source {error_source}"
        ai_decision = await asyncio.to_thread(analyze_with_ai, prompt)
        emit_log(f"[AI BACKGROUND AUDIT] {payment_id} evaluated: {ai_decision.get('decision')}")
    except Exception as e:
        emit_log(f"[AI BACKGROUND AUDIT] {payment_id} error: {e}")


async def _async_dispatch_email(payment_id: str, rupees: float, link: str, target_email: str):
    try:
        emit_log(f"[EMAIL] Dispatching recovery email to customer ({target_email}) for {payment_id} (₹{rupees:.2f})...")
        t0 = time.time()
        email_sent = await asyncio.to_thread(send_recovery_email, payment_id, rupees, link, target_email)
        t1 = time.time()
        if email_sent:
            emit_log(f"[DELIVERY] Recovery email delivered to {target_email} in {t1 - t0:.2f}s.")
        else:
            emit_log(f"[DELIVERY WARNING] Email dispatch failed for {target_email}")
    except Exception as e:
        emit_log(f"[EMAIL ERROR] Background email dispatch failed: {e}")


import socket

UDP_IP = "127.0.0.1"
UDP_PORT = 9001
udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


@router.post("/webhook/razorpay")
async def razorpay_webhook(request: Request):
    if not is_engine_active():
        emit_log("[REJECTED] Webhook blocked — Engine daemon is currently STOPPED.")
        return {"status": "rejected", "message": "Engine is STOPPED"}

    payload = await request.json()
    event_type = payload.get("event")
    is_simulation = payload.get("is_simulation", False)
    
    if event_type == "payment.failed":
        entity = payload.get("payload", {}).get("payment", {}).get("entity", {})
        if not is_simulation:
            is_simulation = entity.get("is_simulation", False) or payload.get("is_simulation", False)
        
        payment_id = entity.get("id", f"pay_sim_{int(time.time())}" if is_simulation else f"pay_wh_{int(time.time())}")
        amount_paise = int(entity.get("amount", 0))
        error_code = entity.get("error_code", "UNKNOWN")
        error_reason = entity.get("error_reason", "unknown")
        error_description = entity.get("error_description", "No description provided")
        error_source = entity.get("error_source", "unknown")
        error_step = entity.get("error_step", "unknown")
        
        event_payload = {
            "event_id": f"evt_wh_{payment_id}_{int(time.time())}",
            "payment_id": payment_id,
            "amount": amount_paise,
            "error_code": error_code,
            "error_description": error_description,
            "error_source": error_source,
            "error_step": error_step,
            "error_reason": error_reason,
            "is_simulation": is_simulation
        }
        
        # Zero-bloat UDP datagram dispatch to C++ daemon (0.05ms latency)
        try:
            udp_sock.sendto(json.dumps(event_payload).encode("utf-8"), (UDP_IP, UDP_PORT))
            sim_tag = " [SIMULATION]" if is_simulation else ""
            emit_log(f"[RAZORPAY WEBHOOK{sim_tag}] Ingested payment.failed for {payment_id} (₹{amount_paise/100:.2f}) -> Handed off to C++ engine via UDP:9001")
        except Exception as e:
            emit_log(f"[UDP ERROR] Failed to dispatch webhook event to C++ daemon: {e}")

    elif event_type in ["payment.captured", "payment_link.paid", "order.paid", "payment.authorized"]:
        plink_entity = payload.get("payload", {}).get("payment_link", {}).get("entity", {})
        payment_entity = payload.get("payload", {}).get("payment", {}).get("entity", {})
        order_entity = payload.get("payload", {}).get("order", {}).get("entity", {})
        
        amount_paise = int(payment_entity.get("amount", 0) or plink_entity.get("amount_paid", 0) or order_entity.get("amount_paid", 0))

        target_pid = None
        candidates = [
            payment_entity.get("notes", {}).get("original_payment_id"),
            payment_entity.get("notes", {}).get("payment_id"),
            plink_entity.get("notes", {}).get("original_payment_id"),
            plink_entity.get("reference_id"),
            payment_entity.get("reference_id"),
            order_entity.get("notes", {}).get("original_payment_id"),
            order_entity.get("receipt")
        ]
        
        desc = payment_entity.get("description", "") or plink_entity.get("description", "")
        if desc:
            import re
            m = re.search(r'(pay_[A-Za-z0-9_]+)', desc)
            if m:
                candidates.append(m.group(1))

        from api_bridge.services.payment_service import extract_clean_payment_id
        for cand in candidates:
            if cand:
                clean = extract_clean_payment_id(str(cand))
                if clean:
                    target_pid = clean
                    break

        if target_pid:
            is_new = mark_payment_successful(target_pid, amount_paise)
            if is_new:
                emit_log(f"[FINANCIAL LEDGER] Successfully credited ₹{amount_paise/100:.2f} for payment {target_pid}")
            else:
                emit_log(f"[IDEMPOTENCY GUARD] Duplicate captured webhook for {target_pid}. Ignored.")
        else:
            emit_log(f"[WEBHOOK WARNING] Could not resolve payment ID for {event_type} event.")

    return {"status": "received"}


@router.get("/api/payment_status/{payment_id}")
async def get_payment_status(payment_id: str):
    from api_bridge.services.payment_service import extract_clean_payment_id, get_db_connection, verify_payment_with_razorpay_api
    clean_id = extract_clean_payment_id(payment_id)
    try:
        conn = get_db_connection()
        cursor = conn.cursor()
        cursor.execute("SELECT current_state FROM payments WHERE payment_id = ?", (clean_id,))
        row = cursor.fetchone()
        conn.close()
        if row and row["current_state"] == "SUCCESSFUL":
            return {"payment_id": clean_id, "state": "SUCCESSFUL"}
    except Exception:
        pass
        
    # Check Razorpay API directly if webhook was delayed/missed
    is_paid = verify_payment_with_razorpay_api(clean_id)
    if is_paid:
        return {"payment_id": clean_id, "state": "SUCCESSFUL"}
        
    return {"payment_id": clean_id, "state": "UNKNOWN"}


@router.post("/webhook/payment_captured")
async def payment_captured_hook(request: Request):
    """Called by the checkout page JS after a successful Razorpay payment."""
    if not is_engine_active():
        emit_log("[REJECTED] Payment captured hook blocked — Engine daemon is currently STOPPED.")
        return {"status": "rejected", "message": "Engine is STOPPED"}

    data = await request.json()
    payment_id = data.get("original_payment_id") or data.get("payment_id", "")
    amount = data.get("amount", 0)
    if payment_id:
        is_new = mark_payment_successful(payment_id, amount)
        if is_new:
            emit_log(f"[FINANCIAL LEDGER] Successfully credited ₹{amount/100:.2f} for payment {payment_id}")
        else:
            emit_log(f"[IDEMPOTENCY GUARD] Duplicate payment_captured webhook for {payment_id}. Ignored.")
    return {"status": "ok"}


@router.post("/api/abandon_payment")
async def abandon_payment_endpoint(request: Request):
    """Marks payment state ABANDONED in SQLite and broadcasts red log entry."""
    if not is_engine_active():
        emit_log("[REJECTED] Abandon request blocked — Engine daemon is currently STOPPED.")
        return {"status": "rejected", "message": "Engine is STOPPED"}

    data = await request.json()
    payment_id = data.get("payment_id", "")
    reason = data.get("reason", "Customer explicit decline")
    
    if payment_id:
        from api_bridge.services.payment_service import get_db_connection, extract_clean_payment_id
        clean_id = extract_clean_payment_id(payment_id)
        now = int(time.time())
        try:
            conn = get_db_connection()
            cursor = conn.cursor()
            cursor.execute(
                """
                INSERT INTO payments (payment_id, amount_paise, current_state, created_at, updated_at)
                VALUES (?, 0, 'ABANDONED', ?, ?)
                ON CONFLICT(payment_id) DO UPDATE SET current_state = 'ABANDONED', updated_at = excluded.updated_at
                """,
                (clean_id, now, now)
            )
            cursor.execute("UPDATE recovery_attempts SET state = 'ABANDONED', completed_at = ? WHERE payment_id = ?", (now, clean_id))
            conn.commit()
            conn.close()
            emit_log(f"[ABANDONED] Payment {clean_id} set to ABANDONED (Reason: {reason}). Subtracted from Revenue at Risk.")
        except Exception as e:
            emit_log(f"[ABANDON ERROR] Failed to mark {clean_id} abandoned: {e}")
            
    return {"status": "abandoned", "payment_id": payment_id}


@router.post("/api/inject_test_payment")
async def inject_test_payment():
    """
    Pushes a synthetic failed payment directly into the engine for testing.
    Generates a unique payment_id each call so idempotency guard never blocks it.
    """
    if not is_engine_active():
        return {"status": "error", "message": "Engine is STOPPED. Please click [ START ENGINE ] on the dashboard."}

    import random
    fake_id = f"pay_TEST_{int(time.time())}_{random.randint(1000,9999)}"
    amount = random.choice([9900, 25000, 49900, 99900, 149900])
    reason = random.choice(["payment_cancelled", "gateway_technical_error", "bank_declined"])

    emit_log(f"[INJECT] Synthetic test payment injected: {fake_id} ₹{amount/100:.2f} reason={reason}")

    data = {
        "payment_id": fake_id,
        "amount": amount,
        "error_code": "GATEWAY_ERROR",
        "error_reason": reason,
        "error_description": "Simulated failure for testing",
        "error_source": "bank",
        "error_step": "payment_authn"
    }

    import asyncio
    asyncio.create_task(_process_injected_payment(data))
    return {"status": "injected", "payment_id": fake_id, "amount_paise": amount}


async def _process_injected_payment(data: dict):
    """Async task: calls /generate_link for the injected payment."""
    import aiohttp
    await asyncio.sleep(0.1)
    try:
        async with aiohttp.ClientSession() as session:
            async with session.post(
                "http://127.0.0.1:8000/generate_link",
                json=data,
                timeout=aiohttp.ClientTimeout(total=30)
            ) as resp:
                result = await resp.json()
                emit_log(f"[INJECT] Engine response for {data['payment_id']}: action={result.get('action')} link={result.get('link')}")
    except Exception as e:
        emit_log(f"[INJECT ERROR] Failed to process injected payment: {e}")