#ifndef gdos_kring_cap_h_INCLUDED
#define gdos_kring_cap_h_INCLUDED

#include <stdint.h>

#include <gdosabi/kring.h>

#define KSCHEME_CAP ((int64_t)-2)

#define CAP_TOKEN_VERSION 1u
#define CAP_TOKEN_SIZE 32u
#define KCAP_MAX_DEPTH 64u

#define KCAP_GRANT_DEVMEM    1u
#define KCAP_GRANT_IRQ_ROUTE 2u
#define KCAP_GRANT_IOMMU_DEV 3u

struct cap_token {
  uint8_t version;
  uint8_t nres;
  uint8_t reserved[6];
  uint64_t grant_id_le;
  uint8_t mac[16];
};

static_assert(sizeof(struct cap_token) == CAP_TOKEN_SIZE,
              "capability token ABI size");

#define KCAP_PARAM_P0 1u
#define KCAP_PARAM_P1 2u
#define KCAP_PARAM_P2 4u

struct kcap_subgrant_req {
  uint64_t token_off;
  uint64_t token_len;
  uint64_t p0;
  uint64_t p1;
  uint64_t p2;
  uint32_t present;
  uint32_t reserved;
  uint64_t reserved2;
};

struct kcap_query_req {
  uint64_t token_off;
  uint64_t token_len;
};

struct kcap_query_result {
  uint64_t grant_id;
  uint64_t p0;
  uint64_t p1;
  uint64_t p2;
  uint32_t type;
  uint32_t depth;
  uint32_t live;
  uint32_t wildcard;
};

#define KCAP_SUBGRANT 1u // a=req offset, b=reserved zero, c=token output offset
#define KCAP_REVOKE   2u // a=token offset, b=token length
#define KCAP_QUERY    3u // a=query request offset, c=query result offset

#endif // gdos_kring_cap_h_INCLUDED
