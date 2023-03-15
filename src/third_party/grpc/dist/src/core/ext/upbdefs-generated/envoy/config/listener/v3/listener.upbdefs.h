/* This file was generated by upbc (the upb compiler) from the input
 * file:
 *
 *     envoy/config/listener/v3/listener.proto
 *
 * Do not edit -- your changes will be discarded when the file is
 * regenerated. */

#ifndef ENVOY_CONFIG_LISTENER_V3_LISTENER_PROTO_UPBDEFS_H_
#define ENVOY_CONFIG_LISTENER_V3_LISTENER_PROTO_UPBDEFS_H_

#include "upb/def.h"
#include "upb/port_def.inc"
#ifdef __cplusplus
extern "C" {
#endif

#include "upb/def.h"

#include "upb/port_def.inc"

extern _upb_DefPool_Init envoy_config_listener_v3_listener_proto_upbdefinit;

UPB_INLINE const upb_MessageDef *envoy_config_listener_v3_ListenerCollection_getmsgdef(upb_DefPool *s) {
  _upb_DefPool_LoadDefInit(s, &envoy_config_listener_v3_listener_proto_upbdefinit);
  return upb_DefPool_FindMessageByName(s, "envoy.config.listener.v3.ListenerCollection");
}

UPB_INLINE const upb_MessageDef *envoy_config_listener_v3_Listener_getmsgdef(upb_DefPool *s) {
  _upb_DefPool_LoadDefInit(s, &envoy_config_listener_v3_listener_proto_upbdefinit);
  return upb_DefPool_FindMessageByName(s, "envoy.config.listener.v3.Listener");
}

UPB_INLINE const upb_MessageDef *envoy_config_listener_v3_Listener_DeprecatedV1_getmsgdef(upb_DefPool *s) {
  _upb_DefPool_LoadDefInit(s, &envoy_config_listener_v3_listener_proto_upbdefinit);
  return upb_DefPool_FindMessageByName(s, "envoy.config.listener.v3.Listener.DeprecatedV1");
}

UPB_INLINE const upb_MessageDef *envoy_config_listener_v3_Listener_ConnectionBalanceConfig_getmsgdef(upb_DefPool *s) {
  _upb_DefPool_LoadDefInit(s, &envoy_config_listener_v3_listener_proto_upbdefinit);
  return upb_DefPool_FindMessageByName(s, "envoy.config.listener.v3.Listener.ConnectionBalanceConfig");
}

UPB_INLINE const upb_MessageDef *envoy_config_listener_v3_Listener_ConnectionBalanceConfig_ExactBalance_getmsgdef(upb_DefPool *s) {
  _upb_DefPool_LoadDefInit(s, &envoy_config_listener_v3_listener_proto_upbdefinit);
  return upb_DefPool_FindMessageByName(s, "envoy.config.listener.v3.Listener.ConnectionBalanceConfig.ExactBalance");
}

UPB_INLINE const upb_MessageDef *envoy_config_listener_v3_Listener_InternalListenerConfig_getmsgdef(upb_DefPool *s) {
  _upb_DefPool_LoadDefInit(s, &envoy_config_listener_v3_listener_proto_upbdefinit);
  return upb_DefPool_FindMessageByName(s, "envoy.config.listener.v3.Listener.InternalListenerConfig");
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#include "upb/port_undef.inc"

#endif  /* ENVOY_CONFIG_LISTENER_V3_LISTENER_PROTO_UPBDEFS_H_ */
