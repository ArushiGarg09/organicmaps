#include "cross_mwm_road_graph.hpp"
#include "cross_mwm_router.hpp"

#include "geometry/distance_on_sphere.hpp"

#include <limits>

namespace
{
using namespace routing;

inline bool IsValidEdgeWeight(EdgeWeight const & w) { return w != INVALID_EDGE_WEIGHT; }

template<class Node>
struct FindingNode
{
  FindingNode(CrossNode const & node, double & minDistance, Node & resultingCrossNode)
    : m_node(node), m_minDistance(minDistance), m_resultingCrossNode(resultingCrossNode)
  {
  }

  void operator()(Node const & crossNode) const
  {
    if (crossNode.m_nodeId != m_node.node)
      return;

    double const dist = ms::DistanceOnEarth(m_node.point, crossNode.m_point);
    if (dist < m_minDistance)
    {
      m_minDistance = dist;
      m_resultingCrossNode = crossNode;
    }
  }

  CrossNode const & m_node;
  double & m_minDistance;
  Node & m_resultingCrossNode;
};

double GetAdjacencyCost(CrossRoutingContextReader const & currentContext,
                        IngoingCrossNode const & ingoingCrossNode,
                        OutgoingCrossNode const & outgoingCrossNode)
{
  return currentContext.GetAdjacencyCost(ingoingCrossNode, outgoingCrossNode);
}

double GetAdjacencyCost(CrossRoutingContextReader const & currentContext,
                        OutgoingCrossNode const & outgoingCrossNode,
                        IngoingCrossNode const & ingoingCrossNode)
{
  return GetAdjacencyCost(currentContext, ingoingCrossNode, outgoingCrossNode);
}

template<class SourceNode, class TargetNode, bool isOutgoing>
struct FillingEdges
{
  FillingEdges(TRoutingMappingPtr const & currentMapping, CrossRoutingContextReader const & currentContext,
               SourceNode const & startingNode, CrossMwmGraph const & crossMwmGraph, vector<CrossWeightedEdge> & adj)
    : m_currentMapping(currentMapping), m_currentContext(currentContext), m_startingNode(startingNode),
      m_crossMwmGraph(crossMwmGraph), m_adj(adj)
  {
  }

  void operator()(TargetNode const & node) const
  {
    TWrittenEdgeWeight const outWeight = GetAdjacencyCost(m_currentContext, m_startingNode, node);
    if (outWeight != kInvalidContextEdgeWeight && outWeight != 0)
    {
      vector<BorderCross> const & targets = m_crossMwmGraph.ConstructBorderCross(m_currentMapping, node);
      for (auto const & target : targets)
      {
        if (target.toNode.IsValid())
          m_adj.emplace_back(target, outWeight);
      }
    }
  }

  TRoutingMappingPtr const & m_currentMapping;
  CrossRoutingContextReader const & m_currentContext;
  SourceNode const & m_startingNode;
  CrossMwmGraph const & m_crossMwmGraph;
  vector<CrossWeightedEdge> & m_adj;
};

void FindIngoingCrossNode(CrossRoutingContextReader const & currentContext, CrossNode const & toNode,
                          IngoingCrossNode & ingoingNode)
{
  double minDistance = std::numeric_limits<double>::max();
  FindingNode<IngoingCrossNode> findingNode(toNode, minDistance, ingoingNode);

  CHECK(currentContext.ForEachIngoingNodeNearPoint(toNode.point, findingNode), ("toNode.point:", toNode.point));
  CHECK_NOT_EQUAL(minDistance, std::numeric_limits<double>::max(), ("toNode.point:", toNode.point));
}

void FindOutgoingCrossNode(CrossRoutingContextReader const & currentContext, CrossNode const & fromNode,
                           OutgoingCrossNode & outgoingNode)
{
  double minDistance = std::numeric_limits<double>::max();
  FindingNode<OutgoingCrossNode> findingNode(fromNode, minDistance, outgoingNode);
  CHECK(currentContext.ForEachOutgoingNodeNearPoint(fromNode.point, findingNode), ("fromNode.point:", fromNode.point));
  CHECK_NOT_EQUAL(minDistance, std::numeric_limits<double>::max(), ("fromNode.point:", fromNode.point));
}

vector<BorderCross> & FindBorderCross(TWrittenNodeId nodeId,
                                      TRoutingMappingPtr const & currentMapping,
                                      unordered_map<CrossMwmGraph::TCachingKey, vector<BorderCross>,
                                          CrossMwmGraph::Hash> & cachedNextNodes,
                                      bool & result)
{
  auto const key = make_pair(nodeId, currentMapping->GetMwmId());
  auto const it = cachedNextNodes.find(key);
  result = (it == cachedNextNodes.end()) ? false : true;
  if (it != cachedNextNodes.end())
  {
    result = true;
    return it->second;
  }

  result = false;
  return cachedNextNodes[key];
}
}  // namespace

namespace routing
{
IRouter::ResultCode CrossMwmGraph::SetStartNode(CrossNode const & startNode)
{
  ASSERT(startNode.mwmId.IsAlive(), ());
  // TODO (ldragunov) make cancellation if necessary
  TRoutingMappingPtr startMapping = m_indexManager.GetMappingById(startNode.mwmId);
  if (!startMapping->IsValid())
      return IRouter::ResultCode::StartPointNotFound;
  MappingGuard startMappingGuard(startMapping);
  UNUSED_VALUE(startMappingGuard);
  startMapping->LoadCrossContext();

  // Load source data.
  vector<OutgoingCrossNode> outgoingNodes;

  startMapping->m_crossContext.ForEachOutgoingNode([&outgoingNodes](OutgoingCrossNode const & node)
                                                   {
                                                     outgoingNodes.push_back(node);
                                                   });

  size_t const outSize = outgoingNodes.size();
  // Can't find the route if there are no routes outside source map.
  if (!outSize)
    return IRouter::RouteNotFound;

  // Generate routing task from one source to several targets.
  TRoutingNodes sources(1), targets;
  targets.reserve(outSize);
  for (auto const & node : outgoingNodes)
  {
    targets.emplace_back(node.m_nodeId, false /* isStartNode */, startNode.mwmId);
    targets.back().segmentPoint = MercatorBounds::FromLatLon(node.m_point);
  }
  sources[0] = FeatureGraphNode(startNode.node, startNode.reverseNode, true /* isStartNode */,
                                startNode.mwmId);

  vector<EdgeWeight> weights;
  FindWeightsMatrix(sources, targets, startMapping->m_dataFacade, weights);
  if (find_if(weights.begin(), weights.end(), &IsValidEdgeWeight) == weights.end())
    return IRouter::StartPointNotFound;
  vector<CrossWeightedEdge> dummyEdges;
  for (size_t i = 0; i < outSize; ++i)
  {
    if (IsValidEdgeWeight(weights[i]))
    {
      vector<BorderCross> const & nextCrosses = ConstructBorderCrossByOutgoingNode(outgoingNodes[i], startMapping);
      for (auto const & nextCross : nextCrosses)
      {
        if (nextCross.toNode.IsValid())
          dummyEdges.emplace_back(nextCross, weights[i]);
      }
    }
  }

  m_virtualEdges.insert(make_pair(startNode, dummyEdges));
  return IRouter::NoError;
}

void CrossMwmGraph::AddVirtualEdge(IngoingCrossNode const & node, CrossNode const & finalNode,
                                   EdgeWeight weight)
{
  CrossNode start(node.m_nodeId, finalNode.mwmId, node.m_point);
  vector<CrossWeightedEdge> dummyEdges;
  dummyEdges.emplace_back(BorderCross(finalNode, finalNode), weight);
  m_virtualEdges.insert(make_pair(start, dummyEdges));
}

IRouter::ResultCode CrossMwmGraph::SetFinalNode(CrossNode const & finalNode)
{
  ASSERT(finalNode.mwmId.IsAlive(), ());
  TRoutingMappingPtr finalMapping = m_indexManager.GetMappingById(finalNode.mwmId);
  if (!finalMapping->IsValid())
      return IRouter::ResultCode::EndPointNotFound;
  MappingGuard finalMappingGuard(finalMapping);
  UNUSED_VALUE(finalMappingGuard);
  finalMapping->LoadCrossContext();

  // Load source data.
  vector<IngoingCrossNode> ingoingNodes;
  finalMapping->m_crossContext.ForEachIngoingNode([&ingoingNodes](IngoingCrossNode const & node)
                                                   {
                                                     ingoingNodes.push_back(node);
                                                   });
  size_t const ingoingSize = ingoingNodes.size();
  // If there is no routes inside target map.
  if (ingoingSize == 0)
    return IRouter::RouteNotFound;

  // Generate routing task from one source to several targets.
  TRoutingNodes sources, targets(1);
  sources.reserve(ingoingSize);

  for (auto const & node : ingoingNodes)
  {
    // Case with a target node at the income mwm node.
    if (node.m_nodeId == finalNode.node)
    {
      AddVirtualEdge(node, finalNode, 0 /* no weight */);
      return IRouter::NoError;
    }
    sources.emplace_back(node.m_nodeId, true /* isStartNode */, finalNode.mwmId);
    sources.back().segmentPoint = MercatorBounds::FromLatLon(node.m_point);
  }
  vector<EdgeWeight> weights;

  targets[0] = FeatureGraphNode(finalNode.node, finalNode.reverseNode, false /* isStartNode */,
                                finalNode.mwmId);
  FindWeightsMatrix(sources, targets, finalMapping->m_dataFacade, weights);
  if (find_if(weights.begin(), weights.end(), &IsValidEdgeWeight) == weights.end())
    return IRouter::EndPointNotFound;
  for (size_t i = 0; i < ingoingSize; ++i)
  {
    if (IsValidEdgeWeight(weights[i]))
    {
      AddVirtualEdge(ingoingNodes[i], finalNode, weights[i]);
    }
  }
  return IRouter::NoError;
}

vector<BorderCross> const & CrossMwmGraph::ConstructBorderCrossByOutgoingNode(OutgoingCrossNode const & startNode,
                                                                              TRoutingMappingPtr const & currentMapping) const
{
  bool result = false;
  vector<BorderCross> & crosses = FindBorderCross(startNode.m_nodeId, currentMapping, m_cachedNextNodesByOutgoing, result);
  if (result)
    return crosses;

  ConstructBorderCrossByOutgoingImpl(startNode, currentMapping, crosses);
  return crosses;
}

vector<BorderCross> const & CrossMwmGraph::ConstructBorderCrossByIngoingNode(IngoingCrossNode const & startNode,
                                                                             TRoutingMappingPtr const & currentMapping) const
{
  bool result = false;
  vector<BorderCross> & crosses = FindBorderCross(startNode.m_nodeId, currentMapping, m_cachedNextNodesByIngoing, result);
  if (result)
    return crosses;

  ConstructBorderCrossByIngoingImpl(startNode, currentMapping, crosses);
  return crosses;
}

bool CrossMwmGraph::ConstructBorderCrossByOutgoingImpl(OutgoingCrossNode const & startNode,
                                                       TRoutingMappingPtr const & currentMapping,
                                                       vector<BorderCross> & crosses) const
{
  auto const fromCross = CrossNode(startNode.m_nodeId, currentMapping->GetMwmId(), startNode.m_point);
  string const & nextMwm = currentMapping->m_crossContext.GetOutgoingMwmName(startNode);
  TRoutingMappingPtr nextMapping = m_indexManager.GetMappingByName(nextMwm);
  // If we haven't this routing file, we skip this path.
  if (!nextMapping->IsValid())
    return false;
  ASSERT(crosses.empty(), ());
  nextMapping->LoadCrossContext();
  nextMapping->m_crossContext.ForEachIngoingNodeNearPoint(startNode.m_point, [&](IngoingCrossNode const & node)
  {
    if (node.m_nodeId == INVALID_NODE_ID)
      return;

    if (!node.m_point.EqualDxDy(startNode.m_point, kMwmCrossingNodeEqualityMeters * MercatorBounds::degreeInMetres))
      return;

    crosses.emplace_back(fromCross, CrossNode(node.m_nodeId, nextMapping->GetMwmId(), node.m_point));
  });
  return !crosses.empty();
}

bool CrossMwmGraph::ConstructBorderCrossByIngoingImpl(IngoingCrossNode const & startNode,
                                                      TRoutingMappingPtr const & currentMapping,
                                                      vector<BorderCross> & crosses) const
{
  ASSERT(crosses.empty(), ());
  auto const toCross = CrossNode(startNode.m_nodeId, currentMapping->GetMwmId(), startNode.m_point);
  vector<string> const & neighboringMwms = currentMapping->m_crossContext.GetNeighboringMwmList();
  string const & currentMwm = currentMapping->GetMwmId().GetInfo()->GetCountryName();
  // Note. There's no field |m_ingoingIndex| in class IngoingCrossNode. Because this
  // index is not saved in osrm routing section. So we need to write a workaround and
  // to check all neighboring mwms.
  for (string const & prevMwm : neighboringMwms)
  {
    TRoutingMappingPtr nextMapping = m_indexManager.GetMappingByName(prevMwm);
    if (!nextMapping->IsValid())
      continue;

    nextMapping->LoadCrossContext();
    nextMapping->m_crossContext.ForEachOutgoingNodeNearPoint(startNode.m_point, [&](OutgoingCrossNode const & node){
      if (node.m_nodeId == INVALID_NODE_ID)
        return;

      if (nextMapping->m_crossContext.GetOutgoingMwmName(node) != currentMwm)
        return;

      if (!node.m_point.EqualDxDy(startNode.m_point, kMwmCrossingNodeEqualityMeters * MercatorBounds::degreeInMetres))
        return;

      crosses.emplace_back(CrossNode(node.m_nodeId, nextMapping->GetMwmId(), node.m_point), toCross);
    });
  }
  return !crosses.empty();
}

vector<BorderCross> const & CrossMwmGraph::ConstructBorderCross(TRoutingMappingPtr const & currentMapping,
                                                                OutgoingCrossNode const & node) const
{
  return ConstructBorderCrossByOutgoingNode(node, currentMapping);
}

vector<BorderCross> const & CrossMwmGraph::ConstructBorderCross(TRoutingMappingPtr const & currentMapping,
                                                                IngoingCrossNode const & node) const
{
  return ConstructBorderCrossByIngoingNode(node, currentMapping);
}

void CrossMwmGraph::GetEdgesList(BorderCross const & v, bool isOutgoing, vector<CrossWeightedEdge> & adj) const
{
  // Check for virtual edges.
  adj.clear();

  // Note. Code below processes virtual edges. This code does not work properly if isOutgoing == false.
  // At the same time when this method is called with isOutgoing == false |m_virtualEdges| is empty.
  if (!isOutgoing && !m_virtualEdges.empty())
  {
    NOTIMPLEMENTED();
    return;
  }

  auto const it = m_virtualEdges.find(v.toNode);
  if (it != m_virtualEdges.end())
  {
    adj.insert(adj.end(), it->second.begin(), it->second.end());
    // For last map we need to load virtual shortcuts and real cross roads. It takes to account case
    // when we have a path from the mwm border to the point inside the map throuh another map.
    // See Ust-Katav test for more.
    if (it->second.empty() || !it->second.front().GetTarget().toNode.isVirtual)
      return;
  }

  // Loading cross routing section.
  TRoutingMappingPtr currentMapping = m_indexManager.GetMappingById(isOutgoing ? v.toNode.mwmId
                                                                               : v.fromNode.mwmId);
  ASSERT(currentMapping->IsValid(), ());
  currentMapping->LoadCrossContext();
  currentMapping->FreeFileIfPossible();

  CrossRoutingContextReader const & currentContext = currentMapping->m_crossContext;

  if (isOutgoing)
  {
    IngoingCrossNode ingoingNode;
    FindIngoingCrossNode(currentContext, v.toNode, ingoingNode);
    currentContext.ForEachOutgoingNode(FillingEdges<IngoingCrossNode, OutgoingCrossNode, true /* isOutgoing */>(
        currentMapping, currentContext, ingoingNode, *this, adj));
  }
  else
  {
    OutgoingCrossNode outgoingNode;
    FindOutgoingCrossNode(currentContext, v.fromNode, outgoingNode);
    currentContext.ForEachIngoingNode(FillingEdges<OutgoingCrossNode, IngoingCrossNode, false /* isOutgoing */>(
        currentMapping, currentContext, outgoingNode, *this, adj));
  }
}

double CrossMwmGraph::HeuristicCostEstimate(BorderCross const & v, BorderCross const & w) const
{
  // Simple travel time heuristic works worse than simple Dijkstra's algorithm, represented by
  // always 0 heuristics estimation.
  return 0;
}

void ConvertToSingleRouterTasks(vector<BorderCross> const & graphCrosses,
                                FeatureGraphNode const & startGraphNode,
                                FeatureGraphNode const & finalGraphNode, TCheckedPath & route)
{
  route.clear();
  for (size_t i = 0; i + 1 < graphCrosses.size(); ++i)
  {
    ASSERT_EQUAL(graphCrosses[i].toNode.mwmId, graphCrosses[i + 1].fromNode.mwmId, ());
    route.emplace_back(graphCrosses[i].toNode.node,
                       MercatorBounds::FromLatLon(graphCrosses[i].toNode.point),
                       graphCrosses[i + 1].fromNode.node,
                       MercatorBounds::FromLatLon(graphCrosses[i + 1].fromNode.point),
                       graphCrosses[i].toNode.mwmId);
  }

  if (route.empty())
    return;

  route.front().startNode = startGraphNode;
  route.back().finalNode = finalGraphNode;
  ASSERT_EQUAL(route.front().startNode.mwmId, route.front().finalNode.mwmId, ());
  ASSERT_EQUAL(route.back().startNode.mwmId, route.back().finalNode.mwmId, ());
}

}  // namespace routing
