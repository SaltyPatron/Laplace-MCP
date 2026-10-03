/* Laplace-MCP HTTP API. The caller sends content or an ID. This process maps tier 0,
 * names the entity, and asks the Laplace database. The caller is not linked to Postgres. */
#ifndef LPM_API_H
#define LPM_API_H

int api_serve(const char *host, int port);

#endif
