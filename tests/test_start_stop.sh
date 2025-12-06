#!/bin/bash

ITERATIONS=1000
for ((i=1; i<=ITERATIONS; i++)); do
    echo
    echo "--- Iteration $i/$ITERATIONS ---"

    curl -X POST http://localhost:8090/stream/start      -H "Content-Type: application/json" -d '{"stream_id":"cam01"}'   	 
    sleep 40
     echo  ":\n" 	
 
    curl -X POST http://localhost:8090/stream/stop -H "Content-Type: application/json" -d '{"stream_id":"cam01"}' 
    sleep 20
    
done
