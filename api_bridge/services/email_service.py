import smtplib
from email.mime.text import MIMEText
from api_bridge.config import SENDER_EMAIL, SENDER_PASSWORD, RECEIVER_EMAIL, SMTP_SERVER, SMTP_PORT

def send_recovery_email(payment_id: str, amount_inr: float, recovery_link: str, recipient_email: str = None) -> bool:
    """
    Sends a payment recovery email to the customer using Gmail SMTP SSL.
    """
    to_email = recipient_email or RECEIVER_EMAIL
    subject = f"Action Required: Complete your payment of ₹{amount_inr:.2f}"
    body = f"""
    Hello,
    
    We noticed your recent payment ({payment_id}) failed due to a bank timeout. 
    Don't worry, your cart is saved! 
    
    Click the secure link below to complete your purchase:
    {recovery_link}
    
    Best,
    Reclaim AI Recovery Team
    """
    
    msg = MIMEText(body)
    msg['Subject'] = subject
    msg['From'] = SENDER_EMAIL
    msg['To'] = to_email

    try:
        with smtplib.SMTP_SSL(SMTP_SERVER, SMTP_PORT, timeout=5) as server:
            server.login(SENDER_EMAIL, SENDER_PASSWORD)
            server.send_message(msg)
        print(f"[DELIVERY] Recovery email successfully sent to {to_email}")
        return True
    except Exception as e:
        print(f"[DELIVERY ERROR] Failed to send email: {e}")
        return False
