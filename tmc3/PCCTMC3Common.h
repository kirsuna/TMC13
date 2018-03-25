/* The copyright in this software is being made available under the BSD
 * Licence, included below.  This software may be subject to other third
 * party and contributor rights, including patent rights, and no such
 * rights are granted under this licence.
 *
 * Copyright (c) 2017-2018, ISO/IEC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *
 * * Neither the name of the ISO/IEC nor the names of its contributors
 *   may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef PCCTMC3Common_h
#define PCCTMC3Common_h
#include "PCCKdTree.h"
#include "PCCMath.h"

#include <cstdint>
#include <vector>

namespace pcc {
const uint32_t PCCTMC3MagicNumber = 20110904;
const uint32_t PCCTMC3FormatVersion = 1;
const uint32_t PCCTMC3MaxPredictionNearestNeighborCount = 4;

const int MAX_NUM_DM_LEAF_POINTS = 2;

struct PCCOctree3Node {
  // 3D position of the current node's origin (local x,y,z = 0).
  PCCVector3<uint32_t> pos;

  // Range of point indexes spanned by node
  uint32_t start;
  uint32_t end;

  // address of the current node in 3D morton order.
  uint64_t mortonIdx;

  // pattern denoting occupied neighbour nodes.
  //    32 8 (y)
  //     |/
  //  2--n--1 (x)
  //    /|
  //   4 16 (z)
  uint8_t neighPattern = 0;

  // The current node's number of siblings plus one.
  // ie, the number of child nodes present in this node's parent.
  uint8_t numSiblingsPlus1;
};

struct PCCNeighborInfo {
  double invDistance;
  double weight;
  uint32_t index;
};

struct PCCPredictor {
  size_t maxNeighborCount;
  size_t neighborCount;
  uint32_t index;
  uint32_t levelOfDetailIndex;
  PCCNeighborInfo neighbors[PCCTMC3MaxPredictionNearestNeighborCount];

  PCCColor3B predictColor(const PCCPointSet3 &pointCloud) const {
    PCCVector3D predicted(0.0);
    for (size_t i = 0; i < neighborCount; ++i) {
      const PCCColor3B color = pointCloud.getColor(neighbors[i].index);
      const double w = neighbors[i].weight;
      for (size_t k = 0; k < 3; ++k) {
        predicted[k] += w * color[k];
      }
    }
    return PCCColor3B(uint8_t(std::round(predicted[0])), uint8_t(std::round(predicted[1])),
                      uint8_t(std::round(predicted[2])));
  }
  uint16_t predictReflectance(const PCCPointSet3 &pointCloud) const {
    double predicted(0.0);
    for (size_t i = 0; i < neighborCount; ++i) {
      predicted += neighbors[i].weight * pointCloud.getReflectance(neighbors[i].index);
    }
    return uint16_t(std::round(predicted));
  }

  void computeWeights(const size_t neighborCount0) {
    neighborCount = std::min(neighborCount0, maxNeighborCount);
    if (!neighborCount) {
      return;
    }
    double sum = 0.0;
    for (size_t n = 0; n < neighborCount; ++n) {
      neighbors[n].weight = neighbors[n].invDistance;
      sum += neighbors[n].weight;
    }
    assert(sum > 0.0);
    for (size_t n = 0; n < neighborCount; ++n) {
      neighbors[n].weight /= sum;
    }
  }

  void init(const uint32_t current, const uint32_t reference, const uint32_t lodIndex) {
    index = current;
    neighborCount = (reference != PCC_UNDEFINED_INDEX);
    maxNeighborCount = neighborCount;
    neighbors[0].index = reference;
    neighbors[0].invDistance = 1.0;
    neighbors[0].weight = 1.0;
    levelOfDetailIndex = lodIndex;
  }
};

inline int64_t PCCQuantization(const int64_t value, const int64_t qs) {
  const int64_t shift = (qs / 3);
  if (!qs) {
    return value;
  }
  if (value >= 0) {
    return (value + shift) / qs;
  }
  return -((shift - value) / qs);
}

inline int64_t PCCInverseQuantization(const int64_t value, const int64_t qs) {
  return qs == 0 ? value : (value * qs);
}

inline void PCCBuildPredictors(const PCCPointSet3 &pointCloud,
                               const size_t numberOfNearestNeighborsInPrediction,
                               const size_t levelOfDetailCount, const std::vector<size_t> &dist2,
                               std::vector<PCCPredictor> &predictors,
                               std::vector<uint32_t> &numberOfPointsPerLOD,
                               std::vector<uint32_t> &indexes) {
  const size_t pointCount = pointCloud.getPointCount();
  predictors.resize(pointCount);
  const size_t initialBalanceTargetPointCount = 16;
  std::vector<bool> visited;
  visited.resize(pointCount, false);
  indexes.resize(0);
  indexes.reserve(pointCount);
  numberOfPointsPerLOD.resize(0);
  numberOfPointsPerLOD.reserve(levelOfDetailCount);
  size_t prevIndexesSize = 0;
  PCCPointDistInfo nearestNeighbors[PCCTMC3MaxPredictionNearestNeighborCount];
  PCCNNQuery3 nNQuery = {PCCVector3D(0.0), 0.0, 1};
  PCCNNQuery3 nNQuery2 = {PCCVector3D(0.0), std::numeric_limits<double>::max(),
                          numberOfNearestNeighborsInPrediction};
  PCCNNResult nNResult = {nearestNeighbors, 0};
  PCCIncrementalKdTree3 kdtree;

  for (size_t lodIndex = 0; lodIndex < levelOfDetailCount && indexes.size() < pointCount;
       ++lodIndex) {
    const bool filterByDistance = (lodIndex + 1) != levelOfDetailCount;
    const double minDist2 = dist2[lodIndex];
    nNQuery.radius = minDist2;
    size_t balanceTargetPointCount = initialBalanceTargetPointCount;
    for (uint32_t current = 0; current < pointCount; ++current) {
      if (!visited[current]) {
        if (filterByDistance) {
          nNQuery.point = pointCloud[current];
          kdtree.findNearestNeighbors2(nNQuery, nNResult);
        }
        if (!filterByDistance || !nNResult.resultCount || nNResult.neighbors[0].dist2 >= minDist2) {
          nNQuery2.point = pointCloud[current];
          kdtree.findNearestNeighbors2(nNQuery2, nNResult);
          auto &predictor = predictors[indexes.size()];
          if (!nNResult.resultCount) {
            predictor.init(current, PCC_UNDEFINED_INDEX, uint32_t(lodIndex));
          } else if (nNResult.neighbors[0].dist2 == 0.0 || nNResult.resultCount == 1) {
            const uint32_t reference = static_cast<uint32_t>(indexes[nNResult.neighbors[0].index]);
            predictor.init(current, reference, uint32_t(lodIndex));
          } else {
            predictor.levelOfDetailIndex = uint32_t(lodIndex);
            predictor.index = current;
            predictor.neighborCount = predictor.maxNeighborCount = nNResult.resultCount;
            for (size_t n = 0; n < nNResult.resultCount; ++n) {
              predictor.neighbors[n].index = indexes[nNResult.neighbors[n].index];
              predictor.neighbors[n].weight = predictor.neighbors[n].invDistance =
                  1.0 / nNResult.neighbors[n].dist2;
            }
          }
          indexes.push_back(current);
          visited[current] = true;
          kdtree.insert(pointCloud[current]);
          if (balanceTargetPointCount <= kdtree.size()) {
            kdtree.balance();
            balanceTargetPointCount = 2 * kdtree.size();
          }
        }
      }
    }
    numberOfPointsPerLOD.push_back(uint32_t(indexes.size()));
    prevIndexesSize = indexes.size();
  }
}

//---------------------------------------------------------------------------
// Update the neighbour pattern flags for a node and the 'left' neighbour on
// each axis.  This update should be applied to each newly inserted node.

void
updateGeometryNeighState(
  const ringbuf<PCCOctree3Node>::iterator& bufEnd,
  int64_t numNodesNextLvl, int childSizeLog2,
  PCCOctree3Node& child, int childIdx, uint8_t neighPattern,
  uint8_t parantOccupancy
) {
  uint64_t midx = child.mortonIdx = mortonAddr(child.pos, childSizeLog2);

  static const struct {
    int childIdxBitPos;
    int axis;
    int patternFlagUs;
    int patternFlagThem;
  } neighParamMap[] = {
    {4, 2, 1<<1, 1<<0}, // x
    {2, 1, 1<<2, 1<<3}, // y
    {1, 0, 1<<4, 1<<5}, // z
  };

  for (const auto& param : neighParamMap) {
    // skip expensive check if parent's flags indicate adjacent neighbour
    // is not present.
    if ((childIdx & param.childIdxBitPos) == 0) {
      // $axis co-ordinate = 0
      if (parantOccupancy & (1<<(childIdx + param.childIdxBitPos)))
        child.neighPattern |= param.patternFlagThem;

      if (!(neighPattern & param.patternFlagUs))
        continue;
    }
    else {
      if (parantOccupancy & (1<<(childIdx - param.childIdxBitPos)))
        child.neighPattern |= param.patternFlagUs;

      // no external search is required for $axis co-ordinate = 1
      continue;
    }

    // calculate the morton address of the 'left' neighbour,
    // the delta is then used as the starting position for a search
    int64_t mortonIdxNeigh =
      morton3dAxisDec(midx, param.axis) & ~0x8000000000000000ull;
    int64_t mortonDelta = midx - mortonIdxNeigh;

    if (mortonDelta < 0) {
      // no neighbour due to being in zero-th col/row/plane
      continue;
    }

    // NB: fifo already contains current node, no point searching it
    auto posEnd = bufEnd;
    std::advance(posEnd, -1);

    auto posStart = bufEnd;
    std::advance(posStart, -std::min(numNodesNextLvl, mortonDelta+2));

    auto found = std::lower_bound(posStart, posEnd, mortonIdxNeigh,
      [](const PCCOctree3Node& node, uint64_t mortonIdx) {
        return node.mortonIdx < mortonIdx;
      }
    );

    // NB: found is always valid (see posEnd) => can skip check.
    if (found->mortonIdx != mortonIdxNeigh) {
      // neighbour isn't present => must have been empty
      continue;
    }

    // update both node's neighbour pattern
    // NB: neighours being present implies occupancy
    child.neighPattern |= param.patternFlagUs;
    found->neighPattern |= param.patternFlagThem;
  }
}

//---------------------------------------------------------------------------
// Determine if direct coding is permitted.
// If tool is enabled:
//   - Block must not be near the bottom of the tree
//   - The parent / grandparent are sparsely occupied

bool
isDirectModeEligible(
  bool featureEnabled,
  int nodeSizeLog2, const PCCOctree3Node& node, const PCCOctree3Node& child
) {
  return featureEnabled && (nodeSizeLog2 >= 2)
      && (child.numSiblingsPlus1 == 1) && (node.numSiblingsPlus1 <= 2);
}

//---------------------------------------------------------------------------

}

#endif /* PCCTMC3Common_h */
