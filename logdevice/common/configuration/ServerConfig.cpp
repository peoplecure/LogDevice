/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#define __STDC_FORMAT_MACROS // pull in PRId64 etc

#include "ServerConfig.h"

#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cinttypes>
#include <utility>
#include <algorithm>

#include <folly/synchronization/Baton.h>
#include <folly/Conv.h>
#include <folly/DynamicConverter.h>
#include <folly/FileUtil.h>
#include <folly/compression/Compression.h>
#include <folly/json.h>
#include "logdevice/common/configuration/ConfigParser.h"
#include "logdevice/common/configuration/LogsConfigParser.h"
#include "logdevice/common/configuration/NodesConfigParser.h"
#include "logdevice/common/configuration/ParsingHelpers.h"
#include "logdevice/common/types_internal.h"
#include "logdevice/common/commandline_util_chrono.h"
#include "logdevice/common/FailureDomainNodeSet.h"
#include "logdevice/common/NodeID.h"
#include "logdevice/common/SlidingWindow.h"
#include "logdevice/common/util.h"
#include "logdevice/include/Err.h"
#include "logdevice/common/debug.h"

using namespace facebook::logdevice::configuration::parser;
using facebook::logdevice::configuration::NodeRole;

namespace facebook { namespace logdevice {

// set of keys that are used in configuration json format
static const std::set<std::string> config_recognized_keys = {
    "client_settings",
    "cluster",
    "cluster_creation_time",
    "defaults",
    "include_log_config",
    "log_namespace_delimiter",
    "logs",
    "nodes",
    "metadata_logs",
    "principals",
    "security_information",
    "server_settings",
    "trace-logger",
    "traffic_shaping",
    "version",
    "zookeeper",
};

std::unique_ptr<ServerConfig>
ServerConfig::fromJson(const std::string& jsonPiece) {
  auto parsed = parseJson(jsonPiece);
  // Make sure the parsed string is actually an object
  if (!parsed.isObject()) {
    ld_error("configuration must be a map");
    err = E::INVALID_CONFIG;
    return nullptr;
  }
  return ServerConfig::fromJson(parsed);
}

std::unique_ptr<ServerConfig>
ServerConfig::fromJson(const folly::dynamic& parsed) {
  std::string clusterName;
  config_version_t version;
  OptionalTimestamp clusterCreationTime;
  NodesConfig nodesConfig;
  MetaDataLogsConfig metaDataLogsConfig;
  PrincipalsConfig principalsConfig;
  SecurityConfig securityConfig;
  TraceLoggerConfig traceLoggerConfig;
  TrafficShapingConfig trafficShapingConfig;
  ZookeeperConfig zookeeperConfig;
  SettingsConfig serverSettingsConfig;
  SettingsConfig clientSettingsConfig;

  // We need the namespace delimiter before loading log configuration, but we
  // can only set it in the LogsConfig after we've chosen the final LogsConfig
  // instance below.
  std::string ns_delimiter = LogsConfig::default_namespace_delimiter_;

  // This setting has to be in the main config, because a client that doesn't
  // have the logs config should still be able to understand namespaces
  // correctly
  std::string ns_delim_fbstr;
  if (getStringFromMap(parsed, "log_namespace_delimiter", ns_delim_fbstr)) {
    // default delimiter
    // validate that it's a single character.
    if (ns_delim_fbstr.size() > 1) {
      // this must be at most 1-character long.
      ld_error("Cannot accept the value of \"log_namespace_delimiter\", value "
               "is '%s'. This must be at most 1 character, failing!",
               ns_delim_fbstr.c_str());
      err = E::INVALID_CONFIG;
      return nullptr;
    }
    ns_delimiter = ns_delim_fbstr;
  }

  InternalLogs internalLogs(ns_delimiter);

  // ParseSecurityInfo should be called before ParseLogs and ParseMetaDataLog
  // as the securityConfig is used in both.
  bool success = parseClusterName(parsed, clusterName) &&
      parsePrincipals(parsed, principalsConfig) &&
      parseVersion(parsed, version) &&
      parseClusterCreationTime(parsed, clusterCreationTime) &&
      parseSecurityInfo(parsed, securityConfig) &&
      parseTrafficShaping(parsed, trafficShapingConfig) &&
      parseNodes(parsed, nodesConfig) &&
      parseMetaDataLog(parsed, securityConfig, metaDataLogsConfig) &&
      parseZookeeper(parsed, zookeeperConfig) &&
      parseSettings(parsed, "server_settings", serverSettingsConfig) &&
      parseSettings(parsed, "client_settings", clientSettingsConfig) &&
      parseInternalLogs(parsed, internalLogs) &&
      parseTraceLogger(parsed, traceLoggerConfig);

  if (!success) {
    return nullptr;
  }

  folly::dynamic customFields = folly::dynamic::object;
  for (auto& pair : parsed.items()) {
    if (config_recognized_keys.find(pair.first.asString()) !=
        config_recognized_keys.end()) {
      // This key is supposed to be parsed by logdevice
      continue;
    }
    customFields[pair.first] = pair.second;
  }

  auto config = fromData(std::move(clusterName),
                         std::move(nodesConfig),
                         std::move(metaDataLogsConfig),
                         std::move(principalsConfig),
                         std::move(securityConfig),
                         std::move(traceLoggerConfig),
                         std::move(trafficShapingConfig),
                         std::move(zookeeperConfig),
                         std::move(serverSettingsConfig),
                         std::move(clientSettingsConfig),
                         std::move(internalLogs),
                         std::move(clusterCreationTime),
                         std::move(customFields),
                         ns_delimiter);

  config->setVersion(version);
  return config;
}

ServerConfig::ServerConfig(std::string cluster_name,
                           NodesConfig nodesConfig,
                           MetaDataLogsConfig metaDataLogsConfig,
                           PrincipalsConfig principalsConfig,
                           SecurityConfig securityConfig,
                           TraceLoggerConfig traceLoggerConfig,
                           TrafficShapingConfig trafficShapingConfig,
                           ZookeeperConfig zookeeperConfig,
                           SettingsConfig serverSettingsConfig,
                           SettingsConfig clientSettingsConfig,
                           InternalLogs internalLogs,
                           OptionalTimestamp clusterCreationTime,
                           folly::dynamic customFields,
                           const std::string& ns_delimiter)
    : clusterName_(std::move(cluster_name)),
      clusterCreationTime_(std::move(clusterCreationTime)),
      nodesConfig_(std::move(nodesConfig)),
      metaDataLogsConfig_(std::move(metaDataLogsConfig)),
      principalsConfig_(std::move(principalsConfig)),
      securityConfig_(std::move(securityConfig)),
      trafficShapingConfig_(std::move(trafficShapingConfig)),
      traceLoggerConfig_(std::move(traceLoggerConfig)),
      zookeeperConfig_(std::move(zookeeperConfig)),
      serverSettingsConfig_(std::move(serverSettingsConfig)),
      clientSettingsConfig_(std::move(clientSettingsConfig)),
      internalLogs_(std::move(internalLogs)),
      ns_delimiter_(ns_delimiter),
      customFields_(std::move(customFields)) {
  // sequencersConfig_ needs consecutive node indexes, see comment in
  // SequencersConfig.h.
  // Pad with zero-weight invalid nodes if there are gaps in numbering.
  size_t max_node = getMaxNodeIdx();
  sequencersConfig_.nodes.resize(max_node + 1);
  sequencersConfig_.weights.resize(max_node + 1);

  for (const auto& it : nodesConfig_.getNodes()) {
    node_index_t i = it.first;
    const auto& node = it.second;

    auto insert_result = addrToIndex_.insert(std::make_pair(node.address, i));
    ld_check(insert_result.second);

    if (node.isSequencingEnabled()) {
      sequencersConfig_.nodes[i] = NodeID(i, node.generation);
      sequencersConfig_.weights[i] = node.getSequencerWeight();
    }
  }

  // Scale all weights to the [0, 1] range. Note that increasing the maximum
  // weight will cause all nodes' weights to change, possibly resulting in
  // many sequencers being relocated.
  auto max_it = std::max_element(
      sequencersConfig_.weights.begin(), sequencersConfig_.weights.end());
  if (max_it != sequencersConfig_.weights.end() && *max_it > 0) {
    double max_weight = *max_it;
    for (double& weight : sequencersConfig_.weights) {
      weight /= max_weight;
    }
  }
}

const ServerConfig::Node* ServerConfig::getNode(node_index_t index) const {
  auto it = nodesConfig_.getNodes().find(index);
  if (it == nodesConfig_.getNodes().end()) {
    err = E::NOTFOUND;
    return nullptr;
  }

  return &it->second;
}

const ServerConfig::Node* ServerConfig::getNode(const NodeID& id) const {
  if (!id.isNodeID()) { // only possible if there was memory corruption
    ld_error("invalid node ID passed: (%d, %d)", id.index(), id.generation());
    err = E::INVALID_PARAM;
    return nullptr;
  }

  const Node* node = getNode(id.index());
  if (node == nullptr ||
      (id.generation() != 0 && node->generation != id.generation())) {
    // Generations don't match, it's not the right server
    err = E::NOTFOUND;
    return nullptr;
  }

  // Found it!
  return node;
}

int ServerConfig::getNodeID(const Sockaddr& address, NodeID* node) const {
  auto it = addrToIndex_.find(address);
  if (it == addrToIndex_.end()) {
    err = E::NOTFOUND;
    return -1;
  }

  node_index_t index = it->second;
  ld_check(nodesConfig_.getNodes().at(index).address == address);

  ld_check(node != nullptr);
  *node = NodeID(index, nodesConfig_.getNodes().at(index).generation);
  return 0;
}

std::shared_ptr<const Principal>
ServerConfig::getPrincipalByName(const std::string* name) const {
  return principalsConfig_.getPrincipalByName(name);
}

folly::Optional<double>
ServerConfig::getTracerSamplePercentage(const std::string& key) const {
  return traceLoggerConfig_.getSamplePercentage(key);
}

double ServerConfig::getDefaultSamplePercentage() const {
  return traceLoggerConfig_.getDefaultSamplePercentage();
}

bool ServerConfig::validStorageSet(const Nodes& cluster_nodes,
                                   const StorageSet& storage_set,
                                   ReplicationProperty replication,
                                   bool strict) {
  if (!replication.isValid()) {
    return false;
  }

  // attribute is weither weight > 0
  FailureDomainNodeSet<bool> failure_domain(
      storage_set, cluster_nodes, replication);

  for (auto shard : storage_set) {
    auto it = cluster_nodes.find(shard.node());
    if (strict && it == cluster_nodes.end()) {
      ld_error("Invalid nodeset: %s is referenced from the nodeset but "
               "doesn't exist in nodes config.",
               shard.toString().c_str());
      return false;
    }
    if (it != cluster_nodes.end() && it->second.isWritableStorageNode()) {
      failure_domain.setShardAttribute(shard, true);
    }
  }

  // return true if the subset of writable storage nodes can satisfy
  // replication property
  return failure_domain.canReplicate(true);
}

std::string ServerConfig::getZookeeperQuorumString() const {
  std::string result;
  for (const Sockaddr& addr : zookeeperConfig_.quorum) {
    if (!result.empty()) {
      result += ',';
    }
    // Do not include brackets "[a:b:c..]" around IPv6 addresses in Zookeeper
    // quorum string. Zookeeper C client currently only supports
    // a:b:c:..:z:port format of IPv6+port specifiers
    result += addr.toStringNoBrackets();
  }
  return result;
}

std::unique_ptr<ServerConfig>
ServerConfig::fromData(std::string cluster_name,
                       NodesConfig nodes,
                       MetaDataLogsConfig metadata_logs,
                       PrincipalsConfig principalsConfig,
                       SecurityConfig securityConfig,
                       TraceLoggerConfig traceLoggerConfig,
                       TrafficShapingConfig trafficShapingConfig,
                       ZookeeperConfig zookeeper,
                       SettingsConfig serverSettingsConfig,
                       SettingsConfig clientSettingsConfig,
                       InternalLogs internalLogs,
                       OptionalTimestamp clusterCreationTime,
                       folly::dynamic customFields,
                       const std::string& ns_delimiter) {
  return std::unique_ptr<ServerConfig>(
      new ServerConfig(std::move(cluster_name),
                       std::move(nodes),
                       std::move(metadata_logs),
                       std::move(principalsConfig),
                       std::move(securityConfig),
                       std::move(traceLoggerConfig),
                       std::move(trafficShapingConfig),
                       std::move(zookeeper),
                       std::move(serverSettingsConfig),
                       std::move(clientSettingsConfig),
                       std::move(internalLogs),
                       std::move(clusterCreationTime),
                       std::move(customFields),
                       ns_delimiter));
}

std::unique_ptr<ServerConfig> ServerConfig::copy() const {
  std::unique_ptr<ServerConfig> config = fromData(clusterName_,
                                                  NodesConfig(getNodes()),
                                                  metaDataLogsConfig_,
                                                  principalsConfig_,
                                                  securityConfig_,
                                                  traceLoggerConfig_,
                                                  trafficShapingConfig_,
                                                  zookeeperConfig_,
                                                  serverSettingsConfig_,
                                                  clientSettingsConfig_,
                                                  internalLogs_,
                                                  getClusterCreationTime(),
                                                  getCustomFields(),
                                                  ns_delimiter_);
  config->setVersion(version_);
  if (hasMyNodeID()) {
    config->setMyNodeID(my_node_id_);
  }
  config->setServerOrigin(server_origin_);
  config->setMainConfigMetadata(main_config_metadata_);
  config->setIncludedConfigMetadata(included_config_metadata_);
  return config;
}

std::shared_ptr<ServerConfig> ServerConfig::withNodes(NodesConfig nodes) const {
  auto metaDataLogsConfig = getMetaDataLogsConfig();
  std::vector<node_index_t> metadata_nodes;
  auto& nodes_map = nodes.getNodes();
  // make sure the metadata logs nodeset is consistent with the nodes config
  for (auto n : metaDataLogsConfig.metadata_nodes) {
    if (nodes_map.find(n) != nodes_map.end()) {
      metadata_nodes.push_back(n);
    }
  }
  if (metaDataLogsConfig.metadata_nodes != metadata_nodes) {
    metaDataLogsConfig.metadata_nodes = metadata_nodes;
  }
  std::shared_ptr<ServerConfig> config = fromData(clusterName_,
                                                  std::move(nodes),
                                                  metaDataLogsConfig,
                                                  principalsConfig_,
                                                  securityConfig_,
                                                  traceLoggerConfig_,
                                                  trafficShapingConfig_,
                                                  zookeeperConfig_,
                                                  serverSettingsConfig_,
                                                  clientSettingsConfig_,
                                                  internalLogs_,
                                                  getClusterCreationTime(),
                                                  getCustomFields(),
                                                  ns_delimiter_);
  config->setVersion(version_);
  if (hasMyNodeID()) {
    config->setMyNodeID(my_node_id_);
  }
  config->setMainConfigMetadata(main_config_metadata_);
  config->setIncludedConfigMetadata(included_config_metadata_);
  return config;
}

std::shared_ptr<ServerConfig>
ServerConfig::withZookeeperConfig(ZookeeperConfig zk) const {
  std::shared_ptr<ServerConfig> config = fromData(clusterName_,
                                                  nodesConfig_,
                                                  metaDataLogsConfig_,
                                                  principalsConfig_,
                                                  securityConfig_,
                                                  traceLoggerConfig_,
                                                  trafficShapingConfig_,
                                                  std::move(zk),
                                                  serverSettingsConfig_,
                                                  clientSettingsConfig_,
                                                  internalLogs_,
                                                  getClusterCreationTime(),
                                                  getCustomFields(),
                                                  ns_delimiter_);
  config->setVersion(version_);
  if (hasMyNodeID()) {
    config->setMyNodeID(my_node_id_);
  }
  config->setMainConfigMetadata(main_config_metadata_);
  config->setIncludedConfigMetadata(included_config_metadata_);
  return config;
}

std::shared_ptr<ServerConfig>
ServerConfig::withVersion(config_version_t version) const {
  std::shared_ptr<ServerConfig> config = fromData(clusterName_,
                                                  nodesConfig_,
                                                  metaDataLogsConfig_,
                                                  principalsConfig_,
                                                  securityConfig_,
                                                  traceLoggerConfig_,
                                                  trafficShapingConfig_,
                                                  zookeeperConfig_,
                                                  serverSettingsConfig_,
                                                  clientSettingsConfig_,
                                                  internalLogs_,
                                                  getClusterCreationTime(),
                                                  getCustomFields(),
                                                  ns_delimiter_);
  config->setVersion(version);
  if (hasMyNodeID()) {
    config->setMyNodeID(my_node_id_);
  }
  config->setMainConfigMetadata(main_config_metadata_);
  config->setIncludedConfigMetadata(included_config_metadata_);
  return config;
}

std::shared_ptr<ServerConfig> ServerConfig::createEmpty() {
  return fromData(std::string(),
                  NodesConfig(),
                  MetaDataLogsConfig(),
                  PrincipalsConfig(),
                  SecurityConfig(),
                  TraceLoggerConfig(),
                  TrafficShapingConfig(),
                  ZookeeperConfig(),
                  SettingsConfig(),
                  SettingsConfig(),
                  InternalLogs(),
                  OptionalTimestamp(),
                  folly::dynamic::object());
}

const std::string ServerConfig::toString(const LogsConfig* with_logs,
                                         bool compress) const {
  // Grab the lock and initialize the cached result if this is the first call
  // to toString()
  std::lock_guard<std::mutex> guard(to_string_cache_mutex_);

  // Normally LogsConfig::getVersion() uniquely defines the contents of the
  // logs config, so we can use cached toString() result if version matches.
  // However, unit tests may modify LocalLogsConfig in place without changing
  // version. In this case we shouldn't use cache.
  auto local_logs_config =
      dynamic_cast<const configuration::LocalLogsConfig*>(with_logs);
  bool no_cache = local_logs_config && local_logs_config->wasModifiedInPlace();

  if (with_logs) {
    uint64_t logs_config_version = with_logs->getVersion();
    if (logs_config_version != last_to_string_logs_config_version_ ||
        no_cache) {
      // Clear the cache for the full config if the LogsConfig has changed
      last_to_string_logs_config_version_ = LSN_INVALID;
      all_to_string_cache_.clear();
      compressed_all_to_string_cache_.clear();
    }
  }

  std::string uncached_config_str;
  std::string& config_str = no_cache
      ? uncached_config_str
      : with_logs ? all_to_string_cache_ : main_to_string_cache_;
  if (config_str.empty()) {
    config_str = toStringImpl(with_logs);
  }
  ld_check(!config_str.empty());

  if (!compress) {
    return config_str;
  }

  std::string uncached_compressed_config_str;
  std::string& compressed_config_str = no_cache
      ? uncached_compressed_config_str
      : with_logs ? compressed_all_to_string_cache_
                  : compressed_main_to_string_cache_;
  if (compressed_config_str.empty()) {
    using folly::IOBuf;
    std::unique_ptr<IOBuf> input =
        IOBuf::wrapBuffer(config_str.data(), config_str.size());
    auto codec = folly::io::getCodec(folly::io::CodecType::GZIP);
    std::unique_ptr<IOBuf> compressed;
    try {
      compressed = codec->compress(input.get());
    } catch (const std::invalid_argument& ex) {
      ld_error("gzip compression of config failed");
      return compressed_config_str;
    }
    compressed_config_str = compressed->moveToFbString().toStdString();
  }
  return compressed_config_str;
}

std::string ServerConfig::toStringImpl(const LogsConfig* with_logs) const {
  auto json = toJson(with_logs);

  folly::json::serialization_opts opts;
  opts.pretty_formatting = true;
  opts.sort_keys = true;
  return folly::json::serialize(json, opts);
}

folly::dynamic ServerConfig::toJson(const LogsConfig* with_logs) const {
  folly::dynamic output_nodes = folly::dynamic::array;

  const auto& nodes = nodesConfig_.getNodes();
  std::vector<node_index_t> sorted_node_ids(nodes.size());
  std::transform(
      nodes.begin(), nodes.end(), sorted_node_ids.begin(), [](const auto& src) {
        return src.first;
      });
  std::sort(sorted_node_ids.begin(), sorted_node_ids.end());

  for (const auto& nidx : sorted_node_ids) {
    const ServerConfig::Node& node = nodes.at(nidx);

    folly::dynamic node_dict = folly::dynamic::object("node_id", nidx)(
        "host", node.address.toString())("generation", node.generation)(
        "gossip_address", node.gossip_address.toString());

    if (node.hasRole(NodeRole::STORAGE)) {
      // TODO: Remove once all production configs and tooling
      //       No longer use this field.
      node_dict["weight"] = node.getLegacyWeight();
    }

    // Optional Universal Attributes.
    if (node.location.hasValue()) {
      node_dict["location"] = node.locationStr();
    }
    if (node.ssl_address) {
      node_dict["ssl_host"] = node.ssl_address->toString();
    }
    if (!node.settings.empty()) {
      node_dict["settings"] = folly::toDynamic(node.settings);
    }
    if (node.admin_address.hasValue()) {
      node_dict["admin_host"] = node.admin_address->toString();
    }

    // Sequencer Role Attributes.
    auto roles = folly::dynamic::array();
    if (node.hasRole(configuration::NodeRole::SEQUENCER)) {
      roles.push_back("sequencer");
      node_dict["sequencer"] = node.getSequencerWeight();
    }

    // Storage Role Attributes.
    if (node.hasRole(configuration::NodeRole::STORAGE)) {
      roles.push_back("storage");
      auto* storage = node.storage_attributes.get();
      node_dict["storage"] =
          configuration::storageStateToString(storage->state);
      node_dict["storage_capacity"] = storage->capacity;
      node_dict["num_shards"] = storage->num_shards;
      if (storage->exclude_from_nodesets) {
        node_dict["exclude_from_nodesets"] = storage->exclude_from_nodesets;
      }
    }
    node_dict["roles"] = roles;

    output_nodes.push_back(node_dict);
  }

  folly::dynamic meta_nodeset = folly::dynamic::array;
  for (auto index : metaDataLogsConfig_.metadata_nodes) {
    meta_nodeset.push_back(index);
  }

  folly::dynamic metadata_logs =
      getMetaDataLogGroupInDir().toFollyDynamic(true /*is_metadata*/);

  metadata_logs["nodeset"] = meta_nodeset;
  metadata_logs["nodeset_selector"] =
      NodeSetSelectorTypeToString(metaDataLogsConfig_.nodeset_selector_type);
  metadata_logs["sequencers_write_metadata_logs"] =
      metaDataLogsConfig_.sequencers_write_metadata_logs;
  metadata_logs["sequencers_provision_epoch_store"] =
      metaDataLogsConfig_.sequencers_provision_epoch_store;
  auto& metadata_version = metaDataLogsConfig_.metadata_version_to_write;
  if (metadata_version.hasValue()) {
    metadata_logs["metadata_version"] = metadata_version.value();
  }

  folly::dynamic json_all = folly::dynamic::object("cluster", clusterName_)(
      "version", version_.val())("nodes", std::move(output_nodes))(
      "metadata_logs", std::move(metadata_logs))(
      "internal_logs", internalLogs_.toDynamic())(
      "principals", principalsConfig_.toFollyDynamic())(
      "traffic_shaping", trafficShapingConfig_.toFollyDynamic())(
      "server_settings", folly::toDynamic(serverSettingsConfig_))(
      "client_settings", folly::toDynamic(clientSettingsConfig_))(
      "trace-logger", traceLoggerConfig_.toFollyDynamic());

  if (clusterCreationTime_.hasValue()) {
    json_all["cluster_creation_time"] = clusterCreationTime_.value().count();
  }
  if (with_logs != nullptr) {
    json_all["logs"] = with_logs->toJson();
  }
  if (ns_delimiter_ != LogsConfig::default_namespace_delimiter_) {
    json_all["log_namespace_delimiter"] = ns_delimiter_;
  }
  // Authentication Information is optional
  if (securityConfig_.securityOptionsEnabled()) {
    json_all["security_information"] = securityConfig_.toFollyDynamic();
  }

  // Zookeeper section is optional
  if (!zookeeperConfig_.quorum.empty()) {
    folly::dynamic quorum = folly::dynamic::array;
    for (const Sockaddr& addr : zookeeperConfig_.quorum) {
      quorum.push_back(addr.toString());
    }
    std::string timeout_str =
        std::to_string(zookeeperConfig_.session_timeout.count()) + "ms";
    folly::dynamic zookeeper =
        folly::dynamic::object()("quorum", quorum)("timeout", timeout_str);
    json_all["zookeeper"] = zookeeper;
  }

  // insert custom fields
  for (auto& pair : customFields_.items()) {
    json_all[pair.first] = pair.second;
  }

  return json_all;
}

bool ServerConfig::getNodeSSL(folly::Optional<NodeLocation> my_location,
                              NodeID node,
                              NodeLocationScope diff_level) const {
  if (diff_level == NodeLocationScope::ROOT) {
    // Never use SSL
    return false;
  }

  if (diff_level == NodeLocationScope::NODE) {
    // Always use SSL
    return true;
  }

  if (!my_location) {
    RATELIMIT_ERROR(std::chrono::seconds(1),
                    10,
                    "--ssl-boundary specified, but no location available for "
                    "local machine. Defaulting to SSL.");
    return true;
  }

  auto node_cfg = getNode(node);
  ld_check(node_cfg);
  if (!node_cfg->location) {
    RATELIMIT_ERROR(std::chrono::seconds(1),
                    10,
                    "--ssl-boundary specified, but no location available for "
                    "node %s. Defaulting to SSL.",
                    node.toString().c_str());
    return true;
  }

  if (!my_location->sharesScopeWith(*node_cfg->location, diff_level)) {
    if (!node_cfg->ssl_address) {
      RATELIMIT_ERROR(std::chrono::seconds(1),
                      10,
                      "--ssl-boundary specified, but no SSL address specified "
                      "for node %s.",
                      node.toString().c_str());
    }
    return true;
  }

  return false;
}

}} // namespace facebook::logdevice
