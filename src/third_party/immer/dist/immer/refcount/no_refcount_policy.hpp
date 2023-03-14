//
// immer: immutable data structures for C++
// Copyright (C) 2016, 2017, 2018 Juan Pedro Bolivar Puente
//
// This software is distributed under the Boost Software License, Version 1.0.
// See accompanying file LICENSE or copy at http://boost.org/LICENSE_1_0.txt
//

#pragma once

namespace immer {

struct disowned
{};

/*!
 * Disables reference counting, to be used with an alternative garbage
 * collection strategy like a `gc_heap`.
 */
struct no_refcount_policy
{
    no_refcount_policy(){};
    no_refcount_policy(disowned) {}

    void inc() {}
    bool dec() { return false; }
    bool unique() { return false; }
};

} // namespace immer
