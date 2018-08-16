/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */
#ifndef HEADERUUID_8CAAB40D_AC65_46CF_9FA9_B48825C825DC_DEFINED
#define HEADERUUID_8CAAB40D_AC65_46CF_9FA9_B48825C825DC_DEFINED

// <inttypes.h> is needed to avoid macro redefinition error when compiling on Windows.
// Should be fixed inside mongoc.h
#include <inttypes.h>
#include <mongo/embedded/capi.h>
#include <mongoc.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Creates a client with the correct stream intiator set
 * @param db must be a valid instance handle created by `mongo_embedded_v1_instance_create`
 * @returns a mongoc client or `NULL` on error
 */
mongoc_client_t* mongo_embedded_v1_mongoc_client_create(mongo_embedded_v1_instance* instance);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // HEADERUUID_8CAAB40D_AC65_46CF_9FA9_B48825C825DC_DEFINED
