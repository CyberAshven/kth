// Copyright (c) 2016-present Knuth Project developers.
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test_helpers.hpp>

#include <sstream>

#include <kth/node.hpp>

using namespace kth;

// =============================================================================
// Script filters (compact filter type 1)
// =============================================================================

TEST_CASE("script filters are built by default", "[node][script_filters][parser]") {
    node::parser parser(domain::config::network::mainnet);
    char const* argv[]{"kth", "--network", "mainnet"};
    std::ostringstream error;
    REQUIRE(parser.parse(3, argv, error));
    CHECK(parser.configured.chain.script_filters);
}

TEST_CASE("chain.script_filters=false turns the script filters off", "[node][script_filters][parser]") {
    node::parser parser(domain::config::network::mainnet);
    char const* argv[]{"kth", "--network", "mainnet", "--chain.script_filters=false"};
    std::ostringstream error;
    REQUIRE(parser.parse(4, argv, error));
    CHECK_FALSE(parser.configured.chain.script_filters);
}
