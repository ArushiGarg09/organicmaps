#pragma once

#include "generator/collector_interface.hpp"
#include "generator/intermediate_elements.hpp"
#include "generator/osm_element.hpp"

#include "routing/road_access.hpp"
#include "routing/vehicle_mask.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <map>
#include <ostream>
#include <string>
#include <vector>

struct OsmElement;
class FeatureParams;

// The road accessibility information is collected in the same manner
// as the restrictions are.
// See generator/restriction_generator.hpp for details.
namespace routing
{
class RoadAccessTagProcessor
{
public:
  using TagMapping = std::map<OsmElement::Tag, RoadAccess::Type>;

  explicit RoadAccessTagProcessor(VehicleType vehicleType);

  void Process(OsmElement const & elem, std::ofstream & oss);

private:
  bool HasCarAccessTag(OsmElement const & osmElement) const;
  bool ShouldIgnorePrivateAccess(OsmElement const & osmElement, bool hasAccessTag) const;
  RoadAccess::Type GetAccessType(OsmElement const & elem) const;

  VehicleType m_vehicleType;
  // Order of tag mappings in m_tagMappings is from more to less specific.
  // e.g. for car: motorcar, motorvehicle, vehicle, general access tags.
  std::vector<TagMapping const *> m_tagMappings;
  // Tag mapping for barriers. Key is barrier node osm id.
  std::map<uint64_t, std::pair<RoadAccess::Type, bool /* has access tag */>> m_barriers;
};

class RoadAccessWriter : public generator::CollectorInterface
{
public:
  RoadAccessWriter(std::string const & filePath);

  // CollectorInterface overrides:
  void CollectFeature(FeatureBuilder1 const &, OsmElement const & elem) override;
  void Save() override {}

private:
  void Open(std::string const & filePath);
  bool IsOpened() const;

  std::ofstream m_stream;
  std::vector<RoadAccessTagProcessor> m_tagProcessors;
};

class RoadAccessCollector
{
public:
  using RoadAccessByVehicleType = std::array<RoadAccess, static_cast<size_t>(VehicleType::Count)>;

  RoadAccessCollector(std::string const & dataFilePath, std::string const & roadAccessPath,
                      std::string const & osmIdsToFeatureIdsPath);

  RoadAccessByVehicleType const & GetRoadAccessAllTypes() const
  {
    return m_roadAccessByVehicleType;
  }

  bool IsValid() const { return m_valid; }

private:
  RoadAccessByVehicleType m_roadAccessByVehicleType;
  bool m_valid = true;
};

// The generator tool's interface to writing the section with
// road accessibility information for one mwm file.
void BuildRoadAccessInfo(std::string const & dataFilePath, std::string const & roadAccessPath,
                         std::string const & osmIdsToFeatureIdsPath);
}  // namespace routing
