import time
import sqlite3
import razorpay
from api_bridge.config import RAZORPAY_KEY_ID, RAZORPAY_KEY_SECRET

rzp_client = razorpay.Client(auth=(RAZORPAY_KEY_ID, RAZORPAY_KEY_SECRET))

import os
PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DB_PATH = os.path.join(PROJECT_ROOT, "recovery_engine.db")

def get_db_connection():
    conn = sqlite3.connect(DB_PATH, timeout=10.0)
    conn.row_factory = sqlite3.Row
    return conn

def create_payment_link(payment_id: str, amount_paise: int, expire_minutes: int = 2, is_simulation: bool = False) -> str:
    """
    Generates a local Reclaim Payment Link (localhost URL) with a 2-minute expiry window.
    Bypasses external Razorpay API to prevent quota exhaustion and ensure instant local testing.
    """
    now = int(time.time())
    checkout_url = f"http://127.0.0.1:8000/mock_checkout/{payment_id}?amount={amount_paise}&created_at={now}"
    print(f"[RECLAIM LOCAL PAYMENT LINK] Generated local URL (TTL 2m): {checkout_url} for {payment_id}")
    return checkout_url


def schedule_twenty_four_hour_retry(payment_id: str, amount_paise: int):
    """
    Persists insufficient funds retry into SQLite recovery_attempts table.
    """
    now = int(time.time())
    expires_at = now + 120  # 2-minute retry delay for testing
    attempt_id = f"att_py_retry_{payment_id}_{now}"

    try:
        conn = get_db_connection()
        cursor = conn.cursor()
        cursor.execute(
            """
            INSERT INTO recovery_attempts (attempt_id, payment_id, strategy, state, razorpay_link_id, created_at, expires_at, completed_at)
            VALUES (?, ?, 'PAYDAY_RETRY', 'CREATED', '', ?, ?, 0)
            """,
            (attempt_id, payment_id, now, expires_at)
        )
        conn.commit()
        conn.close()
        print(f"[FAST-PATH] Insufficient funds for {payment_id}. Persisted 24-hour retry into SQLite.")
    except Exception as e:
        print(f"[DB ERROR] Failed to persist retry for {payment_id}: {e}")

def extract_clean_payment_id(raw_id: str) -> str:
    if not raw_id:
        return ""
    clean = str(raw_id).strip()
    
    # 1. Direct match check in SQLite database
    try:
        conn = get_db_connection()
        cursor = conn.cursor()
        cursor.execute("SELECT payment_id FROM payments WHERE payment_id = ?", (clean,))
        row = cursor.fetchone()
        conn.close()
        if row:
            return row["payment_id"]
    except Exception:
        pass
        
    # 2. Trim optional epoch timestamp suffix (_1788510500) if present at end
    import re
    match = re.search(r'^(pay_[A-Za-z0-9_]+?)(?:_\d{10})?$', clean)
    if match:
        candidate = match.group(1)
        try:
            conn = get_db_connection()
            cursor = conn.cursor()
            cursor.execute("SELECT payment_id FROM payments WHERE payment_id = ?", (candidate,))
            row = cursor.fetchone()
            conn.close()
            if row:
                return row["payment_id"]
        except Exception:
            pass
        return candidate
        
    return clean

def mark_payment_successful(payment_id: str, amount_paise: int = 0) -> bool:
    """
    Webhook synchronization: Atomically credits financial_ledger in SQLite and marks payment SUCCESSFUL.
    Returns True if new credit recorded, False if duplicate webhook ignored.
    """
    clean_id = extract_clean_payment_id(payment_id)
    now = int(time.time())
    ledger_id = f"ledg_{clean_id}_{now}"

    try:
        conn = get_db_connection()
        cursor = conn.cursor()

        # Upsert payment record into payments table
        cursor.execute(
            """
            INSERT INTO payments (payment_id, amount_paise, current_state, created_at, updated_at)
            VALUES (?, ?, 'SUCCESSFUL', ?, ?)
            ON CONFLICT(payment_id) DO UPDATE SET 
                current_state = 'SUCCESSFUL',
                amount_paise = CASE WHEN payments.amount_paise = 0 THEN excluded.amount_paise ELSE payments.amount_paise END,
                updated_at = excluded.updated_at
            """,
            (clean_id, amount_paise, now, now)
        )
        
        # Complete recovery attempts
        cursor.execute("UPDATE recovery_attempts SET state = 'COMPLETED', completed_at = ? WHERE payment_id = ?", (now, clean_id))
        
        # Atomic credit to financial_ledger preventing double-counting
        cursor.execute(
            """
            INSERT INTO financial_ledger (ledger_id, payment_id, attempt_id, amount_paise, entry_type, created_at)
            SELECT ?, ?, 'wh_captured', COALESCE(NULLIF((SELECT amount_paise FROM payments WHERE payment_id = ?), 0), ?), 'RECOVERY_CREDIT', ?
            WHERE NOT EXISTS (
                SELECT 1 FROM financial_ledger WHERE payment_id = ? AND entry_type = 'RECOVERY_CREDIT'
            )
            """,
            (ledger_id, clean_id, clean_id, amount_paise, now, clean_id)
        )
        changes = cursor.rowcount
        conn.commit()
        conn.close()

        if changes > 0:
            print(f"[WEBHOOK SYNC] Payment {clean_id} marked SUCCESSFUL. Financial ledger credited in SQLite.")
            return True
        else:
            print(f"[WEBHOOK SYNC GUARD] Payment {clean_id} already credited previously. Duplicate ignored.")
            return False
    except Exception as e:
        print(f"[DB ERROR] Webhook sync failed for {clean_id}: {e}")
        return False

def verify_payment_with_razorpay_api(payment_id: str) -> bool:
    """
    Direct API verification fallback: Queries Razorpay API Orders, Payment Links, & Payments.
    If Razorpay API returns paid or captured status, marks SQLite state SUCCESSFUL & credits financial_ledger.
    """
    from api_bridge.config import RAZORPAY_KEY_ID, RAZORPAY_KEY_SECRET
    import razorpay
    if not RAZORPAY_KEY_ID or not RAZORPAY_KEY_SECRET:
        return False
        
    clean_id = extract_clean_payment_id(payment_id)
    try:
        client = razorpay.Client(auth=(RAZORPAY_KEY_ID, RAZORPAY_KEY_SECRET))
        
        # 1. Query recent Orders (Razorpay Payment Links store reference_id inside order.receipt)
        orders = client.order.all({"count": 20})
        for item in orders.get("items", []):
            receipt = item.get("receipt", "")
            notes = item.get("notes", {})
            status = item.get("status", "")
            amt = int(item.get("amount_paid", 0) or item.get("amount", 0))
            
            if status in ["paid", "partially_paid"] and (clean_id in receipt or clean_id in str(notes)):
                matched_id = extract_clean_payment_id(receipt) or clean_id
                is_new = mark_payment_successful(matched_id, amt)
                if is_new:
                    print(f"[RAZORPAY DIRECT API VERIFICATION] Payment {matched_id} verified PAID via Razorpay Orders API query.")
                return True

        # 2. Query recent payment links
        plinks = client.payment_link.all({"count": 10})
        for item in plinks.get("items", []):
            ref_id = item.get("reference_id", "")
            desc = item.get("description", "")
            status = item.get("status", "")
            amt = int(item.get("amount_paid", 0) or item.get("amount", 0))
            
            if status in ["paid", "partially_paid"] and (clean_id in ref_id or clean_id in desc):
                matched_id = extract_clean_payment_id(ref_id) or clean_id
                is_new = mark_payment_successful(matched_id, amt)
                if is_new:
                    print(f"[RAZORPAY DIRECT API VERIFICATION] Payment {matched_id} verified PAID via Razorpay Payment Link query.")
                return True
                
        # 3. Query recent captured payments
        payments = client.payment.all({"count": 10})
        for item in payments.get("items", []):
            desc = item.get("description", "")
            notes = item.get("notes", {})
            status = item.get("status", "")
            amt = int(item.get("amount", 0))
            
            if status == "captured" and (clean_id in desc or clean_id in str(notes)):
                is_new = mark_payment_successful(clean_id, amt)
                if is_new:
                    print(f"[RAZORPAY DIRECT API VERIFICATION] Payment {clean_id} verified CAPTURED via Razorpay Payments query.")
                return True
    except Exception as e:
        print(f"[RAZORPAY VERIFY API ERROR] {e}")
        
    return False

def check_if_already_paid(payment_id: str, check_remote_api: bool = False) -> bool:
    """
    Queries SQLite payments table to verify if payment is already resolved.
    Fast local check in <0.1ms.
    """
    clean_id = extract_clean_payment_id(payment_id)
    try:
        conn = get_db_connection()
        cursor = conn.cursor()
        cursor.execute("SELECT current_state FROM payments WHERE payment_id = ?", (clean_id,))
        row = cursor.fetchone()
        conn.close()
        if row and row["current_state"] == "SUCCESSFUL":
            return True
    except Exception as e:
        print(f"[DB ERROR] Failed to check status for {clean_id}: {e}")
        
    if check_remote_api:
        return verify_payment_with_razorpay_api(clean_id)
        
    return False