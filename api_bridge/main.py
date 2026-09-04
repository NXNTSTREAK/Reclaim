import os
from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from api_bridge.routers import recovery, checkout

app = FastAPI(
    title="Reclaim - AI Revenue Recovery API Bridge",
    description="Modular API Bridge for Reclaim AI-driven revenue recovery workflow",
    version="1.0.0"
)

app.include_router(recovery.router)
app.include_router(checkout.router)

FRONTEND_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "frontend"))
INDEX_FILE = os.path.join(FRONTEND_DIR, "index.html")
SIMULATOR_FILE = os.path.join(FRONTEND_DIR, "simulator.html")

if os.path.exists(FRONTEND_DIR):
    app.mount("/static", StaticFiles(directory=FRONTEND_DIR), name="static")

@app.get("/")
async def serve_dashboard():
    if os.path.exists(INDEX_FILE):
        return FileResponse(INDEX_FILE)
    return {"message": "Reclaim AI Revenue Recovery Engine API Bridge is running."}

@app.get("/simulator")
@app.get("/simulator.html")
async def serve_simulator():
    if os.path.exists(SIMULATOR_FILE):
        return FileResponse(SIMULATOR_FILE)
    return {"message": "simulator.html not found"}

if __name__ == "__main__":
    import uvicorn
    from api_bridge.config import API_HOST, API_PORT
    uvicorn.run("api_bridge.main:app", host=API_HOST, port=API_PORT, reload=True)
