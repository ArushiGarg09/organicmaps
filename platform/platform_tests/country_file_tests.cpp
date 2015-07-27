#include "testing/testing.hpp"

#include "defines.hpp"
#include "platform/country_file.hpp"

namespace platform
{
UNIT_TEST(CountryFile_Smoke)
{
  CountryFile countryFile("TestCountry");
  TEST_EQUAL("TestCountry", countryFile.GetNameWithoutExt(), ());
  TEST_EQUAL("TestCountry" DATA_FILE_EXTENSION, countryFile.GetNameWithExt(TMapOptions::Map), ());
  TEST_EQUAL("TestCountry" DATA_FILE_EXTENSION ROUTING_FILE_EXTENSION,
             countryFile.GetNameWithExt(TMapOptions::CarRouting), ());

  TEST_EQUAL(0, countryFile.GetRemoteSize(TMapOptions::Nothing), ());
  TEST_EQUAL(0, countryFile.GetRemoteSize(TMapOptions::Map), ());
  TEST_EQUAL(0, countryFile.GetRemoteSize(TMapOptions::CarRouting), ());
  TEST_EQUAL(0, countryFile.GetRemoteSize(TMapOptions::MapWithCarRouting), ());

  countryFile.SetRemoteSizes(1 /* mapSize */, 2 /* routingSize */);

  TEST_EQUAL(0, countryFile.GetRemoteSize(TMapOptions::Nothing), ());
  TEST_EQUAL(1, countryFile.GetRemoteSize(TMapOptions::Map), ());
  TEST_EQUAL(2, countryFile.GetRemoteSize(TMapOptions::CarRouting), ());
  TEST_EQUAL(3, countryFile.GetRemoteSize(TMapOptions::MapWithCarRouting), ());
}
}  // namespace platform
