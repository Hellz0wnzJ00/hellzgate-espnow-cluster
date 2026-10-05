// the real espnow code built as the master

#include "../main/espnow.c"

esp_err_t m_init(void) { scan_mode_init(); return espnow_init(); }
uint32_t m_collect(hg_record_t *out, uint32_t max) { return espnow_collect(out, max); }
const hg_node_info *m_node(uint8_t id) { return &nodes[id]; }

// a master reboot, what it knew about every node gone
void m_reboot(void)
{
    memset(nodes, 0, sizeof nodes);
}
