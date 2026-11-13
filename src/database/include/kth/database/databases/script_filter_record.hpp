// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef KTH_DATABASE_SCRIPT_FILTER_RECORD_HPP_
#define KTH_DATABASE_SCRIPT_FILTER_RECORD_HPP_

#include <kth/infrastructure/hash_define.hpp>
#include <kth/infrastructure/utility/data.hpp>

namespace kth::database {

/// The script filter (compact filter type 1) of one block as it is stored: the
/// bytes served for the block, and the filter header, which commits to those
/// bytes and to the filter header of the parent block.
struct script_filter_record {
    hash_digest block_hash;
    data_chunk filter;
    hash_digest header;
};

} // namespace kth::database

#endif // KTH_DATABASE_SCRIPT_FILTER_RECORD_HPP_
