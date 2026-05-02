#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "node.h"

Algorithm parse_algorithm(const char *name) {
    if (strcmp(name, "tahoe") == 0) return ALG_TAHOE;
    if (strcmp(name, "reno") == 0) return ALG_RENO;
    if (strcmp(name, "newreno") == 0) return ALG_NEWRENO;
    return ALG_UNKNOWN;
}

const char *algorithm_name(Algorithm algorithm) {
    switch (algorithm) {
        case ALG_TAHOE: return "TCP Tahoe";
        case ALG_RENO: return "TCP Reno";
        case ALG_NEWRENO: return "TCP NewReno";
        default: return "Unknown";
    }
}

const char *state_name(CCState state) {
    switch (state) {
        case SLOW_START: return "Slow Start";
        case CONGESTION_AVOIDANCE: return "Congestion Avoidance";
        case FAST_RECOVERY: return "Fast Recovery";
        default: return "Unknown";
    }
}

int load_config(const char *filename, NodeConfig *config) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        return 0;
    }

    char line[MAX_LINE];

    config->neighbor_count = 0;

    /*
       First non-comment line:
       NodeID Port

       Example:
       A 5001
    */
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        if (sscanf(line, " %c %d", &config->node_id, &config->port) == 2) {
            break;
        }
    }

    /*
       Remaining non-comment lines:
       NeighborID IP Port Cost

       Example:
       B 127.0.0.1 5002 4
    */
    while (fgets(line, sizeof(line), fp)) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }

        if (config->neighbor_count >= MAX_NEIGHBORS) {
            printf("Too many neighbors. Increase MAX_NEIGHBORS.\n");
            fclose(fp);
            return 0;
        }

        Neighbor *n = &config->neighbors[config->neighbor_count];

        if (sscanf(line, " %c %63s %d %d", &n->id, n->ip, &n->port, &n->cost) == 4) {
            config->neighbor_count++;
        }
    }

    fclose(fp);
    return 1;
}

void init_tcp(TCPState *tcp, Algorithm algorithm) {
    tcp->algorithm = algorithm;
    tcp->state = SLOW_START;
    tcp->cwnd = 1.0;
    tcp->ssthresh = 16.0;
    tcp->dup_ack_count = 0;
    tcp->round = 0;
}

static void print_tcp_status(TCPState *tcp, const char *event, int ack_no, const char *note) {
    printf("%5d | %-10s | ACK=%-3d | cwnd=%5.2f | ssthresh=%5.2f | %-22s | %s\n",
           tcp->round,
           event,
           ack_no,
           tcp->cwnd,
           tcp->ssthresh,
           state_name(tcp->state),
           note);
}

void on_ack(TCPState *tcp, int ack_no) {
    tcp->round++;
    tcp->dup_ack_count = 0;

    if (tcp->state == SLOW_START) {
        tcp->cwnd += 1.0;

        if (tcp->cwnd >= tcp->ssthresh) {
            tcp->state = CONGESTION_AVOIDANCE;
        }

        print_tcp_status(tcp, "ACK", ack_no, "new ACK received; cwnd increased");
    }
    else if (tcp->state == CONGESTION_AVOIDANCE) {
        tcp->cwnd += 1.0 / tcp->cwnd;
        print_tcp_status(tcp, "ACK", ack_no, "new ACK received; additive increase");
    }
    else if (tcp->state == FAST_RECOVERY) {
        /*
           Reno:
           A full ACK exits Fast Recovery.

           NewReno:
           Students may extend this part:
           - partial ACK: retransmit next missing segment and stay in Fast Recovery
           - full ACK: exit Fast Recovery
        */
        tcp->cwnd = tcp->ssthresh;
        tcp->state = CONGESTION_AVOIDANCE;
        print_tcp_status(tcp, "ACK", ack_no, "full ACK; exit Fast Recovery");
    }
}

void on_duplicate_ack(TCPState *tcp, int ack_no) {
    tcp->round++;
    tcp->dup_ack_count++;

    if (tcp->dup_ack_count < 3) {
        print_tcp_status(tcp, "DUPACK", ack_no, "duplicate ACK received; waiting");
        return;
    }

    /*
       Three duplicate ACKs indicate likely packet loss.
    */
    tcp->ssthresh = tcp->cwnd / 2.0;
    if (tcp->ssthresh < 2.0) {
        tcp->ssthresh = 2.0;
    }

    if (tcp->algorithm == ALG_TAHOE) {
        tcp->cwnd = 1.0;
        tcp->state = SLOW_START;
        print_tcp_status(tcp, "DUPACK", ack_no, "3 duplicate ACKs; Tahoe resets cwnd");
    }
    else if (tcp->algorithm == ALG_RENO) {
        tcp->cwnd = tcp->ssthresh + 3.0;
        tcp->state = FAST_RECOVERY;
        print_tcp_status(tcp, "DUPACK", ack_no, "3 duplicate ACKs; Reno Fast Recovery");
    }
    else if (tcp->algorithm == ALG_NEWRENO) {
        tcp->cwnd = tcp->ssthresh + 3.0;
        tcp->state = FAST_RECOVERY;
        print_tcp_status(tcp, "DUPACK", ack_no, "3 duplicate ACKs; NewReno Fast Recovery");
    }
}

void on_timeout(TCPState *tcp) {
    tcp->round++;

    tcp->ssthresh = tcp->cwnd / 2.0;
    if (tcp->ssthresh < 2.0) {
        tcp->ssthresh = 2.0;
    }

    tcp->cwnd = 1.0;
    tcp->dup_ack_count = 0;
    tcp->state = SLOW_START;

    print_tcp_status(tcp, "TIMEOUT", 0, "timeout; cwnd reset to 1");
}

static void print_config(NodeConfig *config) {
    int i;

    printf("Node %c listening on port %d\n", config->node_id, config->port);
    printf("Neighbors:\n");

    for (i = 0; i < config->neighbor_count; i++) {
        printf("  %c %s %d cost=%d\n",
               config->neighbors[i].id,
               config->neighbors[i].ip,
               config->neighbors[i].port,
               config->neighbors[i].cost);
    }
}

static int run_event_file(const char *event_file, TCPState *tcp) {
    FILE *fp = fopen(event_file, "r");
    if (!fp) {
        printf("Could not open event file: %s\n", event_file);
        return 0;
    }

    char event[32];
    int ack_no;

    printf("\nCongestion-control trace\n");
    printf("Round | Event      | ACK    | cwnd       | ssthresh   | State                  | Explanation\n");
    printf("------------------------------------------------------------------------------------------------\n");

    while (fscanf(fp, "%31s", event) == 1) {
        if (strcmp(event, "ACK") == 0) {
            if (fscanf(fp, "%d", &ack_no) != 1) {
                printf("Malformed ACK event.\n");
                fclose(fp);
                return 0;
            }
            on_ack(tcp, ack_no);
        }
        else if (strcmp(event, "DUPACK") == 0) {
            if (fscanf(fp, "%d", &ack_no) != 1) {
                printf("Malformed DUPACK event.\n");
                fclose(fp);
                return 0;
            }
            on_duplicate_ack(tcp, ack_no);
        }
        else if (strcmp(event, "TIMEOUT") == 0) {
            on_timeout(tcp);
        }
        else {
            printf("Unknown event: %s\n", event);
            fclose(fp);
            return 0;
        }
    }

    fclose(fp);
    return 1;
}

int main(int argc, char *argv[]) {
    NodeConfig config;
    TCPState tcp;

    /*
       For simplicity:
       ./node A.conf reno events/example_events.txt

       Students may also hard-code their selected algorithm if the instructor prefers:
       Algorithm selected = ALG_RENO;
    */
    if (argc != 4) {
        printf("Usage: ./node <config_file> <algorithm> <event_file>\n");
        printf("Example: ./node A.conf reno events/example_events.txt\n");
        printf("Algorithms: tahoe, reno, newreno\n");
        return 1;
    }

    Algorithm selected = parse_algorithm(argv[2]);
    if (selected == ALG_UNKNOWN) {
        printf("Invalid algorithm. Use: tahoe, reno, or newreno\n");
        return 1;
    }

    if (!load_config(argv[1], &config)) {
        printf("Could not load config file: %s\n", argv[1]);
        return 1;
    }

    print_config(&config);

    init_tcp(&tcp, selected);

    printf("\nSelected congestion control algorithm: %s\n", algorithm_name(selected));
    printf("Initial cwnd=%.2f, ssthresh=%.2f\n", tcp.cwnd, tcp.ssthresh);

    if (!run_event_file(argv[3], &tcp)) {
        return 1;
    }

    return 0;
}
