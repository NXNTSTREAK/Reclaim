from api_bridge.main import app
from api_bridge.config import API_HOST, API_PORT

if __name__ == "__main__":
    import uvicorn
    uvicorn.run("api_bridge.main:app", host=API_HOST, port=API_PORT, reload=True)
