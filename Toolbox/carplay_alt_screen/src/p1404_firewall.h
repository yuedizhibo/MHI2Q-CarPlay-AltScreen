/* p1404_firewall.h - runtime-scoped CarPlay type-111 PF aperture. */
#ifndef P1404_FIREWALL_H
#define P1404_FIREWALL_H
#include <stddef.h>
#include <stdint.h>
/* Value-only cleanup ownership: no receiver/ScreenSession pointer survives. */
struct p1404_pf_lease { uint16_t port; uint32_t generation; };
int p1404_alt111_firewall_open(uint16_t port);
int p1404_alt111_firewall_close(uint16_t port);
int p1404_alt111_firewall_open_owned(uint16_t port, struct p1404_pf_lease *lease);
int p1404_alt111_firewall_close_owned(const struct p1404_pf_lease *lease);
int p1404_alt111_firewall_rewrite(const char *rules, uint16_t port, int enable,
                                  char *out, size_t out_cap, int *changed_out);
#endif
