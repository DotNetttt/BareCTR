#ifndef RUNTIME_H
#define RUNTIME_H
extern int server_view_distance, server_max_clients, server_chunk_interval_us;
extern int server_circuit_nodes, server_circuit_budget_us;
void runtime_configure(int new_3ds);
#endif
