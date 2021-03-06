/**
 * Copyright (c) 2017-present, Facebook, Inc. and its affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */
#pragma once

#include <boost/icl/interval_map.hpp>

#include "logdevice/common/EpochMetaData.h"
#include "logdevice/common/types_internal.h"
#include "logdevice/include/types.h"

namespace facebook { namespace logdevice {

/**
 * RebuildingPlan describes the plan that a LogRebuilding must follow to
 * rebuild (with local data) a certain log in a certain shard. In particular,
 * it contains ranges of epochs for which this node has relevant data and an
 * estimation of the smallest record timestamp that will be rebuilt.
 */

class RebuildingPlan {
 public:
  explicit RebuildingPlan(RecordTimestamp smallest_ts)
      : smallestTimestamp(smallest_ts) {}

  RebuildingPlan() = default;

  // Set of epoch intervals we need to read. This is all the epochs we know have
  // a nodeset that contains at least one shard in the rebuilding set.
  using epoch_ranges_t = boost::icl::interval_map<
      epoch_t::raw_type,
      std::shared_ptr<EpochMetaData>,
      boost::icl::partial_absorber,
      std::less,
      boost::icl::inplace_plus,
      boost::icl::inter_section,
      boost::icl::right_open_interval<epoch_t::raw_type, std::less>>;

  void addEpochRange(epoch_t since,
                     epoch_t until,
                     std::shared_ptr<EpochMetaData> metadata);
  void addEpochRange(epoch_ranges_t::interval_type epoch_range,
                     std::shared_ptr<EpochMetaData> metadata);

  void clearEpochRange() {
    epochsToRead.clear();
  }

  std::string toString() const;

  lsn_t untilLSN = LSN_INVALID;
  epoch_ranges_t epochsToRead;

  // Smallest timestamp for which there should be anything to rebuild.
  folly::Optional<RecordTimestamp> smallestTimestamp;
};

}} // namespace facebook::logdevice
