/*
 * Copyright 2022-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <bson/bson.h>

#include "mc-fle2-find-range-payload-private-v2.h"
#include "mongocrypt.h"
#include "mongocrypt-buffer-private.h"

void
mc_FLE2FindRangePayloadV2_init (mc_FLE2FindRangePayloadV2_t *payload)
{
   BSON_ASSERT_PARAM (payload);
   *payload = (mc_FLE2FindRangePayloadV2_t){{{{0}}}};
   _mc_array_init (&payload->payload.value.edgeFindTokenSetArray,
                   sizeof (mc_EdgeFindTokenSetV2_t));
}

static void
mc_EdgeFindTokenSetV2_cleanup (mc_EdgeFindTokenSetV2_t *etc)
{
   if (!etc) {
      return;
   }
   _mongocrypt_buffer_cleanup (&etc->edcDerivedToken);
   _mongocrypt_buffer_cleanup (&etc->escDerivedToken);
   _mongocrypt_buffer_cleanup (&etc->serverDerivedFromDataToken);
}

void
mc_FLE2FindRangePayloadV2_cleanup (mc_FLE2FindRangePayloadV2_t *payload)
{
   if (!payload) {
      return;
   }
   // Free all EdgeFindTokenSet entries.
   for (size_t i = 0; i < payload->payload.value.edgeFindTokenSetArray.len;
        i++) {
      mc_EdgeFindTokenSetV2_t entry =
         _mc_array_index (&payload->payload.value.edgeFindTokenSetArray,
                          mc_EdgeFindTokenSetV2_t,
                          i);
      mc_EdgeFindTokenSetV2_cleanup (&entry);
   }
   _mc_array_destroy (&payload->payload.value.edgeFindTokenSetArray);
}

#define APPEND_BINDATA(out, name, subtype, value)              \
   if (!_mongocrypt_buffer_append (&(value), out, name, -1)) { \
      return false;                                            \
   }

bool
mc_FLE2FindRangePayloadV2_serialize (const mc_FLE2FindRangePayloadV2_t *payload,
                                   bson_t *out)
{
   BSON_ASSERT_PARAM (out);
   BSON_ASSERT_PARAM (payload);

   // Append "payload" if this is not a stub.
   if (payload->payload.set) {
      bson_t payload_bson;

      if (!BSON_APPEND_DOCUMENT_BEGIN (out, "payload", &payload_bson)) {
         return false;
      }
      // Append "payload.g" array of EdgeTokenSets.
      bson_t g_bson;
      if (!BSON_APPEND_ARRAY_BEGIN (&payload_bson, "g", &g_bson)) {
         return false;
      }

      uint32_t g_index = 0;
      for (size_t i = 0; i < payload->payload.value.edgeFindTokenSetArray.len;
           i++) {
         mc_EdgeFindTokenSetV2_t etc =
            _mc_array_index (&payload->payload.value.edgeFindTokenSetArray,
                             mc_EdgeFindTokenSetV2_t,
                             i);
         bson_t etc_bson;

         const char *g_index_string;
         char storage[16];
         bson_uint32_to_string (
            g_index, &g_index_string, storage, sizeof (storage));

         if (!BSON_APPEND_DOCUMENT_BEGIN (&g_bson, g_index_string, &etc_bson)) {
            return false;
         }

         APPEND_BINDATA (
            &etc_bson, "d", BSON_SUBTYPE_BINARY, etc.edcDerivedToken);
         APPEND_BINDATA (
            &etc_bson, "s", BSON_SUBTYPE_BINARY, etc.escDerivedToken);
         APPEND_BINDATA (
            &etc_bson, "l", BSON_SUBTYPE_BINARY, etc.serverDerivedFromDataToken);

         if (!bson_append_document_end (&g_bson, &etc_bson)) {
            return false;
         }
         if (g_index == UINT32_MAX)
            break;
         g_index++;
      }

      if (!bson_append_array_end (&payload_bson, &g_bson)) {
         return false;
      }

      // Append "payload.cm".
      if (!BSON_APPEND_INT64 (&payload_bson,
                              "cm",
                              payload->payload.value.maxContentionCounter)) {
         return false;
      }

      if (!bson_append_document_end (out, &payload_bson)) {
         return false;
      }
   }

   // Append "payloadId"
   if (!BSON_APPEND_INT32 (out, "payloadId", payload->payloadId)) {
      return false;
   }

   // Append "firstOperator".
   if (!BSON_APPEND_INT32 (out, "firstOperator", payload->firstOperator)) {
      return false;
   }

   // Append "secondOperator" if present.
   if (payload->secondOperator != FLE2RangeOperator_kNone &&
       !BSON_APPEND_INT32 (out, "secondOperator", payload->secondOperator)) {
      return false;
   }

   return true;
}
#undef APPEND_BINDATA
