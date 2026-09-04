# Reclaim — AI Revenue Recovery Engine

An enterprise-grade, high-throughput AI-augmented payment recovery engine engineered for Razorpay payment ecosystems operating under Reserve Bank of India (RBI) digital payment compliance guidelines.

---

## Architectural Highlights

- **High-Throughput C++17 Core Engine (`src/`, `include/`)**:
  - Thread-safe concurrent event queueing with zero-latency UDP datagram ingestion (port 9001).
  - Low-overhead worker thread pool for parallel payment state machine processing.
  - SQLite WAL (Write-Ahead Logging) persistent storage for idempotency and atomic financial ledger journaling.
  - Efficient min-heap timer scheduler for non-blocking payday retries and link expiry enforcement.

- **Asynchronous Python FastAPI Bridge (`api_bridge/`)**:
  - Smart-Path routing for gateway error classification and RBI prompt compliance enforcement.
  - Local LLM inference via Ollama (`phi4-mini`) with fallback integration to Gemini AI SDK.
  - Direct Razorpay API verification and automated payment link generation.
  - Real-time log stream broadcasting via Server-Sent Events (SSE).

- **Dual Frontend Control Planes (`frontend/`)**:
  - **Observability Dashboard (`index.html`)**: Real-time log streams, live financial metrics (`Revenue at Risk` & `Revenue Recovered`), and C++ daemon lifecycle controls.
  - **Traffic Simulator (`simulator.html`)**: High-speed payment failure micro-burst generator (15–20 events/ms) and interactive test harness.

---

## Repository Structure

```
revenue_recovery/
├── api_bridge/              # FastAPI bridge, routers, and services
│   ├── config.py            # Environment configuration loader (.env auto-load)
│   ├── main.py              # Application entry point
│   ├── routers/             # API routes (recovery, checkout)
│   └── services/            # Services (AI, Payment, Email)
├── frontend/                # Dashboard and simulator HTML UI
│   ├── index.html           # Observability Dashboard
│   └── simulator.html       # Traffic Simulator
├── include/                 # C++ header files
├── src/                     # C++ core daemon source implementation
├── CMakeLists.txt           # CMake build configuration
├── .env.example             # Environment variables template
├── .gitignore               # Comprehensive Git ignore rules
└── README.md                # Repository documentation
```

---

## Quick Start

### 1. Prerequisites
- **C++17 Compiler** (`g++` or `clang++`) & `cmake`
- **Python 3.10+**
- **SQLite3 development libraries** (`libsqlite3-dev`)
- **OpenSSL development libraries** (`libssl-dev`)

### 2. Environment Configuration
Copy `.env.example` to `.env` and fill in your API credentials:
```bash
cp .env.example .env
```

### 3. Build the C++ Engine Daemon
```bash
mkdir -p build
cd build
cmake ..
make -j$(nproc)
cd ..
```

### 4. Start the API Bridge & Control Dashboard
```bash
# Create and activate Python virtual environment
python3 -m venv .venv
source .venv/bin/activate

# Install dependencies
pip install fastapi uvicorn razorpay google-genai requests

# Launch FastAPI Bridge
python -m api_bridge.main
```

Access the **Observability Dashboard** at `http://localhost:8000` and the **Traffic Simulator** at `http://localhost:8000/simulator`.

---

## License

MIT License. Designed for high-availability enterprise recovery workflows.
