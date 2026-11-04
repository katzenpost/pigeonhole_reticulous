// SPDX-License-Identifier: AGPL-3.0-only
// The JSON of member/trace.rs: what katzenqt1's member traces record, and the group as the
// client saves it. Readers throw Bad on anything that does not fit.
#pragma once
#include <stdexcept>

#include "kq1_member.h"
#include "kq_json.h"

namespace kq1::trace {

using kq::Json;
struct Bad : std::runtime_error { using std::runtime_error::runtime_error; };

Json h(const Bytes& b);
Json oh(const std::optional<Bytes>& b);
Json nu(uint64_t x);
Json on(const std::optional<size_t>& x);
template <class K, class V, class FK, class FV>
Json pairs(const OMap<K, V>& m, FK fk, FV fv) {
    std::vector<Json> a;
    for (auto& [k, v] : m) a.push_back(Json::arr({fk(k), fv(v)}));
    return Json::arr(a);
}
Json message(const Message& m);
Json payload(const Payload& p);
Json join_reply(const JoinReply& r);
Json at_j(const At& a);
Json reader(const Reader& r);
Json retention(const Retention& p);
Json hexes(const std::vector<Bytes>& v);
Json group_j(const Group& g);

const Json& get(const Json& j, const char* k);
Bytes b(const Json& j);
std::optional<Bytes> ob(const Json& j);
uint64_t u(const Json& j);
std::optional<size_t> ous(const Json& j);
const std::vector<Json>& arr(const Json& j);
const std::vector<Json>& tup(const Json& j, size_t n);
bool bo(const Json& j);
std::vector<Bytes> indexes_r(const Json& j);
template <class F>
void each_pair(const Json& j, F f) {
    for (auto& p : arr(j)) {
        auto& kv = tup(p, 2);
        f(kv[0], kv[1]);
    }
}
Message read_message(const Json& j);
std::optional<Message> read_opt_message(const Json& j);
Payload read_payload(const Json& j);
JoinReply read_join_reply(const Json& j);
Retention read_retention(const Json& j);
At read_at(const Json& j);
Reader read_reader(const Json& j);
OMap<Key, Index> index_pairs(const Json& j);
OMap<Key, uint64_t> nat_pairs(const Json& j);
Group read_group(const Json& j);

}  // namespace kq1::trace
