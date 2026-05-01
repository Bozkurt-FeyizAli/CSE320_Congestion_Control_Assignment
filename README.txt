CSE 320 — Node-Style Congestion Control Template

This template keeps the original network-node configuration format.

Example A.conf:

A 5001
B 127.0.0.1 5002 4
C 127.0.0.1 5003 7
D 127.0.0.1 5004 13
F 127.0.0.1 5006 5

Compile with any C compiler.

Linux / macOS:
gcc node.c -o node

Windows with MinGW:
gcc node.c -o node.exe

Run format:

./node <config_file> <algorithm> <event_file>

Examples:

./node A.conf reno events/example_events.txt
./node B.conf tahoe events/duplicate_ack_test.txt
./node C.conf newreno events/timeout_test.txt

Students must choose ONE algorithm:
- tahoe
- reno
- newreno

Event file format:

ACK <number>
DUPACK <number>
TIMEOUT

Example:

ACK 1
ACK 2
ACK 3
DUPACK 3
DUPACK 3
DUPACK 3
ACK 4
TIMEOUT

What should be visible in console?

The program prints:
- node ID
- node port
- neighbors
- selected congestion control algorithm
- ACK events
- duplicate ACK events
- timeout events
- cwnd changes
- ssthresh changes
- current congestion-control state

Expected style of output:

Node A listening on port 5001
Neighbors:
  B 127.0.0.1 5002 cost=4
  C 127.0.0.1 5003 cost=7

Selected congestion control algorithm: TCP Reno
Initial cwnd=1.00, ssthresh=16.00

Round | Event      | ACK    | cwnd       | ssthresh   | State                  | Explanation
------------------------------------------------------------------------------------------------
1     | ACK        | ACK=1   | cwnd=2.00  | ssthresh=16.00 | Slow Start            | new ACK received; cwnd increased
2     | ACK        | ACK=2   | cwnd=3.00  | ssthresh=16.00 | Slow Start            | new ACK received; cwnd increased
3     | DUPACK     | ACK=3   | cwnd=3.00  | ssthresh=16.00 | Slow Start            | duplicate ACK received; waiting
4     | DUPACK     | ACK=3   | cwnd=3.00  | ssthresh=16.00 | Slow Start            | duplicate ACK received; waiting
5     | DUPACK     | ACK=3   | cwnd=4.50  | ssthresh=1.50  | Fast Recovery         | 3 duplicate ACKs; Reno Fast Recovery

Important:
- Tahoe must reset cwnd to 1 after three duplicate ACKs.
- Reno must enter Fast Recovery after three duplicate ACKs.
- NewReno must behave like Reno at first, but students should improve partial ACK behavior.
