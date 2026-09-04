import os

def load_dotenv(dotenv_path: str = None):
    """
    Loads environment variables from a .env file into os.environ.
    Preserves existing environment variables if already set.
    """
    if dotenv_path is None:
        project_root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
        dotenv_path = os.path.join(project_root, ".env")
        if not os.path.exists(dotenv_path):
            dotenv_path = os.path.join(os.getcwd(), ".env")

    if os.path.exists(dotenv_path):
        try:
            with open(dotenv_path, "r", encoding="utf-8") as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("#") or "=" not in line:
                        continue
                    key, val = line.split("=", 1)
                    key = key.strip()
                    val = val.strip().strip("'\"")
                    if key and key not in os.environ:
                        os.environ[key] = val
        except Exception as e:
            print(f"[CONFIG WARNING] Could not load .env file: {e}")

# Automatically load environment variables from .env file on module import
load_dotenv()

# Razorpay API Credentials
RAZORPAY_KEY_ID = os.getenv("RAZORPAY_KEY_ID", "")
RAZORPAY_KEY_SECRET = os.getenv("RAZORPAY_KEY_SECRET", "")

# Gemini API Credentials
GEMINI_API_KEY = os.getenv("GEMINI_API_KEY", "")

# Email Settings
SENDER_EMAIL = os.getenv("SENDER_EMAIL", "")
SENDER_PASSWORD = os.getenv("SENDER_PASSWORD", "")
RECEIVER_EMAIL = os.getenv("RECEIVER_EMAIL", "")
SMTP_SERVER = os.getenv("SMTP_SERVER", "smtp.gmail.com")
SMTP_PORT = int(os.getenv("SMTP_PORT", "465"))

# Ollama Local AI Settings
OLLAMA_URL = os.getenv("OLLAMA_URL", "http://localhost:11434/api/generate")
OLLAMA_MODEL = os.getenv("OLLAMA_MODEL", "phi4-mini")

# Server & Database Configuration
API_HOST = os.getenv("API_HOST", "0.0.0.0")
API_PORT = int(os.getenv("API_PORT", "8000"))
UDP_PORT = int(os.getenv("UDP_PORT", "9001"))
DB_PATH = os.getenv("DB_PATH", "recovery_engine.db")