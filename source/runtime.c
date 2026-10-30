#include "runtime.h"
#include "anvil.h"
int server_view_distance=2, server_max_clients=2, server_chunk_interval_us=10000;
int server_circuit_nodes=16, server_circuit_budget_us=1000;
void runtime_configure(int new_3ds) {
  server_circuit_nodes=new_3ds?64:16;
  server_circuit_budget_us=new_3ds?2500:1000;
  server_view_distance=new_3ds?4:2;
  server_max_clients=new_3ds?4:2;
  server_chunk_interval_us=new_3ds?3000:10000;
  anvil_cache_configure(new_3ds?12:4);
}
