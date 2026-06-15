#!/bin/bash

# Exit immediately if a command exits with a non-zero status
set -e

echo "=== 1. Checking and installing dependencies ==="

# Check if git is installed
if ! command -v git &> /dev/null; then
    echo "git is not installed. Installing..."
    sudo apt-get update && sudo apt-get install -y git
else
    echo "git is already installed."
fi

# Check if compiler tools are missing
if ! command -v g++ &> /dev/null || ! command -v make &> /dev/null; then
    echo "Compiler tools (g++ or make) not found. Installing build-essential..."
    sudo apt-get update && sudo apt-get install -y build-essential
else
    echo "g++ and make are already installed."
fi

echo "=== 2. Building the project ==="
make clean || true
make all

# Verify executable exists
if [ ! -f "./server" ]; then
    echo "Error: Build failed, 'server' executable not found."
    exit 1
fi

echo "=== 3. Starting the server in background ==="
# Check if a server is already running on port 8080 and kill it gracefully
EXISTING_PID=$(lsof -t -i:8080 || true)
if [ -n "$EXISTING_PID" ]; then
    echo "Stopping existing server running on port 8080 (PID: $EXISTING_PID)..."
    kill -15 "$EXISTING_PID" || true
    sleep 2 # wait for graceful shutdown
fi

# Launch server safely and redirect output
nohup ./server > server.log 2>&1 &
SERVER_PID=$!

echo "=== Deployment successful! ==="
echo "Server started safely in the background with PID: $SERVER_PID"
echo ""
echo "--------------------------------------------------------"
echo "                   MANUAL INSTRUCTIONS                  "
echo "--------------------------------------------------------"
echo " STARTUP:      ./deploy.sh (This script safely restarts it)"
echo " SHUTDOWN:     kill -15 \$(lsof -t -i:8080) (Graceful exit)"
echo " LOG VIEWING:  tail -f server.log"
echo "--------------------------------------------------------"
