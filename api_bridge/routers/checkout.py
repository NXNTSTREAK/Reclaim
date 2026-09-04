import time
from fastapi import APIRouter
from fastapi.responses import HTMLResponse
from api_bridge.config import RAZORPAY_KEY_ID

router = APIRouter()

@router.get("/mock_checkout/{payment_id}")
async def mock_checkout(payment_id: str, amount: int = 0, created_at: int = 0, order_id: str = "", key: str = ""):
    now = int(time.time())

    # Query database for missing amount or creation time
    if amount <= 0 or created_at <= 0:
        try:
            from api_bridge.services.payment_service import get_db_connection, extract_clean_payment_id
            clean_id = extract_clean_payment_id(payment_id)
            conn = get_db_connection()
            cursor = conn.cursor()
            cursor.execute("SELECT amount_paise, created_at FROM payments WHERE payment_id = ?", (clean_id,))
            row = cursor.fetchone()
            conn.close()
            if row:
                if amount <= 0 and row["amount_paise"] > 0:
                    amount = row["amount_paise"]
                if created_at <= 0 and row["created_at"] > 0:
                    created_at = row["created_at"]
        except Exception:
            pass

    rupees = amount / 100.0 if amount > 0 else 0.0
    
    # 2-Minute Link Expiry Check (120 seconds)
    is_expired = False
    if created_at > 0 and (now - created_at) > 120:
        is_expired = True

    if is_expired:
        html_content = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Link Expired - Reclaim Gateway</title>
    <style>
        * {{ margin: 0; padding: 0; box-sizing: border-box; }}
        body {{
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
            background: #090a0f;
            background-image: radial-gradient(at 50% 50%, rgba(239, 68, 68, 0.15) 0px, transparent 60%);
            display: flex; justify-content: center; align-items: center;
            min-height: 100vh; color: #f3f4f6; padding: 20px;
        }}
        .card {{
            background: rgba(17, 24, 39, 0.7);
            border: 1px solid rgba(239, 68, 68, 0.3);
            backdrop-filter: blur(24px);
            border-radius: 20px;
            padding: 44px 36px;
            text-align: center;
            max-width: 440px;
            width: 100%;
            box-shadow: 0 25px 50px -12px rgba(239, 68, 68, 0.2);
        }}
        .brand {{
            display: inline-flex; align-items: center; gap: 8px;
            font-size: 12px; font-weight: 700; color: #ef4444;
            letter-spacing: 1.5px; text-transform: uppercase;
            background: rgba(239, 68, 68, 0.1);
            padding: 6px 14px; border-radius: 20px; margin-bottom: 24px;
            border: 1px solid rgba(239, 68, 68, 0.2);
        }}
        h1 {{ font-size: 24px; font-weight: 700; color: #f87171; margin-bottom: 10px; }}
        .sub {{ color: #9ca3af; font-size: 14px; margin-bottom: 28px; line-height: 1.5; }}
        .amount-card {{
            background: rgba(255, 255, 255, 0.02);
            border: 1px solid rgba(239, 68, 68, 0.2);
            border-radius: 14px; padding: 20px; margin-bottom: 28px; opacity: 0.7;
        }}
        .amount-label {{ font-size: 12px; color: #ef4444; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 6px; }}
        .amount {{ font-size: 38px; font-weight: 800; color: #ef4444; text-decoration: line-through; }}
        .pid {{ font-size: 13px; color: #9ca3af; margin-top: 6px; font-family: ui-monospace, SFMono-Regular, monospace; }}
        .status-box {{
            padding: 16px; border-radius: 12px; font-size: 14px; text-align: center;
            background: rgba(239, 68, 68, 0.15); border: 1px solid #ef4444; color: #f87171; font-weight: 600;
        }}
    </style>
</head>
<body>
    <div class="card">
        <div class="brand">⚠️ PAYMENT LINK EXPIRED</div>
        <h1>Link Time Window Passed</h1>
        <p class="sub">This payment recovery link was valid for 2 minutes (120 seconds) and has expired.</p>
        
        <div class="amount-card">
            <div class="amount-label">Expired Amount</div>
            <div class="amount">&#8377;{rupees:.2f}</div>
            <div class="pid">ID: {payment_id}</div>
        </div>

        <div class="status-box">
            ⛔ LINK EXPIRED<br><small>This 2-minute recovery window passed. Please request a fresh payment link.</small>
        </div>
    </div>
</body>
</html>"""
        return HTMLResponse(content=html_content, status_code=200)

    html_content = f"""<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Reclaim Secure Payment Gateway</title>
    <style>
        * {{ margin: 0; padding: 0; box-sizing: border-box; }}
        body {{
            font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Oxygen, Ubuntu, Cantarell, sans-serif;
            background: #090a0f;
            background-image: 
                radial-gradient(at 0% 0%, rgba(82, 143, 240, 0.15) 0px, transparent 50%),
                radial-gradient(at 100% 100%, rgba(16, 185, 129, 0.1) 0px, transparent 50%);
            display: flex; justify-content: center; align-items: center;
            min-height: 100vh; color: #f3f4f6; padding: 20px;
        }}
        .card {{
            background: rgba(17, 24, 39, 0.7);
            border: 1px solid rgba(255, 255, 255, 0.1);
            backdrop-filter: blur(24px);
            border-radius: 20px;
            padding: 44px 36px;
            text-align: center;
            max-width: 440px;
            width: 100%;
            box-shadow: 0 25px 50px -12px rgba(0, 0, 0, 0.6);
        }}
        .brand {{
            display: inline-flex; align-items: center; gap: 8px;
            font-size: 12px; font-weight: 700; color: #3b82f6;
            letter-spacing: 1.5px; text-transform: uppercase;
            background: rgba(59, 130, 246, 0.1);
            padding: 6px 14px; border-radius: 20px; margin-bottom: 24px;
            border: 1px solid rgba(59, 130, 246, 0.2);
        }}
        h1 {{ font-size: 24px; font-weight: 700; color: #ffffff; margin-bottom: 10px; }}
        .sub {{ color: #9ca3af; font-size: 14px; margin-bottom: 28px; line-height: 1.5; }}
        .amount-card {{
            background: rgba(255, 255, 255, 0.03);
            border: 1px solid rgba(255, 255, 255, 0.07);
            border-radius: 14px; padding: 20px; margin-bottom: 28px;
        }}
        .amount-label {{ font-size: 12px; color: #6b7280; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 6px; }}
        .amount {{ font-size: 38px; font-weight: 800; color: #10b981; letter-spacing: -0.5px; }}
        .pid {{ font-size: 13px; color: #9ca3af; margin-top: 6px; font-family: ui-monospace, SFMono-Regular, monospace; }}
        .btn {{
            background: linear-gradient(135deg, #10b981 0%, #059669 100%);
            color: white; border: none;
            padding: 16px 28px; font-size: 16px; font-weight: 700;
            border-radius: 12px; cursor: pointer; width: 100%;
            transition: all 0.2s ease;
            box-shadow: 0 4px 14px rgba(16, 185, 129, 0.3);
        }}
        .btn:hover {{ transform: translateY(-2px); box-shadow: 0 8px 25px rgba(16, 185, 129, 0.4); }}
        .btn:active {{ transform: translateY(0); }}
        .btn:disabled {{ opacity: 0.6; cursor: not-allowed; transform: none; }}
        .btn-fail {{
            background: rgba(239, 68, 68, 0.12);
            color: #f87171;
            border: 1px solid rgba(239, 68, 68, 0.3);
            padding: 14px 28px; font-size: 15px; font-weight: 700;
            border-radius: 12px; cursor: pointer; width: 100%; margin-top: 12px;
            transition: all 0.2s ease;
        }}
        .btn-fail:hover {{
            background: rgba(239, 68, 68, 0.25);
            border-color: #ef4444;
            transform: translateY(-2px);
        }}
        .btn-fail:disabled {{ opacity: 0.5; cursor: not-allowed; transform: none; }}
        .secure {{ margin-top: 24px; font-size: 12px; color: #6b7280; display: flex; align-items: center; justify-content: center; gap: 6px; }}
        .secure-icon {{ color: #10b981; }}
        .status-box {{
            margin-top: 20px; padding: 16px; border-radius: 12px; display: none; font-size: 14px; text-align: center;
        }}
        .status-box.success {{
            background: rgba(16, 185, 129, 0.15); border: 1px solid #10b981; color: #34d399; font-weight: 600;
        }}
        .status-box.error {{
            background: rgba(239, 68, 68, 0.15); border: 1px solid #ef4444; color: #f87171;
        }}
    </style>
</head>
<body>
    <div class="card">
        <div class="brand">⚡ RECLAIM RECOVERY GATEWAY</div>
        <h1>Complete Your Payment</h1>
        <p class="sub">Your previous transaction was incomplete. Pay securely below to complete your payment.</p>
        
        <div class="amount-card">
            <div class="amount-label">Total Amount Due</div>
            <div class="amount">&#8377;{rupees:.2f}</div>
            <div class="pid">ID: {payment_id}</div>
        </div>

        <button class="btn" id="pay-btn" onclick="executePayment()">Pay &#8377;{rupees:.2f} Now</button>
        <button class="btn-fail" id="fail-btn" onclick="executeFail()">Decline / Fail Payment</button>
        
        <div id="status" class="status-box"></div>

        <div class="secure">
            <span class="secure-icon">&#10003;</span> 256-bit Encrypted SSL &bull; Instant Settlement Engine (TTL: 2m)
        </div>
    </div>

    <script>
        async function executePayment() {{
            const payBtn = document.getElementById("pay-btn");
            const failBtn = document.getElementById("fail-btn");
            const statusDiv = document.getElementById("status");
            
            payBtn.disabled = true;
            failBtn.disabled = true;
            payBtn.innerText = "Processing Payment...";
            
            try {{
                const res = await fetch("/webhook/payment_captured", {{
                    method: "POST",
                    headers: {{ "Content-Type": "application/json" }},
                    body: JSON.stringify({{
                        payment_id: "{payment_id}",
                        amount: {amount}
                    }})
                }});
                const data = await res.json();
                if (data.status === "rejected") {{
                    statusDiv.style.display = "block";
                    statusDiv.className = "status-box error";
                    statusDiv.innerHTML = "&#9888; Engine Daemon is currently offline.<br><small>Click [ START ENGINE ] on dashboard and try again.</small>";
                    payBtn.disabled = false;
                    failBtn.disabled = false;
                    payBtn.innerText = "Retry Payment";
                }} else {{
                    statusDiv.style.display = "block";
                    statusDiv.className = "status-box success";
                    statusDiv.innerHTML = "&#10003; Payment Completed Successfully!<br><small>Financial Ledger credited & transaction marked PAID.</small>";
                    payBtn.style.display = "none";
                    failBtn.style.display = "none";
                }}
            }} catch (err) {{
                statusDiv.style.display = "block";
                statusDiv.className = "status-box error";
                statusDiv.innerText = "Error completing payment: " + err.message;
                payBtn.disabled = false;
                failBtn.disabled = false;
                payBtn.innerText = "Retry Payment";
            }}
        }}

        async function executeFail() {{
            const payBtn = document.getElementById("pay-btn");
            const failBtn = document.getElementById("fail-btn");
            const statusDiv = document.getElementById("status");
            
            payBtn.disabled = true;
            failBtn.disabled = true;
            failBtn.innerText = "Declining Payment...";
            
            try {{
                const res = await fetch("/api/abandon_payment", {{
                    method: "POST",
                    headers: {{ "Content-Type": "application/json" }},
                    body: JSON.stringify({{
                        payment_id: "{payment_id}",
                        reason: "Customer explicitly clicked Decline / Fail Payment"
                    }})
                }});
                const data = await res.json();
                statusDiv.style.display = "block";
                statusDiv.className = "status-box error";
                statusDiv.innerHTML = "❌ Payment Declined / Abandoned<br><small>Recorded ABANDONED in database & logged to dashboard.</small>";
                payBtn.style.display = "none";
                failBtn.style.display = "none";
            }} catch (err) {{
                statusDiv.style.display = "block";
                statusDiv.className = "status-box error";
                statusDiv.innerText = "Error marking failed: " + err.message;
                payBtn.disabled = false;
                failBtn.disabled = false;
                failBtn.innerText = "Decline / Fail Payment";
            }}
        }}
    </script>
</body>
</html>"""

    return HTMLResponse(content=html_content, status_code=200)

