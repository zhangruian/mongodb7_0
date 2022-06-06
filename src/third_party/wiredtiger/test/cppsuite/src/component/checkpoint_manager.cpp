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

#include "checkpoint_manager.h"

#include "src/common/api_const.h"
#include "src/common/logger.h"
#include "src/main/configuration.h"
#include "src/storage/connection_manager.h"

extern "C" {
#include "test_util.h"
}

namespace test_harness {
checkpoint_manager::checkpoint_manager(configuration *configuration)
    : component(CHECKPOINT_MANAGER, configuration)
{
}

void
checkpoint_manager::load()
{
    /* Load the general component things. */
    component::load();

    /* Create session that we'll use for checkpointing. */
    if (_enabled)
        _session = connection_manager::instance().create_session();
}

void
checkpoint_manager::do_work()
{
    logger::log_msg(LOG_INFO, "Running checkpoint");
    testutil_check(_session->checkpoint(_session.get(), nullptr));
}
} // namespace test_harness
