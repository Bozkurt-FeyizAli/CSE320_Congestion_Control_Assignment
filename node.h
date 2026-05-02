#ifndef NODE_H
#define NODE_H

#define MAX_NEIGHBORS 16
#define MAX_LINE 256

typedef enum {
    ALG_TAHOE = 0,
    ALG_RENO,
    ALG_NEWRENO,
    ALG_UNKNOWN
} Algorithm;

typedef enum {
    SLOW_START = 0,
    CONGESTION_AVOIDANCE,
    FAST_RECOVERY
} CCState;

typedef struct {
    char id;
    char ip[64];
    int port;
    int cost;
} Neighbor;

typedef struct {
    char node_id;
    int port;
    Neighbor neighbors[MAX_NEIGHBORS];
    int neighbor_count;
} NodeConfig;

typedef struct {
    Algorithm algorithm;
    CCState state;
    double cwnd;
    double ssthresh;
    int dup_ack_count;
    int round;
    int last_ack; 
} TCPState;

int load_config(const char *filename, NodeConfig *config);

Algorithm parse_algorithm(const char *name);
const char *algorithm_name(Algorithm algorithm);
const char *state_name(CCState state);

void init_tcp(TCPState *tcp, Algorithm algorithm);
void on_ack(TCPState *tcp, int ack_no);
void on_duplicate_ack(TCPState *tcp, int ack_no);
void on_timeout(TCPState *tcp);

#endif
