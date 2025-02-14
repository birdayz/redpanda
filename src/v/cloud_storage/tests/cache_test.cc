/*
 * Copyright 2021 Redpanda Data, Inc.
 *
 * Licensed as a Redpanda Enterprise file under the Redpanda Community
 * License (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 * https://github.com/redpanda-data/redpanda/blob/master/licenses/rcl.md
 */

#include "bytes/iobuf.h"
#include "cache_test_fixture.h"
#include "cloud_storage/access_time_tracker.h"
#include "cloud_storage/cache_service.h"
#include "test_utils/fixture.h"
#include "units.h"

#include <seastar/core/fstream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/sleep.hh>
#include <seastar/util/file.hh>

#include <boost/test/tools/old/interface.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <stdexcept>

using namespace cloud_storage;

FIXTURE_TEST(put_creates_file, cache_test_fixture) {
    auto data_string = create_data_string('a', 1_MiB + 1_KiB);
    put_into_cache(data_string, KEY);

    bool file_exists = ss::file_exists((CACHE_DIR / KEY).native()).get();
    BOOST_CHECK(file_exists);
}

FIXTURE_TEST(get_after_put, cache_test_fixture) {
    auto data_string = create_data_string('a', 1_MiB + 1_KiB);
    put_into_cache(data_string, KEY);

    std::optional<cloud_storage::cache_item> returned_item
      = sharded_cache.local().get(KEY).get();
    BOOST_REQUIRE(returned_item);
    BOOST_CHECK_EQUAL(returned_item->size, data_string.length());

    auto stream = ss::make_file_input_stream(returned_item->body);

    auto read_buf = stream.read_exactly(data_string.length()).get();
    BOOST_CHECK_EQUAL(
      std::string_view(read_buf.get(), read_buf.size()), data_string);
    BOOST_CHECK(!stream.read().get().get());
    stream.close().get();
}

FIXTURE_TEST(put_rewrites_file, cache_test_fixture) {
    auto data_string1 = create_data_string('a', 1_MiB + 1_KiB);
    put_into_cache(data_string1, KEY);

    auto data_string2 = create_data_string('b', 1_MiB + 1_KiB);
    put_into_cache(data_string2, KEY);

    std::optional<cloud_storage::cache_item> returned_item
      = sharded_cache.local().get(KEY).get();
    BOOST_REQUIRE(returned_item);
    BOOST_CHECK_EQUAL(returned_item->size, data_string2.length());

    auto body = ss::make_file_input_stream(returned_item->body);

    auto read_buf = body.read_exactly(data_string2.length()).get();
    BOOST_CHECK_EQUAL(
      std::string_view(read_buf.get(), read_buf.size()), data_string2);
    BOOST_CHECK(!body.read().get().get());
    body.close().get();
}

FIXTURE_TEST(get_missing_file, cache_test_fixture) {
    std::optional<cloud_storage::cache_item> returned_item
      = sharded_cache.local().get(WRONG_KEY).get();

    BOOST_CHECK(!returned_item);
}

FIXTURE_TEST(missing_file_not_cached, cache_test_fixture) {
    auto is_cached = sharded_cache.local().is_cached(WRONG_KEY).get();

    BOOST_CHECK_EQUAL(is_cached, cache_element_status::not_available);
}

FIXTURE_TEST(is_cached_after_put_success, cache_test_fixture) {
    iobuf buf;
    auto input = make_iobuf_input_stream(std::move(buf));
    sharded_cache.local().put(KEY, input).get();

    auto is_cached = sharded_cache.local().is_cached(KEY).get();

    BOOST_CHECK_EQUAL(is_cached, cache_element_status::available);
}

FIXTURE_TEST(after_invalidate_is_not_cached, cache_test_fixture) {
    auto data_string1 = create_data_string('a', 1_MiB + 1_KiB);
    put_into_cache(data_string1, KEY);

    sharded_cache.local().invalidate(KEY).get();

    auto is_cached = sharded_cache.local().is_cached(KEY).get();
    BOOST_CHECK_EQUAL(is_cached, cache_element_status::not_available);
}

FIXTURE_TEST(invalidate_missing_file_ok, cache_test_fixture) {
    BOOST_CHECK_NO_THROW(sharded_cache.local().invalidate(WRONG_KEY).get());
}

FIXTURE_TEST(empty_cache_nothing_deleted, cache_test_fixture) {
    ss::sleep(ss::lowres_clock::duration(2s)).get();

    BOOST_CHECK_EQUAL(0, sharded_cache.local().get_total_cleaned());
}

FIXTURE_TEST(files_up_to_max_cache_size_not_deleted, cache_test_fixture) {
    auto data_string1 = create_data_string('a', 1_MiB + 1_KiB);
    put_into_cache(data_string1, KEY);

    ss::sleep(ss::lowres_clock::duration(2s)).get();

    BOOST_CHECK_EQUAL(0, sharded_cache.local().get_total_cleaned());
}

FIXTURE_TEST(file_bigger_than_max_cache_size_deleted, cache_test_fixture) {
    auto data_string1 = create_data_string('a', 2_MiB + 1_KiB);
    put_into_cache(data_string1, KEY);

    ss::sleep(ss::lowres_clock::duration(2s)).get();

    BOOST_CHECK_EQUAL(2_MiB + 1_KiB, sharded_cache.local().get_total_cleaned());
}

FIXTURE_TEST(
  files_bigger_than_max_cache_size_oldest_deleted, cache_test_fixture) {
    auto data_string1 = create_data_string('a', 1_MiB + 1_KiB);
    put_into_cache(data_string1, KEY);
    auto data_string2 = create_data_string('b', 1_MiB + 1_KiB);
    ss::sleep(1s).get();
    put_into_cache(data_string1, KEY2);

    ss::sleep(ss::lowres_clock::duration(2s)).get();

    BOOST_CHECK_EQUAL(1_MiB + 1_KiB, sharded_cache.local().get_total_cleaned());
    BOOST_REQUIRE(!ss::file_exists((CACHE_DIR / KEY).native()).get());
    BOOST_REQUIRE(ss::file_exists((CACHE_DIR / KEY2).native()).get());
}

FIXTURE_TEST(cannot_put_tmp_file, cache_test_fixture) {
    auto data_string1 = create_data_string('a', 1_KiB);
    BOOST_CHECK_THROW(
      put_into_cache(data_string1, TEMP_KEY), std::invalid_argument);
}

FIXTURE_TEST(invalidate_cleans_directory, cache_test_fixture) {
    auto data_string1 = create_data_string('a', 1_MiB + 1_KiB);
    const std::filesystem::path unique_prefix_key{
      "unique_prefix/test_topic/test_cache_file.txt"};
    put_into_cache(data_string1, unique_prefix_key);

    sharded_cache.local().invalidate(unique_prefix_key).get();

    BOOST_CHECK(
      !ss::file_exists((CACHE_DIR / unique_prefix_key).native()).get());
    BOOST_CHECK(
      !ss::file_exists((CACHE_DIR / "unique_prefix/test_topic").native())
         .get());
    BOOST_CHECK(!ss::file_exists((CACHE_DIR / "unique_prefix").native()).get());
    BOOST_CHECK(ss::file_exists(CACHE_DIR.native()).get());
}

FIXTURE_TEST(eviction_cleans_directory, cache_test_fixture) {
    auto data_string1 = create_data_string('a', 1_MiB + 1_KiB);
    const std::filesystem::path key1{"a/b/c/first_topic/file1.txt"};
    // this file will be evicted
    put_into_cache(data_string1, key1);

    auto data_string2 = create_data_string('b', 1_MiB + 1_KiB);
    ss::sleep(1s).get();
    const std::filesystem::path key2{"a/b/c/second_topic/file2.txt"};
    // this file will not be evicted
    put_into_cache(data_string2, key2);

    ss::sleep(ss::lowres_clock::duration(2s)).get();

    BOOST_CHECK_EQUAL(1_MiB + 1_KiB, sharded_cache.local().get_total_cleaned());
    BOOST_CHECK(!ss::file_exists((CACHE_DIR / key1).native()).get());
    BOOST_CHECK(
      !ss::file_exists((CACHE_DIR / "a/b/c/first_topic").native()).get());
    BOOST_CHECK(ss::file_exists((CACHE_DIR / "a/b/c").native()).get());
    BOOST_CHECK(ss::file_exists(CACHE_DIR.native()).get());
}

FIXTURE_TEST(invalidate_outside_cache_dir_throws, cache_test_fixture) {
    // make sure the cache directory is empty to get reliable results
    ss::recursive_touch_directory(CACHE_DIR.native()).get();
    auto target_dir = ss::open_directory(CACHE_DIR.native()).get();
    target_dir
      .list_directory([this](const ss::directory_entry& entry) -> ss::future<> {
          auto entry_path = std::filesystem::path(CACHE_DIR)
                            / std::filesystem::path(entry.name);
          return ss::recursive_remove_directory(entry_path.native());
      })
      .done()
      .get();
    target_dir.close().get();

    auto key = std::filesystem::path("../outside_cache/file.txt");
    auto flags = ss::open_flags::wo | ss::open_flags::create
                 | ss::open_flags::exclusive;

    auto dir_path = (CACHE_DIR / key).remove_filename();
    ss::recursive_touch_directory(dir_path.string()).get();
    auto tmp_cache_file
      = ss::open_file_dma((CACHE_DIR / key).native(), flags).get();

    BOOST_CHECK_THROW(
      sharded_cache.local().invalidate(key).get(), std::invalid_argument);
    BOOST_CHECK(ss::file_exists((CACHE_DIR / key).native()).get());
}

FIXTURE_TEST(invalidate_prefix_outside_cache_dir_throws, cache_test_fixture) {
    // make sure the cache directory is empty to get reliable results
    ss::recursive_touch_directory(CACHE_DIR.native()).get();
    auto target_dir = ss::open_directory(CACHE_DIR.native()).get();
    target_dir
      .list_directory([this](const ss::directory_entry& entry) -> ss::future<> {
          auto entry_path = std::filesystem::path(CACHE_DIR)
                            / std::filesystem::path(entry.name);
          return ss::recursive_remove_directory(entry_path.native());
      })
      .done()
      .get();
    target_dir.close().get();

    // CACHE_DIR is "test_cache_dir"
    auto key = std::filesystem::path("../test_cache_dir_bar/file.txt");
    auto flags = ss::open_flags::wo | ss::open_flags::create
                 | ss::open_flags::exclusive;
    auto dir_path = (CACHE_DIR / key).remove_filename();
    ss::recursive_touch_directory(dir_path.string()).get();
    auto tmp_cache_file
      = ss::open_file_dma((CACHE_DIR / key).native(), flags).get();

    BOOST_CHECK_THROW(
      sharded_cache.local().invalidate(key).get(), std::invalid_argument);
    BOOST_CHECK(ss::file_exists((CACHE_DIR / key).native()).get());
}

FIXTURE_TEST(put_outside_cache_dir_throws, cache_test_fixture) {
    // make sure the cache directory is empty to get reliable results
    ss::recursive_touch_directory(CACHE_DIR.native()).get();
    auto target_dir = ss::open_directory(CACHE_DIR.native()).get();
    target_dir
      .list_directory([this](const ss::directory_entry& entry) -> ss::future<> {
          auto entry_path = std::filesystem::path(CACHE_DIR)
                            / std::filesystem::path(entry.name);
          return ss::recursive_remove_directory(entry_path.native());
      })
      .done()
      .get();
    target_dir.close().get();

    // CACHE_DIR is "test_cache_dir"
    auto key = std::filesystem::path("../test_cache_dir_put/file.txt");
    auto data_string1 = create_data_string('a', 1_MiB + 1_KiB);
    iobuf buf;
    buf.append(data_string1.data(), data_string1.length());
    auto input = make_iobuf_input_stream(std::move(buf));

    BOOST_CHECK_EXCEPTION(
      sharded_cache.local().put(key, input).get(),
      std::invalid_argument,
      [](const std::invalid_argument& e) {
          return std::string(e.what()).find(
                   "test_cache_dir_put/file.txt, which is outside of cache_dir")
                 != std::string::npos;
      });
    BOOST_CHECK(!ss::file_exists((CACHE_DIR / key).native()).get());
}

static std::chrono::system_clock::time_point make_ts(int64_t val) {
    auto seconds = std::chrono::seconds(val);
    return std::chrono::system_clock::time_point(seconds);
}

SEASTAR_THREAD_TEST_CASE(test_access_time_tracker) {
    access_time_tracker cm;

    std::vector<std::chrono::system_clock::time_point> timestamps = {
      make_ts(1653000000),
      make_ts(1653000001),
      make_ts(1653000002),
      make_ts(1653000003),
      make_ts(1653000004),
      make_ts(1653000005),
      make_ts(1653000006),
      make_ts(1653000007),
      make_ts(1653000008),
      make_ts(1653000009),
    };

    std::vector<std::string_view> names = {
      "key0",
      "key1",
      "key2",
      "key3",
      "key4",
      "key5",
      "key6",
      "key7",
      "key8",
      "key9",
    };

    for (int i = 0; i < 10; i++) {
        cm.add_timestamp(names[i], timestamps[i]);
    }

    for (int i = 0; i < 10; i++) {
        auto ts = cm.estimate_timestamp(names[i]);
        BOOST_REQUIRE(ts.value() >= timestamps[i]);
    }
}

SEASTAR_THREAD_TEST_CASE(test_access_time_tracker_serializer) {
    access_time_tracker in;

    std::vector<std::chrono::system_clock::time_point> timestamps = {
      make_ts(1653000000),
      make_ts(1653000001),
      make_ts(1653000002),
      make_ts(1653000003),
      make_ts(1653000004),
      make_ts(1653000005),
      make_ts(1653000006),
      make_ts(1653000007),
      make_ts(1653000008),
      make_ts(1653000009),
    };

    std::vector<std::string_view> names = {
      "key0",
      "key1",
      "key2",
      "key3",
      "key4",
      "key5",
      "key6",
      "key7",
      "key8",
      "key9",
    };

    for (int i = 0; i < 10; i++) {
        in.add_timestamp(names[i], timestamps[i]);
    }

    access_time_tracker out;
    out.from_iobuf(in.to_iobuf());

    for (int i = 0; i < 10; i++) {
        auto ts = out.estimate_timestamp(names[i]);
        BOOST_REQUIRE(ts.value() >= timestamps[i]);
    }
}
