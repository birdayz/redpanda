/*
 * Copyright 2020 Redpanda Data, Inc.
 *
 * Use of this software is governed by the Business Source License
 * included in the file licenses/BSL.md
 *
 * As of the Change Date specified in that file, in accordance with
 * the Business Source License, use of this software will be governed
 * by the Apache License, Version 2.0
 */

#pragma once
#include "cluster/types.h"
#include "model/metadata.h"
#include "raft/consensus_client_protocol.h"
#include "raft/heartbeat_manager.h"
#include "raft/raft_feature_table.h"
#include "raft/recovery_memory_quota.h"
#include "raft/rpc_client_protocol.h"
#include "raft/types.h"
#include "storage/fwd.h"

#include <seastar/core/metrics_registration.hh>
#include <seastar/core/scheduling.hh>

#include <absl/container/flat_hash_map.h>

#include <tuple>

namespace raft {

/*
 * Owns and manages all raft groups.
 */
class group_manager {
public:
    using leader_cb_t = ss::noncopyable_function<void(
      raft::group_id, model::term_id, std::optional<model::node_id>)>;

    struct configuration {
        config::binding<std::chrono::milliseconds> heartbeat_interval;
        config::binding<std::chrono::milliseconds> heartbeat_timeout;
        std::chrono::milliseconds raft_io_timeout_ms;
    };
    using config_provider_fn = ss::noncopyable_function<configuration()>;

    group_manager(
      model::node_id self,
      ss::scheduling_group raft_scheduling_group,
      config_provider_fn,
      recovery_memory_quota::config_provider_fn recovery_mem_cfg,
      ss::sharded<rpc::connection_cache>& clients,
      ss::sharded<storage::api>& storage,
      ss::sharded<recovery_throttle>&);

    ss::future<> start();
    ss::future<> stop();
    ss::future<> stop_heartbeats();

    ss::future<ss::lw_shared_ptr<raft::consensus>> create_group(
      raft::group_id id, std::vector<model::broker> nodes, storage::log log);

    ss::future<> shutdown(ss::lw_shared_ptr<raft::consensus>);

    ss::future<> remove(ss::lw_shared_ptr<raft::consensus>);

    cluster::notification_id_type
    register_leadership_notification(leader_cb_t cb) {
        auto id = _notification_id++;
        // call notification for all the groups
        for (auto& gr : _groups) {
            cb(gr->group(), gr->term(), gr->get_leader_id());
        }
        _notifications.emplace_back(id, std::move(cb));
        return id;
    }

    void unregister_leadership_notification(cluster::notification_id_type id) {
        auto it = std::find_if(
          _notifications.begin(),
          _notifications.end(),
          [id](const std::pair<cluster::notification_id_type, leader_cb_t>& n) {
              return n.first == id;
          });
        if (it != _notifications.end()) {
            _notifications.erase(it);
        }
    }

    consensus_client_protocol raft_client() const { return _client; }

    void set_feature_active(raft_feature);

private:
    void trigger_leadership_notification(raft::leadership_status);
    void setup_metrics();

    model::node_id _self;
    ss::scheduling_group _raft_sg;
    raft::consensus_client_protocol _client;
    configuration _configuration;
    raft::heartbeat_manager _heartbeats;
    ss::gate _gate;
    std::vector<ss::lw_shared_ptr<raft::consensus>> _groups;
    cluster::notification_id_type _notification_id{0};
    std::vector<std::pair<cluster::notification_id_type, leader_cb_t>>
      _notifications;
    ss::metrics::metric_groups _metrics;
    storage::api& _storage;
    recovery_throttle& _recovery_throttle;
    recovery_memory_quota _recovery_mem_quota;
    raft_feature_table _raft_feature_table;
};

} // namespace raft
