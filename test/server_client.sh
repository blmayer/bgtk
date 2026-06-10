#!/bin/bash

bgce > bgce.log 2>&1 &
sleep 1
echo "[TEST] Server running"

echo "[TEST] Running test client..."
./app > app.log 2>&1

sleep 2
echo "[TEST] Stopping server..."
killall bgce

echo "[TEST] Test finished."

