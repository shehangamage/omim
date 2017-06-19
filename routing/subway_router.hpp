#pragma once

#include "routing/num_mwm_id.hpp"
#include "routing/router.hpp"
#include "routing/subway_graph.hpp"
#include "routing/subway_model.hpp"
#include "routing/world_road_point.hpp"

#include "routing_common/vehicle_model.hpp"

#include "indexer/index.hpp"

#include <memory>
#include <vector>

namespace routing
{
class SubwayRouter final : public IRouter
{
public:
  SubwayRouter(std::shared_ptr<NumMwmIds> numMwmIds, Index & index);

  // IRouter overrides:
  virtual string GetName() const override { return "subway"; }
  virtual IRouter::ResultCode CalculateRoute(Checkpoints const & checkpoints,
                                             m2::PointD const & startDirection, bool adjust,
                                             RouterDelegate const & delegate, Route & route) override;

private:
  void SetGeometry(std::vector<WorldRoadPoint> const & vertexes, Route & route) const;
  void SetTimes(size_t routeSize, double time, Route & route) const;

  Index & m_index;
  std::shared_ptr<NumMwmIds> m_numMwmIds;
  SubwayModelFactory m_modelFactory;
  SubwayGraph m_graph;
};
}  // namespace routing