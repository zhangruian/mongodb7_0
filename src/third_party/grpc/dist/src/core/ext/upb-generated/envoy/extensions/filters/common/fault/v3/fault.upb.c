/* This file was generated by upbc (the upb compiler) from the input
 * file:
 *
 *     envoy/extensions/filters/common/fault/v3/fault.proto
 *
 * Do not edit -- your changes will be discarded when the file is
 * regenerated. */

#include <stddef.h>
#include "upb/msg.h"
#include "envoy/extensions/filters/common/fault/v3/fault.upb.h"
#include "envoy/type/v3/percent.upb.h"
#include "google/protobuf/duration.upb.h"
#include "udpa/annotations/status.upb.h"
#include "udpa/annotations/versioning.upb.h"
#include "validate/validate.upb.h"

#include "upb/port_def.inc"

static const upb_msglayout *const envoy_extensions_filters_common_fault_v3_FaultDelay_submsgs[3] = {
  &envoy_extensions_filters_common_fault_v3_FaultDelay_HeaderDelay_msginit,
  &envoy_type_v3_FractionalPercent_msginit,
  &google_protobuf_Duration_msginit,
};

static const upb_msglayout_field envoy_extensions_filters_common_fault_v3_FaultDelay__fields[3] = {
  {3, UPB_SIZE(8, 16), UPB_SIZE(-13, -25), 2, 11, 1},
  {4, UPB_SIZE(4, 8), 1, 1, 11, 1},
  {5, UPB_SIZE(8, 16), UPB_SIZE(-13, -25), 0, 11, 1},
};

const upb_msglayout envoy_extensions_filters_common_fault_v3_FaultDelay_msginit = {
  &envoy_extensions_filters_common_fault_v3_FaultDelay_submsgs[0],
  &envoy_extensions_filters_common_fault_v3_FaultDelay__fields[0],
  UPB_SIZE(16, 32), 3, false, 255,
};

const upb_msglayout envoy_extensions_filters_common_fault_v3_FaultDelay_HeaderDelay_msginit = {
  NULL,
  NULL,
  UPB_SIZE(0, 0), 0, false, 255,
};

static const upb_msglayout *const envoy_extensions_filters_common_fault_v3_FaultRateLimit_submsgs[3] = {
  &envoy_extensions_filters_common_fault_v3_FaultRateLimit_FixedLimit_msginit,
  &envoy_extensions_filters_common_fault_v3_FaultRateLimit_HeaderLimit_msginit,
  &envoy_type_v3_FractionalPercent_msginit,
};

static const upb_msglayout_field envoy_extensions_filters_common_fault_v3_FaultRateLimit__fields[3] = {
  {1, UPB_SIZE(8, 16), UPB_SIZE(-13, -25), 0, 11, 1},
  {2, UPB_SIZE(4, 8), 1, 2, 11, 1},
  {3, UPB_SIZE(8, 16), UPB_SIZE(-13, -25), 1, 11, 1},
};

const upb_msglayout envoy_extensions_filters_common_fault_v3_FaultRateLimit_msginit = {
  &envoy_extensions_filters_common_fault_v3_FaultRateLimit_submsgs[0],
  &envoy_extensions_filters_common_fault_v3_FaultRateLimit__fields[0],
  UPB_SIZE(16, 32), 3, false, 255,
};

static const upb_msglayout_field envoy_extensions_filters_common_fault_v3_FaultRateLimit_FixedLimit__fields[1] = {
  {1, UPB_SIZE(0, 0), 0, 0, 4, 1},
};

const upb_msglayout envoy_extensions_filters_common_fault_v3_FaultRateLimit_FixedLimit_msginit = {
  NULL,
  &envoy_extensions_filters_common_fault_v3_FaultRateLimit_FixedLimit__fields[0],
  UPB_SIZE(8, 8), 1, false, 255,
};

const upb_msglayout envoy_extensions_filters_common_fault_v3_FaultRateLimit_HeaderLimit_msginit = {
  NULL,
  NULL,
  UPB_SIZE(0, 0), 0, false, 255,
};

#include "upb/port_undef.inc"

