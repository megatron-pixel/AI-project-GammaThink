// task:forward-fixtures-v1
#pragma once

#include "core.hpp"
#include <array>
#include <cstdint>
#include <string_view>

namespace forward_fixture {

struct Case {
    std::string_view name;
    std::string_view module;
    std::string_view goal;
    std::uint64_t work;
    std::uint32_t events;
    std::uint32_t depth;
    axiom::Status status;
    bool limited;
};

inline const std::array<Case, 16> cases{{
    {
        "fact",
        "axioms 1 forward_fixture 1\nfact a | synthetic\n",
        "a",
        10000,
        4096,
        128,
        axiom::Status::Proved,
        false
    },
    {
        "negative",
        "axioms 1 forward_fixture 1\nfact !a | synthetic\n",
        "a",
        10000,
        4096,
        128,
        axiom::Status::Disproved,
        false
    },
    {
        "conflict",
        "axioms 1 forward_fixture 1\nfact a | synthetic\nfact !a | synthetic\n",
        "a",
        10000,
        4096,
        128,
        axiom::Status::Contradiction,
        false
    },
    {
        "cycle",
        "axioms 1 forward_fixture 1\nrule a -> a | synthetic\n",
        "a",
        10000,
        4096,
        128,
        axiom::Status::Unknown,
        false
    },
    {
        "chain",
        "axioms 1 forward_fixture 1\nfact a | synthetic\nrule a -> b | synthetic\nrule b -> c | synthetic\n",
        "c",
        10000,
        4096,
        128,
        axiom::Status::Proved,
        false
    },
    {
        "conjunction",
        "axioms 1 forward_fixture 1\nfact a | synthetic\nfact b | synthetic\nrule a & b -> c | synthetic\n",
        "c",
        10000,
        4096,
        128,
        axiom::Status::Proved,
        false
    },
    {
        "missing_joint",
        "axioms 1 forward_fixture 1\nfact a | synthetic\nrule a & b -> c | synthetic\n",
        "c",
        10000,
        4096,
        128,
        axiom::Status::Unknown,
        false
    },
    {
        "alternatives",
        "axioms 1 forward_fixture 1\nrule x -> g | synthetic\nfact c | synthetic\nrule c -> g | synthetic\n",
        "g",
        10000,
        4096,
        128,
        axiom::Status::Proved,
        false
    },
    {
        "cycle_seed",
        "axioms 1 forward_fixture 1\nfact x | synthetic\nrule a -> b | synthetic\nrule b -> a | synthetic\nrule x -> a | synthetic\n",
        "b",
        10000,
        4096,
        128,
        axiom::Status::Proved,
        false
    },
    {
        "joint_conflict",
        "axioms 1 forward_fixture 1\nfact a | synthetic\nfact b | synthetic\nrule a & b -> c | synthetic\nfact !c | synthetic\n",
        "c",
        10000,
        4096,
        128,
        axiom::Status::Contradiction,
        false
    },
    {
        "zero_work",
        "axioms 1 forward_fixture 1\nfact a | synthetic\n",
        "a",
        0,
        4096,
        128,
        axiom::Status::Unknown,
        true
    },
    {
        "event_bound",
        "axioms 1 forward_fixture 1\nfact a | synthetic\n",
        "a",
        10000,
        1,
        128,
        axiom::Status::Unknown,
        true
    },
    {
        "depth_bound",
        "axioms 1 forward_fixture 1\nfact a | synthetic\nrule a -> b | synthetic\nrule b -> c | synthetic\n",
        "c",
        10000,
        4096,
        1,
        axiom::Status::Unknown,
        true
    },
    {
        "negative_chain",
        "axioms 1 forward_fixture 1\nfact a | synthetic\nrule a -> !b | synthetic\n",
        "b",
        10000,
        4096,
        128,
        axiom::Status::Disproved,
        false
    },
    {
        "unrelated",
        "axioms 1 forward_fixture 1\nfact x | synthetic\n",
        "q",
        10000,
        4096,
        128,
        axiom::Status::Unknown,
        false
    },
    {
        "no_explosion",
        "axioms 1 forward_fixture 1\nfact a | synthetic\nfact !a | synthetic\nrule b -> b | synthetic\n",
        "b",
        10000,
        4096,
        128,
        axiom::Status::Unknown,
        false
    }
}};

} // namespace forward_fixture
