import json
import time
import requests
from google import genai
from google.genai import types
from api_bridge.config import GEMINI_API_KEY, OLLAMA_URL, OLLAMA_MODEL

# Initialize Gemini Client
try:
    ai_client = genai.Client(api_key=GEMINI_API_KEY)
except Exception as e:
    ai_client = None

def analyze_payment_failure(
    payment_id: str,
    rupees: float,
    error_code: str,
    error_reason: str,
    error_description: str,
    error_source: str,
    error_step: str
) -> dict:
    """
    Analyzes payment failure context using local Ollama model (or Gemini SDK fallback)
    in compliance with RBI digital payment guidelines.
    Returns dict containing 'decision' and 'reasoning'.
    """
    prompt = f"""
    You are an elite Reclaim AI Payment Recovery Agent operating under strict Reserve Bank of India (RBI) digital payment regulations. 
    Analyze this failed transaction context:
    - PAYMENT_ID: {payment_id}
    - AMOUNT_INR: {rupees}
    - ERROR_CODE: "{error_code}"
    - ERROR_REASON: "{error_reason}"
    - ERROR_DESCRIPTION: "{error_description}"
    - ERROR_SOURCE: "{error_source}"
    - ERROR_STEP: "{error_step}"

    RBI GUIDELINES & COMPLIANCE CHECK:
    - Review RBI mandates on electronic payment transactions, authentication failures, and customer grievance redressal.
    - Ensure retry mechanisms do not violate RBI auto-debit/mandate or friction-reduction frameworks.
    - If funds are insufficient, avoid spamming the customer with active payment links to prevent harassment norms violation under fair practice codes.

    AGENT RULES:
    1. If ERROR_REASON is "insufficient_funds" or source is "customer" due to lack of money: Action MUST BE "SCHEDULE_PAYDAY_RETRY".
    2. If ERROR_REASON is "gateway_technical_error" or source is "bank" (network/timeout issues): Action MUST BE "GENERATE_NEW_LINK".
    3. If ERROR_REASON is "payment_risk_check_failed" or suspected fraud: Action MUST BE "ABANDON".

    Return ONLY a JSON object with these keys:
    - "decision": Your chosen action based on the rules and RBI compliance check.
    - "reasoning": A brief explanation of your diagnostic logic citing relevant operational safety or RBI considerations.
    """

    try:
        ollama_response = requests.post(
            OLLAMA_URL,
            json={
                "model": "phi4-mini",
                "prompt": prompt,
                "stream": False,
                "format": "json",
                "options": {
                    "num_ctx": 2048,
                    "num_predict": 100,
                    "temperature": 0.1
                }
            },
            timeout=15
        )
        result_data = ollama_response.json()
        ai_decision_text = result_data.get("response", "{}")
        
        ai_decision = json.loads(ai_decision_text)
        decision = ai_decision.get("decision", "GENERATE_NEW_LINK")
        reasoning = ai_decision.get("reasoning", "No reasoning provided")
        print(f"[LOCAL AI AGENT] Decision: {decision} | Reason: {reasoning}")
        return {"decision": decision, "reasoning": reasoning}
        
    except Exception as e:
        print(f"[OLLAMA ERROR] Local AI failed or timed out: {e}")
        return {"decision": "GENERATE_NEW_LINK", "reasoning": f"Fallback due to local AI timeout: {e}"}


def analyze_with_ai(prompt: str) -> dict:
    """
    Lightweight AI analysis accepting a raw prompt string.
    Used by the router's Smart-Path for gateway error classification.
    Falls back to GENERATE_NEW_LINK on any failure.
    """
    print(f"[OLLAMA AI] Sending prompt to Ollama ({OLLAMA_URL}) model={OLLAMA_MODEL}...")
    t0 = time.time()
    try:
        ollama_response = requests.post(
            OLLAMA_URL,
            json={
                "model": OLLAMA_MODEL,
                "prompt": prompt,
                "stream": False,
                "format": "json",
                "options": {
                    "num_ctx": 2048,
                    "num_predict": 100,
                    "temperature": 0.1
                }
            },
            timeout=10
        )
        elapsed = time.time() - t0
        print(f"[OLLAMA AI] Response received in {elapsed:.2f}s. HTTP status={ollama_response.status_code}")
        result_data = ollama_response.json()
        ai_decision_text = result_data.get("response", "{}")
        print(f"[OLLAMA AI] Raw response text: {ai_decision_text[:200]}")
        ai_decision = json.loads(ai_decision_text)
        decision = ai_decision.get("decision", "GENERATE_NEW_LINK")
        reasoning = ai_decision.get("reasoning", "No reasoning provided")
        print(f"[OLLAMA AI] Parsed decision: '{decision}' | Reasoning: {reasoning}")
        return {
            "decision": decision,
            "reasoning": reasoning
        }
    except Exception as e:
        elapsed = time.time() - t0
        print(f"[OLLAMA FALLBACK] analyze_with_ai failed after {elapsed:.2f}s: {e}")
        return {"decision": "GENERATE_NEW_LINK", "reasoning": f"Fallback on local AI failure: {e}"}
