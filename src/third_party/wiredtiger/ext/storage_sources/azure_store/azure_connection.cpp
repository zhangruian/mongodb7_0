/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "azure_connection.h"

// Includes necessary for connection
#include <azure/core.hpp>
#include <azure/storage/blobs.hpp>

#include <filesystem>
#include <iostream>

azure_connection::azure_connection(const std::string &bucket_name, const std::string &obj_prefix)
    : _azure_client(Azure::Storage::Blobs::BlobContainerClient::CreateFromConnectionString(
        std::getenv("AZURE_STORAGE_CONNECTION_STRING"), bucket_name)),
      _bucket_name(bucket_name), _object_prefix(obj_prefix)
{
}

// Build a list of all of the objects in the bucket.
int
azure_connection::list_objects(
  const std::string &prefix, std::vector<std::string> &objects, bool list_single) const
{
    Azure::Storage::Blobs::ListBlobsOptions blob_parameters;
    blob_parameters.Prefix = prefix;
    // If list_single is true, set the maximum number of returned blobs in the list_blob_response to
    // one.
    if (list_single)
        blob_parameters.PageSizeHint = 1;

    auto list_blobs_response = _azure_client.ListBlobs(blob_parameters);

    for (const auto blob_item : list_blobs_response.Blobs) {
        objects.push_back(blob_item.Name);
    }
    return 0;
}

// Puts an object into the cloud storage using the prefix and file name.
int
azure_connection::put_object(const std::string &object_key, const std::string &file_path) const
{
    auto blob_client = _azure_client.GetBlockBlobClient(_object_prefix + object_key);
    // UploadFrom will always return a UploadBlockBlobFromResult describing the state of the updated
    // block blob so there's no need to check for errors.
    blob_client.UploadFrom(file_path);
    return 0;
}

int
azure_connection::delete_object() const
{
    return 0;
}

int
azure_connection::get_object(const std::string &path) const
{
    return 0;
}
