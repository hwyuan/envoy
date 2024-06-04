#include "envoy/extensions/geoip_providers/maxmind/v3/maxmind.pb.h"
#include "envoy/registry/registry.h"

#include "source/common/network/address_impl.h"
#include "source/common/network/utility.h"
#include "source/extensions/geoip_providers/maxmind/config.h"
#include "source/extensions/geoip_providers/maxmind/geoip_provider.h"

#include "test/mocks/server/factory_context.h"
#include "test/mocks/stats/mocks.h"
#include "test/test_common/environment.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::InvokeWithoutArgs;
using testing::NiceMock;
using testing::ReturnRef;
using testing::SaveArg;

namespace Envoy {
namespace Extensions {
namespace GeoipProviders {
namespace Maxmind {

class GeoipProviderPeer {
public:
  static Stats::Scope& providerScope(const DriverSharedPtr& driver) {
    auto provider = std::static_pointer_cast<GeoipProvider>(driver);
    return provider->config_->getStatsScopeForTest();
  }
};

namespace {
const std::string default_city_db_path =
    "{{ test_rundir "
    "}}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-City-Test.mmdb";

const std::string default_updated_city_db_path =
    "{{ test_rundir "
    "}}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-City-Test-Updated.mmdb";

const std::string default_city_config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        country: "x-geo-country"
        region: "x-geo-region"
        city: "x-geo-city"
    city_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-City-Test.mmdb"
  )EOF";

const std::string default_isp_db_path =
    "{{ test_rundir "
    "}}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-ASN-Test.mmdb";

const std::string default_updated_isp_db_path =
    "{{ test_rundir "
    "}}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-ASN-Test-Updated.mmdb";

const std::string default_isp_config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        asn: "x-geo-asn"
    isp_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-ASN-Test.mmdb"
  )EOF";

const std::string default_anon_db_path =
    "{{ test_rundir "
    "}}/test/extensions/geoip_providers/maxmind/test_data/GeoIP2-Anonymous-IP-Test.mmdb";

const std::string default_updated_anon_db_path =
    "{{ test_rundir "
    "}}/test/extensions/geoip_providers/maxmind/test_data/GeoIP2-Anonymous-IP-Test-Updated.mmdb";

const std::string default_anon_config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        is_anon: "x-geo-anon"
    anon_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/GeoIP2-Anonymous-IP-Test.mmdb"
  )EOF";
} // namespace

class GeoipProviderTestBase {
public:
  GeoipProviderTestBase() : api_(Api::createApiForTest(stats_store_)) {
    provider_factory_ = dynamic_cast<MaxmindProviderFactory*>(
        Registry::FactoryRegistry<Geolocation::GeoipProviderFactory>::getFactory(
            "envoy.geoip_providers.maxmind"));
    ASSERT(provider_factory_);
  }

  void initializeProvider(const std::string& yaml) {
    EXPECT_CALL(context_, scope()).WillRepeatedly(ReturnRef(*scope_));
    EXPECT_CALL(context_, serverFactoryContext())
        .WillRepeatedly(ReturnRef(server_factory_context_));
    EXPECT_CALL(server_factory_context_, api()).WillRepeatedly(ReturnRef(*api_));
    EXPECT_CALL(dispatcher_, createFilesystemWatcher_()).WillRepeatedly(InvokeWithoutArgs([this] {
      Filesystem::MockWatcher* mock_watcher = new NiceMock<Filesystem::MockWatcher>();
      EXPECT_CALL(*mock_watcher, addWatch(_, Filesystem::Watcher::Events::MovedTo, _))
          .WillRepeatedly(
              Invoke([this](absl::string_view, uint32_t, Filesystem::Watcher::OnChangedCb cb) {
                on_changed_cbs_.emplace_back(cb);
                return absl::OkStatus();
              }));
      return mock_watcher;
    }));
    EXPECT_CALL(server_factory_context_, mainThreadDispatcher())
        .WillRepeatedly(ReturnRef(dispatcher_));
    envoy::extensions::geoip_providers::maxmind::v3::MaxMindConfig config;
    TestUtility::loadFromYaml(TestEnvironment::substitute(yaml), config);
    provider_ = provider_factory_->createGeoipProviderDriver(config, "prefix.", context_);
  }

  void expectStats(const std::string& db_type, const uint32_t total_count = 1,
                   const uint32_t hit_count = 1, const uint32_t error_count = 0) {
    auto& provider_scope = GeoipProviderPeer::providerScope(provider_);
    EXPECT_EQ(provider_scope.counterFromString(absl::StrCat(db_type, ".total")).value(),
              total_count);
    EXPECT_EQ(provider_scope.counterFromString(absl::StrCat(db_type, ".hit")).value(), hit_count);
    EXPECT_EQ(provider_scope.counterFromString(absl::StrCat(db_type, ".lookup_error")).value(),
              error_count);
  }

  void expectReloadStats(const std::string& db_type, const uint32_t reload_success_count = 0,
                         const uint32_t reload_error_count = 0) {
    auto& provider_scope = GeoipProviderPeer::providerScope(provider_);
    EXPECT_EQ(provider_scope.counterFromString(absl::StrCat(db_type, ".db_reload_success")).value(),
              reload_success_count);
    EXPECT_EQ(provider_scope.counterFromString(absl::StrCat(db_type, ".db_reload_error")).value(),
              reload_error_count);
  }

  Event::MockDispatcher dispatcher_;
  Stats::IsolatedStoreImpl stats_store_;
  Stats::ScopeSharedPtr scope_{stats_store_.createScope("")};
  Api::ApiPtr api_;
  NiceMock<Server::Configuration::MockServerFactoryContext> server_factory_context_;
  NiceMock<Server::Configuration::MockFactoryContext> context_;
  DriverSharedPtr provider_;
  MaxmindProviderFactory* provider_factory_;
  Event::SimulatedTimeSystem time_system_;
  absl::flat_hash_map<std::string, std::string> captured_lookup_response_;
  std::vector<Filesystem::Watcher::OnChangedCb> on_changed_cbs_;
};

class GeoipProviderTest : public testing::Test, public GeoipProviderTestBase {};

TEST_F(GeoipProviderTest, ValidConfigCityAndIspDbsSuccessfulLookup) {
  const std::string config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        country: "x-geo-country"
        region: "x-geo-region"
        city: "x-geo-city"
        asn: "x-geo-asn"
    city_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-City-Test.mmdb"
    isp_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-ASN-Test.mmdb"
  )EOF";
  initializeProvider(config_yaml);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress("78.26.243.166");
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std));
  EXPECT_EQ(4, captured_lookup_response_.size());
  const auto& city_it = captured_lookup_response_.find("x-geo-city");
  EXPECT_EQ("Boxford", city_it->second);
  const auto& region_it = captured_lookup_response_.find("x-geo-region");
  EXPECT_EQ("ENG", region_it->second);
  const auto& country_it = captured_lookup_response_.find("x-geo-country");
  EXPECT_EQ("GB", country_it->second);
  const auto& asn_it = captured_lookup_response_.find("x-geo-asn");
  EXPECT_EQ("15169", asn_it->second);
  expectStats("city_db");
  expectStats("isp_db");
}

TEST_F(GeoipProviderTest, ValidConfigCityLookupError) {
  const std::string config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        country: "x-geo-country"
        city: "x-geo-city"
    city_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/MaxMind-DB-test-ipv4-24.mmdb"
  )EOF";
  initializeProvider(config_yaml);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress("2345:0425:2CA1:0:0:0567:5673:23b5");
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std));
  expectStats("city_db", 1, 0, 1);
  EXPECT_EQ(0, captured_lookup_response_.size());
}

// Tests for anonymous database replicate expectations from corresponding Maxmind tests:
// https://github.com/maxmind/GeoIP2-perl/blob/main/t/GeoIP2/Database/Reader-Anonymous-IP.t
TEST_F(GeoipProviderTest, ValidConfigAnonVpnSuccessfulLookup) {
  const std::string config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        is_anon: "x-geo-anon"
        anon_vpn: "x-geo-anon-vpn"
    anon_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/GeoIP2-Anonymous-IP-Test.mmdb"
  )EOF";
  initializeProvider(config_yaml);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress("1.2.0.0");
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std));
  EXPECT_EQ(2, captured_lookup_response_.size());
  const auto& anon_it = captured_lookup_response_.find("x-geo-anon");
  EXPECT_EQ("true", anon_it->second);
  const auto& anon_vpn_it = captured_lookup_response_.find("x-geo-anon-vpn");
  EXPECT_EQ("true", anon_vpn_it->second);
  expectStats("anon_db");
}

TEST_F(GeoipProviderTest, ValidConfigAnonHostingSuccessfulLookup) {
  const std::string config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        is_anon: "x-geo-anon"
        anon_hosting: "x-geo-anon-hosting"
    anon_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/GeoIP2-Anonymous-IP-Test.mmdb"
  )EOF";
  initializeProvider(config_yaml);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress("71.160.223.45");
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std));
  EXPECT_EQ(2, captured_lookup_response_.size());
  const auto& anon_it = captured_lookup_response_.find("x-geo-anon");
  EXPECT_EQ("true", anon_it->second);
  const auto& anon_hosting_it = captured_lookup_response_.find("x-geo-anon-hosting");
  EXPECT_EQ("true", anon_hosting_it->second);
  expectStats("anon_db");
}

TEST_F(GeoipProviderTest, ValidConfigAnonTorNodeSuccessfulLookup) {
  const std::string config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        is_anon: "x-geo-anon"
        anon_tor: "x-geo-anon-tor"
    anon_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/GeoIP2-Anonymous-IP-Test.mmdb"
  )EOF";
  initializeProvider(config_yaml);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress("65.4.3.2");
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std));
  EXPECT_EQ(2, captured_lookup_response_.size());
  const auto& anon_it = captured_lookup_response_.find("x-geo-anon");
  EXPECT_EQ("true", anon_it->second);
  const auto& anon_tor_it = captured_lookup_response_.find("x-geo-anon-tor");
  EXPECT_EQ("true", anon_tor_it->second);
  expectStats("anon_db");
}

TEST_F(GeoipProviderTest, ValidConfigAnonProxySuccessfulLookup) {
  const std::string config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        is_anon: "x-geo-anon"
        anon_proxy: "x-geo-anon-proxy"
    anon_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/GeoIP2-Anonymous-IP-Test.mmdb"
  )EOF";
  initializeProvider(config_yaml);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress("abcd:1000::1");
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std));
  EXPECT_EQ(2, captured_lookup_response_.size());
  const auto& anon_it = captured_lookup_response_.find("x-geo-anon");
  EXPECT_EQ("true", anon_it->second);
  const auto& anon_tor_it = captured_lookup_response_.find("x-geo-anon-proxy");
  EXPECT_EQ("true", anon_tor_it->second);
  expectStats("anon_db");
}

TEST_F(GeoipProviderTest, ValidConfigEmptyLookupResult) {
  initializeProvider(default_anon_config_yaml);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress("10.10.10.10");
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std));
  EXPECT_EQ(0, captured_lookup_response_.size());
  expectStats("anon_db", 1, 0);
}

TEST_F(GeoipProviderTest, ValidConfigCityMultipleLookups) {
  initializeProvider(default_city_config_yaml);
  Network::Address::InstanceConstSharedPtr remote_address1 =
      Network::Utility::parseInternetAddress("78.26.243.166");
  Geolocation::LookupRequest lookup_rq1{std::move(remote_address1)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq1), std::move(lookup_cb_std));
  EXPECT_EQ(3, captured_lookup_response_.size());
  // Another lookup request.
  Network::Address::InstanceConstSharedPtr remote_address2 =
      Network::Utility::parseInternetAddress("63.25.243.11");
  Geolocation::LookupRequest lookup_rq2{std::move(remote_address2)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb2;
  auto lookup_cb_std2 = lookup_cb2.AsStdFunction();
  EXPECT_CALL(lookup_cb2, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq2), std::move(lookup_cb_std2));
  EXPECT_EQ(3, captured_lookup_response_.size());
  expectStats("city_db", 2, 2);
}

TEST_F(GeoipProviderTest, DbReloadedOnMmdbFileUpdate) {
  constexpr absl::string_view config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        country: "x-geo-country"
        region: "x-geo-region"
        city: "x-geo-city"
    city_db_path: {}
  )EOF";
  std::string city_db_path = TestEnvironment::substitute(
      "{{ test_rundir "
      "}}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-City-Test.mmdb");
  std::string reloaded_city_db_path = TestEnvironment::substitute(
      "{{ test_rundir "
      "}}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-City-Test-Updated.mmdb");
  const std::string formatted_config =
      fmt::format(config_yaml, TestEnvironment::substitute(city_db_path));
  initializeProvider(formatted_config);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress("78.26.243.166");
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std));
  EXPECT_EQ(3, captured_lookup_response_.size());
  const auto& city_it = captured_lookup_response_.find("x-geo-city");
  EXPECT_EQ("Boxford", city_it->second);
  TestEnvironment::renameFile(city_db_path, city_db_path + "1");
  TestEnvironment::renameFile(reloaded_city_db_path, city_db_path);
  EXPECT_TRUE(on_changed_cbs_[0](Filesystem::Watcher::Events::MovedTo).ok());
  expectReloadStats("city_db", 1, 0);
  captured_lookup_response_.clear();
  EXPECT_EQ(0, captured_lookup_response_.size());
  remote_address = Network::Utility::parseInternetAddress("78.26.243.166");
  Geolocation::LookupRequest lookup_rq2{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb2;
  auto lookup_cb_std2 = lookup_cb2.AsStdFunction();
  EXPECT_CALL(lookup_cb2, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq2), std::move(lookup_cb_std2));
  const auto& city1_it = captured_lookup_response_.find("x-geo-city");
  EXPECT_EQ("BoxfordImaginary", city1_it->second);
  // Clean up modifications to mmdb file names.
  TestEnvironment::renameFile(city_db_path, reloaded_city_db_path);
  TestEnvironment::renameFile(city_db_path + "1", city_db_path);
}

using GeoipProviderDeathTest = GeoipProviderTest;

TEST_F(GeoipProviderDeathTest, GeoDbNotSetForConfiguredHeader) {
  const std::string config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        city: "x-geo-city"
        asn: "x-geo-asn"
    city_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data/GeoLite2-City-Test.mmdb"
  )EOF";
  initializeProvider(config_yaml);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress("78.26.243.166");
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  EXPECT_DEATH(provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std)),
               "assert failure: isp_db_. Details: Maxmind asn database is not initialised for "
               "performing lookups");
}

TEST_F(GeoipProviderDeathTest, GeoDbPathDoesNotExist) {
  const std::string config_yaml = R"EOF(
    common_provider_config:
      geo_headers_to_add:
        city: "x-geo-city"
    city_db_path: "{{ test_rundir }}/test/extensions/geoip_providers/maxmind/test_data_atc/GeoLite2-City-Test.mmdb"
  )EOF";
  EXPECT_DEATH(initializeProvider(config_yaml), ".*Unable to open Maxmind database file.*");
}

struct MmdbReloadTestCase {

  MmdbReloadTestCase() = default;
  MmdbReloadTestCase(const std::string& yaml_config, const std::string& db_type,
                     const std::string& source_db_file_path,
                     const std::string& reloaded_db_file_path,
                     const std::string& expected_header_name,
                     const std::string& expected_header_value,
                     const std::string& expected_reloaded_header_value, const std::string& ip)
      : yaml_config_(yaml_config), db_type_(db_type), source_db_file_path_(source_db_file_path),
        reloaded_db_file_path_(reloaded_db_file_path), expected_header_name_(expected_header_name),
        expected_header_value_(expected_header_value),
        expected_reloaded_header_value_(expected_reloaded_header_value), ip_(ip) {}
  MmdbReloadTestCase(const MmdbReloadTestCase& rhs) = default;

  std::string yaml_config_;
  std::string db_type_;
  std::string source_db_file_path_;
  std::string reloaded_db_file_path_;
  std::string expected_header_name_;
  std::string expected_header_value_;
  std::string expected_reloaded_header_value_;
  std::string ip_;
};

class MmdbReloadImplTest : public ::testing::TestWithParam<MmdbReloadTestCase>,
                           public GeoipProviderTestBase {};

TEST_P(MmdbReloadImplTest, MmdbReloaded) {
  MmdbReloadTestCase test_case = GetParam();
  initializeProvider(test_case.yaml_config_);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress(test_case.ip_);
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std));
  const auto& geoip_header_it = captured_lookup_response_.find(test_case.expected_header_name_);
  EXPECT_EQ(test_case.expected_header_value_, geoip_header_it->second);
  expectStats(test_case.db_type_, 1, 1);
  std::string source_db_file_path = TestEnvironment::substitute(test_case.source_db_file_path_);
  std::string reloaded_db_file_path = TestEnvironment::substitute(test_case.reloaded_db_file_path_);
  TestEnvironment::renameFile(source_db_file_path, source_db_file_path + "1");
  TestEnvironment::renameFile(reloaded_db_file_path, source_db_file_path);
  EXPECT_TRUE(on_changed_cbs_[0](Filesystem::Watcher::Events::MovedTo).ok());
  expectReloadStats(test_case.db_type_, 1, 0);
  captured_lookup_response_.clear();
  remote_address = Network::Utility::parseInternetAddress(test_case.ip_);
  Geolocation::LookupRequest lookup_rq2{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb2;
  auto lookup_cb_std2 = lookup_cb2.AsStdFunction();
  EXPECT_CALL(lookup_cb2, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq2), std::move(lookup_cb_std2));
  const auto& geoip_header1_it = captured_lookup_response_.find(test_case.expected_header_name_);
  EXPECT_EQ(test_case.expected_reloaded_header_value_, geoip_header1_it->second);
  // Clean up modifications to mmdb file names.
  TestEnvironment::renameFile(source_db_file_path, reloaded_db_file_path);
  TestEnvironment::renameFile(source_db_file_path + "1", source_db_file_path);
}

TEST_P(MmdbReloadImplTest, MmdbNotReloadedRuntimeFeatureDisabled) {
  TestScopedRuntime scoped_runtime_;
  scoped_runtime_.mergeValues({{"envoy.reloadable_features.mmdb_files_reload_enabled", "false"}});
  MmdbReloadTestCase test_case = GetParam();
  initializeProvider(test_case.yaml_config_);
  Network::Address::InstanceConstSharedPtr remote_address =
      Network::Utility::parseInternetAddress(test_case.ip_);
  Geolocation::LookupRequest lookup_rq{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb;
  auto lookup_cb_std = lookup_cb.AsStdFunction();
  EXPECT_CALL(lookup_cb, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq), std::move(lookup_cb_std));
  const auto& geoip_header_it = captured_lookup_response_.find(test_case.expected_header_name_);
  EXPECT_EQ(test_case.expected_header_value_, geoip_header_it->second);
  expectStats(test_case.db_type_, 1, 1);
  std::string source_db_file_path = TestEnvironment::substitute(test_case.source_db_file_path_);
  std::string reloaded_db_file_path = TestEnvironment::substitute(test_case.reloaded_db_file_path_);
  TestEnvironment::renameFile(source_db_file_path, source_db_file_path + "1");
  TestEnvironment::renameFile(reloaded_db_file_path, source_db_file_path);
  EXPECT_EQ(0, on_changed_cbs_.size());
  expectReloadStats(test_case.db_type_, 0, 0);
  captured_lookup_response_.clear();
  remote_address = Network::Utility::parseInternetAddress(test_case.ip_);
  Geolocation::LookupRequest lookup_rq2{std::move(remote_address)};
  testing::MockFunction<void(Geolocation::LookupResult &&)> lookup_cb2;
  auto lookup_cb_std2 = lookup_cb2.AsStdFunction();
  EXPECT_CALL(lookup_cb2, Call(_)).WillRepeatedly(SaveArg<0>(&captured_lookup_response_));
  provider_->lookup(std::move(lookup_rq2), std::move(lookup_cb_std2));
  const auto& geoip_header1_it = captured_lookup_response_.find(test_case.expected_header_name_);
  EXPECT_EQ(test_case.expected_header_value_, geoip_header1_it->second);
  // Clean up modifications to mmdb file names.
  TestEnvironment::renameFile(source_db_file_path, reloaded_db_file_path);
  TestEnvironment::renameFile(source_db_file_path + "1", source_db_file_path);
}

struct MmdbReloadTestCase mmdb_reload_test_cases[] = {
    {default_city_config_yaml, "city_db", default_city_db_path, default_updated_city_db_path,
     "x-geo-city", "Boxford", "BoxfordImaginary", "78.26.243.166"},
    {default_isp_config_yaml, "isp_db", default_isp_db_path, default_updated_isp_db_path,
     "x-geo-asn", "15169", "77777", "78.26.243.166"},
    {default_anon_config_yaml, "anon_db", default_anon_db_path, default_updated_anon_db_path,
     "x-geo-anon", "true", "false", "65.4.3.2"},
};

INSTANTIATE_TEST_SUITE_P(TestName, MmdbReloadImplTest, ::testing::ValuesIn(mmdb_reload_test_cases));

} // namespace Maxmind
} // namespace GeoipProviders
} // namespace Extensions
} // namespace Envoy
